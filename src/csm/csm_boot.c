// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of ReBIOS.
// Copyright (C) 2026 Mintsuki and contributors
//
// ReBIOS is free software: you can redistribute it and/or modify it under the
// terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version.
//
// ReBIOS is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
// FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
// details.
//
// You should have received a copy of the GNU General Public License along
// with ReBIOS. If not, see <https://www.gnu.org/licenses/>.

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <memory.h>
#include <core.h>
#include <csm/csm.h>
#include <csm/csm_boot.h>
#include <csm/e820.h>
#include <hw/intel_pit.h>
#include <hw/lapic_legacy.h>
#include <hw/pci_cfg.h>
#include <iommu/dma.h>
#include <lib/heap.h>
#include <lib/log.h>
#include <sys/mm.h>
#include <virt/guest_acpi.h>
#include <virt/guest_mem.h>
#include <virt/hypercall.h>
#include <virt/layout.h>
#include <virt/vcpu.h>
#include <virt/virt.h>

extern const uint8_t seabios_csm[];
extern const uint8_t seabios_csm_end[];
extern const uint8_t csm_thunk_start[];
extern const uint8_t csm_thunk_hypercall[];
extern const uint8_t csm_thunk_halt[];
extern const uint8_t csm_thunk_entry[];
extern const uint8_t csm_thunk_end[];

// The guest's first MiB while it runs the CSM16. The thunk and its stack sit
// between the CSM16's stack, which grows down from 0x7000, and where boot
// sectors load, at 0x7c00; the tables follow, then the CSM16's temporary low
// memory.
#define THUNK_BASE 0x7000
#define THUNK_STACK_TOP 0x7c00
#define INIT_TABLE 0x8000
#define OPROM_TABLE 0x8100
#define E820_TABLE 0x8400
#define BBS_TABLE 0x9000
#define BOOT_TABLE 0xa000
#define TABLES_END 0x10000
#define LOW_PMM_BASE 0x10000
#define LOW_PMM_SIZE 0x80000
#define ROM_AREA_BASE 0xc0000
#define BIOS_BASE 0xf0000
#define ROM_AREA_END 0x100000
#define OPTION_ROM_ALIGN 2048

// Well above the 2 MiB the CSM16 keeps after boot, the rest being its scratch
// memory while it starts up.
#define HIGH_PMM_SIZE (8u << 20)
#define HIGH_PMM_ALIGN 0x10000
#define FOUR_GIB 0x100000000ull

// The 2.x SMBIOS entry point's fields. The structure table goes right after
// the longest entry point there is.
#define SMBIOS2_CHECKSUM 0x04
#define SMBIOS2_LENGTH 0x05
#define SMBIOS2_INTERMEDIATE 0x10
#define SMBIOS2_INTERMEDIATE_SIZE 0x0f
#define SMBIOS2_INTERMEDIATE_CHECKSUM 0x15
#define SMBIOS2_TABLE_LENGTH 0x16
#define SMBIOS2_TABLE_ADDRESS 0x18
#define SMBIOS2_ENTRY_SPACE 0x20

_Static_assert(E820_TABLE + E820_MAX_ENTRIES * sizeof(struct e820_entry) <= BBS_TABLE,
               "E820 table overlaps the BBS table");
_Static_assert(BBS_TABLE + CSM_BBS_MAX_ENTRIES * sizeof(struct csm_bbs_entry) <= BOOT_TABLE,
               "BBS table overlaps the boot table");
_Static_assert(BOOT_TABLE + sizeof(struct csm_boot_table) <= TABLES_END,
               "boot table overlaps the low PMM memory");

enum csm_stage {
    STAGE_INITIALIZE,
    STAGE_DISPATCH_OPROM,
    STAGE_UPDATE_BBS,
    STAGE_PREPARE_TO_BOOT,
    STAGE_BOOT,
};

static struct {
    enum csm_stage stage;
    struct csm_compat16_table *compat16;
    uint32_t image_base;
    uint32_t bbs_count;
    // Option ROMs handled so far, the VBIOS coming first when there is one.
    uint32_t roms_done;
    // Where the ROM being run sits, and where the next one goes.
    uint32_t rom_at;
    uint32_t rom_end;
} csm;

