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
#include <efi.h>
#include <memory.h>
#include <core.h>
#include <csm/csm.h>
#include <csm/csm_boot.h>
#include <csm/e820.h>
#include <hw/intel_pit.h>
#include <hw/ioapic.h>
#include <hw/lapic_legacy.h>
#include <hw/pci_cfg.h>
#include <lib/heap.h>
#include <lib/log.h>
#include <sys/mm.h>
#include <virt/gpt.h>
#include <virt/hypercall.h>
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
// memory. The legacy ROM area is the hypervisor's RAM, as the host's need not
// be RAM at all.
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
#define ROM_AREA_END 0x100000
#define OPTION_ROM_ALIGN 2048

// Well above the 2 MiB the CSM16 keeps after boot, the rest being its scratch
// memory while it starts up.
#define HIGH_PMM_SIZE (8u << 20)
#define HIGH_PMM_ALIGN 0x10000
#define FOUR_GIB 0x100000000ull

#define SMBIOS2_TABLE_LENGTH 0x16

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
    uint8_t *rom_area;
    struct csm_compat16_table *compat16;
    uint32_t image_base;
    uint32_t bbs_count;
    // Option ROMs handled so far, the VBIOS coming first when there is one.
    uint32_t roms_done;
    // Where the ROM being run sits, and where the next one goes.
    uint32_t rom_at;
    uint32_t rom_end;
} csm;

static void *guest_low(uint32_t gpa) {
    if (gpa >= ROM_AREA_BASE && gpa < ROM_AREA_END) {
        return csm.rom_area + (gpa - ROM_AREA_BASE);
    }
    return (void *)(uintptr_t)gpa;
}

// The top of the highest free RAM below 4 GiB that holds it, which the guest's
// memory map then shows as RAM, as the CSM16 expects.
static uint32_t high_pmm_find(void) {
    const struct handoff *h = core_handoff;
    uint64_t best = 0;

    for (uint64_t off = 0; off + h->efi_mmap_desc_size <= h->efi_mmap_size;
         off += h->efi_mmap_desc_size) {
        const EFI_MEMORY_DESCRIPTOR *desc = (const void *)(uintptr_t)(h->efi_mmap + off);
        if (desc->Type != EfiConventionalMemory) {
            continue;
        }
        uint64_t start = desc->PhysicalStart;
        uint64_t end = start + desc->NumberOfPages * PAGE_SIZE;
        if (end > FOUR_GIB) {
            end = FOUR_GIB;
        }
        if (end <= start || end - start < HIGH_PMM_SIZE) {
            continue;
        }
        uint64_t base = (end - HIGH_PMM_SIZE) & ~(uint64_t)(HIGH_PMM_ALIGN - 1);
        if (base >= start && base > best) {
            best = base;
        }
    }
    return (uint32_t)best;
}

static uint32_t e820_build(void) {
    const struct handoff *h = core_handoff;
    struct e820_map *map = kmalloc(sizeof(*map));

    e820_init(map);
    bool ok = e820_set_efi_map(map, (const void *)(uintptr_t)h->efi_mmap, h->efi_mmap_size,
                               h->efi_mmap_desc_size);
    for (uint32_t i = 0; i < h->reserved_count; i++) {
        ok &= e820_set(map, h->reserved[i].base, h->reserved[i].size, E820_RESERVED);
    }
    ok &= e820_set_low_memory(map);
    if (!ok) {
        printf("The guest's memory map lost ranges that did not fit\n");
    }
    printf("Guest memory map:\n");
    e820_log(map);

    memcpy(guest_low(E820_TABLE), map->entries, map->count * sizeof(struct e820_entry));
    uint32_t count = map->count;
    kfree(map);
    return count;
}

static uint32_t smbios_table_length(void) {
    uint16_t length = 0;
    if (core_handoff->smbios_entry != 0) {
        memcpy(&length, (const void *)(uintptr_t)(core_handoff->smbios_entry
                                                  + SMBIOS2_TABLE_LENGTH), sizeof(length));
    }
    return length;
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

        memcpy(guest_low(csm.rom_end), (const void *)(uintptr_t)rom->image, rom->size);
        struct csm_oprom_params params = {
            .compat16 = csm.compat16,
            .segment = (uint16_t)(csm.rom_end >> 4),
            .address = rom->address,
            .bbs_table = BBS_TABLE,
            .bbs_entry_count = csm.bbs_count,
        };
        csm_fill_oprom_table(guest_low(OPROM_TABLE), &params);
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

    switch (csm.stage) {
        case STAGE_INITIALIZE: {
            printf("Legacy BIOS initialised, status %#x\n", status);
            next_rom(v);
            break;
        }
        case STAGE_DISPATCH_OPROM: {
            // What the ROM keeps once initialised, which may be less than it was.
            uint32_t size = *(uint8_t *)guest_low(csm.rom_at + 2) * 512u;
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
    ioapic_mask_all();
    intel_pit_ungate();

    csm.rom_area = page_alloc((ROM_AREA_END - ROM_AREA_BASE) / PAGE_SIZE);
    gpt_map(ROM_AREA_BASE, (uintptr_t)csm.rom_area, ROM_AREA_END - ROM_AREA_BASE, GPT_RWX, MT_WB);

    uint32_t image_size = (uint32_t)(seabios_csm_end - seabios_csm);
    if (image_size > ROM_AREA_END - ROM_AREA_BASE) {
        panic("The legacy BIOS image is %u bytes, too large\n", image_size);
    }
    csm.image_base = ROM_AREA_END - image_size;
    memcpy(guest_low(csm.image_base), seabios_csm, image_size);
    csm.compat16 = csm_find_compat16_table(guest_low(csm.image_base), image_size);
    if (csm.compat16 == NULL) {
        panic("No CSM16 table in the legacy BIOS image\n");
    }

    uint8_t *thunk = guest_low(THUNK_BASE);
    memcpy(thunk, csm_thunk_start, csm_thunk_end - csm_thunk_start);
    if (!virt_vmx) {
        // VMMCALL in place of VMCALL.
        thunk[csm_thunk_hypercall - csm_thunk_start + 2] = 0xd9;
    }
    uint16_t entry[2] = {csm.compat16->compatibility16_call_offset,
                         csm.compat16->compatibility16_call_segment};
    memcpy(thunk + (csm_thunk_entry - csm_thunk_start), entry, sizeof(entry));

    memset(guest_low(INIT_TABLE), 0, TABLES_END - INIT_TABLE);
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
    if (h->acpi_rsdp != 0 && !csm_copy_rsdp(guest_low(csm.image_base), csm.image_base, image_size,
                                            csm.compat16, (const void *)(uintptr_t)h->acpi_rsdp)) {
        printf("Cannot hand the ACPI RSDP to the legacy BIOS\n");
    }
    csm_compat16_table_checksum(csm.compat16);

    uint32_t high_pmm = high_pmm_find();
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
    csm_fill_init_table(guest_low(INIT_TABLE), &init);

    csm.bbs_count = csm_fill_bbs_table(guest_low(BBS_TABLE), CSM_BBS_MAX_ENTRIES,
                                       h->block_devices, h->block_device_count);
    struct csm_boot_params boot = {
        .rsdp = (uint32_t)h->acpi_rsdp,
        .smbios_entry = (uint32_t)h->smbios_entry,
        .smbios_table_length = smbios_table_length(),
        .bbs_table = BBS_TABLE,
        .bbs_entry_count = csm.bbs_count,
        .e820_entry_count = e820_count,
    };
    csm_fill_boot_table(guest_low(BOOT_TABLE), &boot);

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