static uint32_t e820_build(void) {
    struct e820_map *map = kmalloc(sizeof(*map));

    if (!layout_e820(map)) {
        printf("The guest's memory map lost ranges that did not fit\n");
    }
    printf("Guest memory map:\n");
    e820_log(map);

    memcpy(layout_low(E820_TABLE), map->entries, map->count * sizeof(struct e820_entry));
    uint32_t count = map->count;
    kfree(map);
    return count;
}

static uint8_t byte_sum(const uint8_t *p, size_t size) {
    uint8_t sum = 0;
    for (size_t i = 0; i < size; i++) {
        sum += p[i];
    }
    return sum;
}

// The firmware's SMBIOS tables need not be in memory the guest sees, so the
// guest gets a copy, at the top of its RAM. Returns the copy's entry point.
static uint32_t smbios_copy(uint16_t *table_length) {
    *table_length = 0;
    if (core_handoff->smbios_entry == 0) {
        return 0;
    }
    const uint8_t *entry = (const void *)(uintptr_t)core_handoff->smbios_entry;
    uint8_t entry_length = entry[SMBIOS2_LENGTH];
    uint16_t length;
    uint32_t table;
    memcpy(&length, entry + SMBIOS2_TABLE_LENGTH, sizeof(length));
    memcpy(&table, entry + SMBIOS2_TABLE_ADDRESS, sizeof(table));
    if (entry_length > SMBIOS2_ENTRY_SPACE) {
        printf("SMBIOS entry point of %u bytes\n", entry_length);
        return 0;
    }

    uint32_t size = SMBIOS2_ENTRY_SPACE + length;
    uint32_t base = layout_take(size, 16, E820_RESERVED);
    if (base == 0) {
        printf("No room in the guest's memory for the SMBIOS tables\n");
        return 0;
    }
    uint8_t *copy = kmalloc(size);
    memcpy(copy, entry, entry_length);
    memcpy(copy + SMBIOS2_ENTRY_SPACE, (const void *)(uintptr_t)table, length);
    uint32_t guest_table = base + SMBIOS2_ENTRY_SPACE;
    memcpy(copy + SMBIOS2_TABLE_ADDRESS, &guest_table, sizeof(guest_table));
    copy[SMBIOS2_INTERMEDIATE_CHECKSUM] = 0;
    copy[SMBIOS2_INTERMEDIATE_CHECKSUM] =
        (uint8_t)-byte_sum(copy + SMBIOS2_INTERMEDIATE, SMBIOS2_INTERMEDIATE_SIZE);
    copy[SMBIOS2_CHECKSUM] = 0;
    copy[SMBIOS2_CHECKSUM] = (uint8_t)-byte_sum(copy, entry_length);
    guest_phys_write(base, copy, size);
    kfree(copy);
    *table_length = length;
    return base;
}

// The upper memory no option ROM or the CSM16 uses reads as absent, as on a
// PC whose BIOS shadows only its ROMs, for memory managers to map their own
// there. The CSM16 marks where its data begins with an option ROM header.
static void free_uma_absent(void) {
    uint32_t end = csm.rom_end;
    while (end < BIOS_BASE) {
        const uint8_t *header = layout_low(end);
        if (header[0] == 0x55 && header[1] == 0xaa) {
            break;
        }
        end += OPTION_ROM_ALIGN;
    }
    if (end >= BIOS_BASE) {
        printf("No end found to the free upper memory, keeping it as RAM\n");
        return;
    }
    uint32_t start = (csm.rom_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    end &= ~(PAGE_SIZE - 1);
    if (start < end) {
        printf("Upper memory %#x-%#x is free, and absent\n", start, end - 1);
        layout_absent(start, end - start);
    }
}

// Sets up the call the thunk makes next: function in AX, its table in ES:BX.
static void csm_call(struct vcpu *v, uint16_t function, uint32_t table) {
    struct segment es = {(uint16_t)(table >> 4), 0x93, 0xffff, table & ~0xfu};
    vcpu_set_seg(v, SEG_ES, &es);
    v->gprs.rax = function;
    v->gprs.rbx = table & 0xf;
    vcpu_set_rip(v, 0);
}

// The CSM16 learns where the boot table is here, before PrepareToBoot uses it.
static void update_bbs(struct vcpu *v) {
    csm.stage = STAGE_UPDATE_BBS;
    csm_call(v, CSM_COMPAT16_UPDATE_BBS, BOOT_TABLE);
}

// Runs the next option ROM that fits, or moves on to the boot.
static void next_rom(struct vcpu *v) {
    const struct handoff *h = core_handoff;
    uint32_t rom_count = h->pci_rom_count + (h->video.vbios_present ? 1 : 0);
    // SeaBIOS lets option ROMs grow up to where its own data begins.
    uint32_t rom_max = csm.compat16->uma_address;
    if (rom_max == 0 || rom_max > csm.image_base) {
        rom_max = csm.image_base;
    }

    for (; csm.roms_done < rom_count; csm.roms_done++) {
        const struct handoff_pci_rom *rom;
        if (h->video.vbios_present && csm.roms_done == 0) {
            rom = &h->video.vbios;
        } else {
            rom = &h->pci_roms[csm.roms_done - (h->video.vbios_present ? 1 : 0)];
        }

        if (rom->address.segment != 0) {
            printf("Not running the option ROM of %04x:%02x:%02x.%x, outside segment 0\n",
                   rom->address.segment, rom->address.bus, rom->address.devfn >> 3,
                   rom->address.devfn & 7);
            continue;
        }
        if (csm.rom_end > rom_max || rom->size > rom_max - csm.rom_end) {
            printf("No room for the option ROM of %02x:%02x.%x, %u bytes at %#x\n",
                   rom->address.bus, rom->address.devfn >> 3, rom->address.devfn & 7, rom->size,
                   csm.rom_end);
            continue;
        }

        memcpy(layout_low(csm.rom_end), (const void *)(uintptr_t)rom->image, rom->size);
        struct csm_oprom_params params = {
            .compat16 = csm.compat16,
            .segment = (uint16_t)(csm.rom_end >> 4),
            .address = rom->address,
            .bbs_table = BBS_TABLE,
            .bbs_entry_count = csm.bbs_count,
        };
        csm_fill_oprom_table(layout_low(OPROM_TABLE), &params);
        printf("Running the option ROM of %02x:%02x.%x (%04x:%04x) at %#x\n", rom->address.bus,
               rom->address.devfn >> 3, rom->address.devfn & 7, rom->vendor_id, rom->device_id,
               csm.rom_end);
        csm.rom_at = csm.rom_end;
        csm.stage = STAGE_DISPATCH_OPROM;
        csm_call(v, CSM_COMPAT16_DISPATCH_OPROM, OPROM_TABLE);
        return;
    }

    update_bbs(v);
}

void csm_boot_returned(struct vcpu *v) {
    uint16_t status = (uint16_t)v->gprs.rsi;
    dma_report_faults();

    switch (csm.stage) {
        case STAGE_INITIALIZE: {
            printf("Legacy BIOS initialised, status %#x\n", status);
            next_rom(v);
            break;
        }
        case STAGE_DISPATCH_OPROM: {
            // What the ROM keeps once initialised, which may be less than it was.
            uint32_t size = *(uint8_t *)layout_low(csm.rom_at + 2) * 512u;
            printf("Option ROM done, status %#x, %u bytes kept\n", status, size);
            csm.rom_end = (csm.rom_at + size + OPTION_ROM_ALIGN - 1) & ~(OPTION_ROM_ALIGN - 1);
            csm.roms_done++;
            next_rom(v);
            break;
        }
        case STAGE_UPDATE_BBS: {
            printf("Legacy BIOS has the BBS table, status %#x\n", status);
            csm.stage = STAGE_PREPARE_TO_BOOT;
            csm_call(v, CSM_COMPAT16_PREPARE_TO_BOOT, BOOT_TABLE);
            break;
        }
        case STAGE_PREPARE_TO_BOOT: {
            printf("Legacy BIOS ready to boot, status %#x\n", status);
            free_uma_absent();
            csm.stage = STAGE_BOOT;
            csm_call(v, CSM_COMPAT16_BOOT, 0);
            break;
        }
        case STAGE_BOOT: {
            printf("The legacy BIOS found nothing to boot, status %#x\n", status);
            vcpu_set_rip(v, csm_thunk_halt - csm_thunk_start);
            break;
        }
    }
}

void csm_boot_start(struct vcpu *v) {
    const struct handoff *h = core_handoff;

    lapic_legacy_init();
    intel_pit_ungate();

    uint32_t image_size = (uint32_t)(seabios_csm_end - seabios_csm);
    if (image_size > ROM_AREA_END - ROM_AREA_BASE) {
        panic("The legacy BIOS image is %u bytes, too large\n", image_size);
    }
    csm.image_base = ROM_AREA_END - image_size;
    memcpy(layout_low(csm.image_base), seabios_csm, image_size);
    csm.compat16 = csm_find_compat16_table(layout_low(csm.image_base), image_size);
    if (csm.compat16 == NULL) {
        panic("No CSM16 table in the legacy BIOS image\n");
    }

    uint8_t *thunk = layout_low(THUNK_BASE);
    memcpy(thunk, csm_thunk_start, csm_thunk_end - csm_thunk_start);
    if (!virt_vmx) {
        // VMMCALL in place of VMCALL.
        thunk[csm_thunk_hypercall - csm_thunk_start + 2] = 0xd9;
    }
    uint16_t entry[2] = {csm.compat16->compatibility16_call_offset,
                         csm.compat16->compatibility16_call_segment};
    memcpy(thunk + (csm_thunk_entry - csm_thunk_start), entry, sizeof(entry));

    memset(layout_low(INIT_TABLE), 0, TABLES_END - INIT_TABLE);
    // The copies come before the map that shows them.
    uint8_t rsdp[GUEST_ACPI_RSDP_SIZE];
    bool acpi = guest_acpi_build(rsdp);
    uint16_t smbios_length;
    uint32_t smbios = smbios_copy(&smbios_length);
    uint32_t e820_count = e820_build();
    csm.compat16->e820_pointer = E820_TABLE;
    csm.compat16->e820_length = e820_count * sizeof(struct e820_entry);
    uint64_t ecam = pci_cfg_ecam_base(0, 0);
    if (ecam < FOUR_GIB) {
        csm.compat16->pci_express_base = (uint32_t)ecam;
    }
    if (csm.compat16->table_length >= sizeof(struct csm_compat16_table)) {
        csm.compat16->hypervisor_interface = HYPERCALL_INTERFACE;
    } else {
        printf("The legacy BIOS predates the hypervisor's interface\n");
    }
    if (acpi && !csm_copy_rsdp(layout_low(csm.image_base), csm.image_base, image_size,
                               csm.compat16, rsdp)) {
        printf("Cannot hand the ACPI RSDP to the legacy BIOS\n");
        acpi = false;
    }
    csm_compat16_table_checksum(csm.compat16);

    uint32_t high_pmm = layout_take(HIGH_PMM_SIZE, HIGH_PMM_ALIGN, E820_RAM);
    if (high_pmm == 0) {
        panic("No room below 4 GiB for the legacy BIOS's memory\n");
    }
    struct csm_init_params init = {
        .low_pmm = LOW_PMM_BASE,
        .low_pmm_size = LOW_PMM_SIZE,
        .high_pmm = high_pmm,
        .high_pmm_size = HIGH_PMM_SIZE,
        .thunk = THUNK_BASE,
        .thunk_size = TABLES_END - THUNK_BASE,
    };
    csm_fill_init_table(layout_low(INIT_TABLE), &init);

    csm.bbs_count = csm_fill_bbs_table(layout_low(BBS_TABLE), CSM_BBS_MAX_ENTRIES,
                                       h->block_devices, h->block_device_count);
    struct csm_boot_params boot = {
        .rsdp = acpi ? csm.compat16->acpi_rsd_ptr_pointer : 0,
        .smbios_entry = smbios,
        .smbios_table_length = smbios_length,
        .bbs_table = BBS_TABLE,
        .bbs_entry_count = csm.bbs_count,
        .e820_entry_count = e820_count,
    };
    csm_fill_boot_table(layout_low(BOOT_TABLE), &boot);

    printf("Legacy BIOS at %#x, %u bytes, entry %04x:%04x, high memory at %#x\n",
           csm.image_base, image_size, entry[1], entry[0], high_pmm);

    vcpu_set_real_mode(v, THUNK_BASE >> 4, 0);
    struct segment stack = {THUNK_BASE >> 4, 0x93, 0xffff, THUNK_BASE};
    vcpu_set_seg(v, SEG_SS, &stack);
    vcpu_set_rsp(v, THUNK_STACK_TOP - THUNK_BASE);
    csm.rom_end = ROM_AREA_BASE;
    csm.stage = STAGE_INITIALIZE;
    csm_call(v, CSM_COMPAT16_INITIALIZE_YOURSELF, INIT_TABLE);
}
