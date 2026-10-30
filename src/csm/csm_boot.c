// SPDX-License-Identifier: GPL-2.0-only
//
// This file is part of ReBIOS.
// Copyright (C) 2026 Mintsuki and contributors
//
// ReBIOS is free software: you can redistribute it and/or modify it under the
// terms of the GNU General Public License version 2 as published by the Free
// Software Foundation.
//
// ReBIOS is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
// FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
// details.
//
// You should have received a copy of the GNU General Public License along
// with ReBIOS. If not, see <https://www.gnu.org/licenses/>.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <csm/csm_boot.h>
#include <core.h>
#include <csm/csm.h>
#include <csm/e820.h>
#include <csm/mptable.h>
#include <csm/pir.h>
#include <handoff.h>
#include <hw/acpi_irq.h>
#include <hw/intel_pit.h>
#include <hw/pci_cfg.h>
#include <hw/pci_enum.h>
#include <iommu/dma.h>
#include <lib/heap.h>
#include <lib/log.h>
#include <lib/printf.h>
#include <memory.h>
#include <sys/mm.h>
#include <vgabios_info.h>
#include <virt/edid.h>
#include <virt/guest_acpi.h>
#include <virt/guest_mem.h>
#include <virt/hypercall.h>
#include <virt/irq.h>
#include <virt/layout.h>
#include <virt/pci_plan.h>
#include <virt/vcpu.h>
#include <virt/vga.h>
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
#define MP_TABLE 0xd000
#define PIR_TABLE 0xe000
#define LEGACY_TABLE_SIZE 0x1000
#define EXTRA_PCI_ROOT_LIST 0xf000
#define EXTRA_PCI_ROOTS_MAX 255
#define BOOT_ORDER 0xf100
#define TABLES_END 0x10000
#define LOW_PMM_BASE 0x10000
#define LOW_PMM_SIZE 0x80000
#define ROM_AREA_BASE 0xc0000
#define BIOS_BASE 0xf0000
#define ROM_AREA_END 0x100000
#define OPTION_ROM_ALIGN 2048

// Well above the 2 MiB the CSM16 keeps after boot, the rest being its scratch
// memory while it starts up.
#define HIGH_PMM_SIZE ((uint32_t)8 << 20)
#define HIGH_PMM_ALIGN 0x10000

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

_Static_assert(
    E820_TABLE + E820_MAX_ENTRIES * sizeof(struct e820_entry) <= BBS_TABLE,
    "E820 table overlaps the BBS table"
);
_Static_assert(
    BBS_TABLE + CSM_BBS_MAX_ENTRIES * sizeof(struct csm_bbs_entry)
        <= BOOT_TABLE,
    "BBS table overlaps the boot table"
);
_Static_assert(
    BOOT_TABLE + sizeof(struct csm_boot_table) <= MP_TABLE,
    "boot table overlaps the MP table"
);
_Static_assert(
    PIR_TABLE + LEGACY_TABLE_SIZE <= EXTRA_PCI_ROOT_LIST,
    "PCI IRQ routing table overlaps the extra PCI root bus list"
);
_Static_assert(
    EXTRA_PCI_ROOT_LIST + EXTRA_PCI_ROOTS_MAX <= TABLES_END,
    "extra PCI root bus list overlaps the low PMM memory"
);

#define STAGE_INITIALIZE 0
#define STAGE_DISPATCH_OPROM 1
#define STAGE_UPDATE_BBS 2
#define STAGE_PREPARE_TO_BOOT 3
#define STAGE_BOOT 4

static struct {
    int stage;
    struct csm_compat16_table *compat16;
    uintptr_t image_base;
    size_t bbs_count;
    // Option ROMs handled so far, the VBIOS coming first when there is one.
    uint32_t roms_done;
    // Where the ROM being run sits, and where the next one goes.
    uint32_t rom_at;
    uintptr_t rom_end;
} csm;

static uint32_t e820_build(void) {
    struct e820_map *map = kmalloc(sizeof(struct e820_map));

    if (!layout_e820(map)) {
        printf("The guest's memory map lost ranges that did not fit\n");
    }
    printf("Guest memory map:\n");
    e820_log(map);

    memcpy(
        layout_low(E820_TABLE), map->entries,
        map->count * sizeof(struct e820_entry)
    );
    size_t count = map->count;
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
static uint32_t smbios_copy(size_t *table_length) {
    *table_length = 0;
    if (core_handoff->smbios_entry == 0) {
        return 0;
    }
    const uint8_t *entry = (const void *)(uintptr_t)core_handoff->smbios_entry;
    size_t entry_length = entry[SMBIOS2_LENGTH];
    size_t length;
    uintptr_t table;
    memcpy(&length, entry + SMBIOS2_TABLE_LENGTH, sizeof(uint16_t));
    memcpy(&table, entry + SMBIOS2_TABLE_ADDRESS, sizeof(uint32_t));
    if (entry_length > SMBIOS2_ENTRY_SPACE) {
        printf("SMBIOS entry point of %zu bytes\n", entry_length);
        return 0;
    }

    size_t size = SMBIOS2_ENTRY_SPACE + length;
    uintptr_t base = layout_take(size, 16, E820_RESERVED);
    if (base == 0) {
        printf("No room in the guest's memory for the SMBIOS tables\n");
        return 0;
    }
    uint8_t *copy = kmalloc(size);
    memcpy(copy, entry, entry_length);
    memcpy(copy + SMBIOS2_ENTRY_SPACE, (const void *)(uintptr_t)table, length);
    uintptr_t guest_table = base + SMBIOS2_ENTRY_SPACE;
    memcpy(copy + SMBIOS2_TABLE_ADDRESS, &guest_table, sizeof(uint32_t));
    copy[SMBIOS2_INTERMEDIATE_CHECKSUM] = 0;
    copy[SMBIOS2_INTERMEDIATE_CHECKSUM] = (uint8_t)-byte_sum(
        copy + SMBIOS2_INTERMEDIATE, SMBIOS2_INTERMEDIATE_SIZE
    );
    copy[SMBIOS2_CHECKSUM] = 0;
    copy[SMBIOS2_CHECKSUM] = (uint8_t)-byte_sum(copy, entry_length);
    guest_phys_write(base, copy, size);
    kfree(copy);
    *table_length = length;
    return base;
}

// The upper memory no option ROM or the CSM16 uses reads as absent, as on a PC
// whose BIOS shadows only its ROMs, for memory managers to map their own
// there. The CSM16 marks where its data begins with an option ROM header.
static void free_uma_absent(void) {
    uintptr_t end = csm.rom_end;
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
    uintptr_t start = (csm.rom_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    end &= ~(PAGE_SIZE - 1);
    if (start < end) {
        printf(
            "Upper memory %#llx-%#llx is free, and absent\n",
            (unsigned long long)start, (unsigned long long)(end - 1)
        );
        layout_absent(start, end - start);
    }
}

// The option ROMs, and what the CSM16 keeps read-only from the end of its low
// memory zone, which it reports as that of the free upper memory, are locked
// as a BIOS locks its shadow RAM.
static void lock_roms(void) {
    uintptr_t roms_end = (csm.rom_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (roms_end > ROM_AREA_BASE) {
        printf(
            "Option ROMs %#x-%#llx are read-only\n", ROM_AREA_BASE,
            (unsigned long long)(roms_end - 1)
        );
        layout_read_only(ROM_AREA_BASE, roms_end - ROM_AREA_BASE);
    }
    uintptr_t start = csm.compat16->uma_address + csm.compat16->uma_size;
    start = (start + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (start >= roms_end && start < ROM_AREA_END) {
        printf(
            "Legacy BIOS %#llx-%#x is read-only\n", (unsigned long long)start,
            ROM_AREA_END - 1
        );
        layout_read_only(start, ROM_AREA_END - start);
    }
}

// Sets up the call the thunk makes next: function in AX, its table in ES:BX.
static void csm_call(struct vcpu *v, uint16_t function, uintptr_t table) {
    struct segment es = {
        (uint16_t)(table >> 4), 0x93, 0xffff, table & ~(uint32_t)0xf
    };
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

extern const uint8_t vgabios_image[];
extern const uint8_t vgabios_image_end[];

// What the emulated VGA's video BIOS learns of the display it drives
// (vgabios/src/vgabios_info.h).
static void emulated_vbios_info(uint8_t *image, size_t size) {
    const struct handoff_video *video = &core_handoff->video;
    uint16_t at = (uint16_t)(image[VGABIOS_INFO_POINTER]
        | image[VGABIOS_INFO_POINTER + 1] << 8);
    if (at + sizeof(struct vgabios_info) > size) {
        return;
    }
    struct vgabios_info info = { 0 };
    if (video->vram_size != 0) {
        info.flags = VGABIOS_INFO_DISPI;
    }
    uint32_t width;
    uint32_t height;
    vga_screen_size(&width, &height);
    info.max_width = (uint16_t)width;
    info.max_height = (uint16_t)height;
    info.lfb = (uint32_t)vga_lfb();
    // A turned panel's own EDID describes it unturned.
    if (video->framebuffer.rotation == HANDOFF_ROTATE_0) {
        info.edid_size = (uint16_t)video->edid_size;
        memcpy(info.edid, video->edid, video->edid_size);
    }
    // Without one, guests would not know the display's resolution.
    if (info.edid_size == 0 && edid_synthesize(info.edid, width, height)) {
        info.edid_size = EDID_BLOCK_SIZE;
        printf(
            "%s, so the emulated VGA's display reports %ux%u at 60 Hz\n",
            video->edid_size != 0 ? "The panel's EDID describes it unturned"
                : "No EDID from the firmware",
            width, height
        );
    }
    memcpy(image + at, &info, sizeof(struct vgabios_info));
}

// The emulated VGA's video BIOS as an option ROM: padded to the size its
// header gives, with the checksum that makes its bytes add up to 0. It is the
// host's VGA device's, where there is one.
static const struct handoff_pci_rom *emulated_vbios(void) {
    static struct handoff_pci_rom rom;
    if (rom.size != 0) {
        return &rom;
    }
    const struct handoff *h = core_handoff;
    size_t length = (uint32_t)(vgabios_image_end - vgabios_image);
    size_t size = vgabios_image[2] * (uint32_t)512;
    uint8_t *image = page_alloc((size + PAGE_SIZE - 1) / PAGE_SIZE);
    memset(image, 0, size);
    memcpy(image, vgabios_image, length < size ? length : size);
    emulated_vbios_info(image, size);
    uint8_t sum = 0;
    for (size_t i = 0; i + 1 < size; i++) {
        sum = (uint8_t)(sum + image[i]);
    }
    image[size - 1] = (uint8_t)-sum;
    if (h->video.vga_present) {
        rom.address = h->video.vga_device;
        rom.vendor_id = h->video.vga_vendor_id;
        rom.device_id = h->video.vga_device_id;
    }
    rom.base_class = 0x03;
    rom.size = size;
    rom.image = (uint64_t)(uintptr_t)image;
    return &rom;
}

// Runs the next option ROM that fits, or moves on to the boot.
static void next_rom(struct vcpu *v) {
    const struct handoff *h = core_handoff;
    bool video = vga_emulated() || h->video.vbios_present;
    size_t rom_count = h->pci_rom_count + (video ? 1 : 0);
    // SeaBIOS lets option ROMs grow up to where its own data begins.
    uint32_t rom_max = csm.compat16->uma_address;
    if (rom_max == 0 || rom_max > csm.image_base) {
        rom_max = csm.image_base;
    }

    for (; csm.roms_done < rom_count; csm.roms_done++) {
        const struct handoff_pci_rom *rom;
        if (video && csm.roms_done == 0) {
            rom = vga_emulated() ? emulated_vbios() : &h->video.vbios;
        } else {
            rom = &h->pci_roms[csm.roms_done - (video ? 1 : 0)];
        }

        if (rom->address.segment != 0) {
            printf(
                "Not running the option ROM of %04x:%02x:%02x.%x, outside "
                "segment 0\n",
                rom->address.segment, rom->address.bus,
                rom->address.devfn >> 3, rom->address.devfn & 7
            );
            continue;
        }
        if (csm.rom_end > rom_max || rom->size > rom_max - csm.rom_end) {
            printf(
                "No room for the option ROM of %02x:%02x.%x, %u bytes at "
                "%#llx\n",
                rom->address.bus, rom->address.devfn >> 3,
                rom->address.devfn & 7, rom->size,
                (unsigned long long)csm.rom_end
            );
            continue;
        }

        memcpy(
            layout_low(csm.rom_end), (const void *)(uintptr_t)rom->image,
            rom->size
        );
        struct csm_oprom_params params = {
            .compat16 = csm.compat16,
            .segment = (uint16_t)(csm.rom_end >> 4),
            .address = rom->address,
            .bbs_table = BBS_TABLE,
            .bbs_entry_count = csm.bbs_count,
        };
        csm_fill_oprom_table(layout_low(OPROM_TABLE), &params);
        printf(
            "Running the option ROM of %02x:%02x.%x (%04x:%04x) at %#llx\n",
            rom->address.bus, rom->address.devfn >> 3, rom->address.devfn & 7,
            rom->vendor_id, rom->device_id, (unsigned long long)csm.rom_end
        );
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
            // What the ROM keeps once initialised, which may be less than it
            // was.
            size_t size = *(uint8_t *)layout_low(csm.rom_at + 2)
                * (uint32_t)512;
            printf(
                "Option ROM done, status %#x, %zu bytes kept\n", status, size
            );
            csm.rom_end = (csm.rom_at + size + OPTION_ROM_ALIGN - 1)
                & ~(OPTION_ROM_ALIGN - 1);
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
            lock_roms();
            csm.stage = STAGE_BOOT;
            csm_call(v, CSM_COMPAT16_BOOT, 0);
            break;
        }
        case STAGE_BOOT: {
            printf(
                "The legacy BIOS found nothing to boot, status %#x\n", status
            );
            vcpu_set_rip(v, csm_thunk_halt - csm_thunk_start);
            break;
        }
    }
}

#define PCI_INTERRUPT_LINE 0x3c
#define PCI_INTERRUPT_LINE_NONE 0xff

static bool same_function(struct pci_addr a, struct pci_addr b) {
    return a.segment == b.segment && a.bus == b.bus && a.device == b.device
        && a.function == b.function;
}

// In PIC mode, PCI devices interrupt on the ISA IRQs the hypervisor picks,
// which software finds in their Interrupt Line registers, as a BIOS writes
// them: PCI Local Bus Specification 3.0, 6.2.4.
static void pci_interrupts(void) {
    size_t count;
    const struct acpi_irq_pci_route *routes = acpi_irq_pci_routes(&count);
    struct irq_pci_line *lines = kmalloc(count * sizeof(struct irq_pci_line));
    for (size_t i = 0; i < count; i++) {
        lines[i] = (struct irq_pci_line){
            routes[i].gsi, routes[i].level, routes[i].active_low
        };
    }
    irq_route_pci(lines, count, acpi_irq_isa_claimed());
    kfree(lines);

    const struct pci_topology *topology = pci_enum_topology();
    for (size_t i = 0; i < topology->function_count; i++) {
        const struct pci_function *f = &topology->functions[i];
        if (f->interrupt_pin == 0 || f->interrupt_pin > 4) {
            continue;
        }
        uint32_t line = PCI_INTERRUPT_LINE_NONE;
        for (size_t j = 0; j < count; j++) {
            if (same_function(routes[j].addr, f->addr)
             && irq_isa_irq(routes[j].gsi) >= 0) {
                line = (uint32_t)irq_isa_irq(routes[j].gsi);
            }
        }
        pci_cfg_write(f->addr, PCI_INTERRUPT_LINE, 1, line);
    }
}

// The CSM16 copies the list while it initialises. A list too long for the
// tables loses its last lines.
static void boot_order(void) {
    const char *list = core_handoff->boot_order;
    size_t room = TABLES_END - BOOT_ORDER - 1;
    size_t length = 0;
    while (length < HANDOFF_MAX_BOOT_ORDER && list[length] != 0) {
        length++;
    }
    if (length == 0) {
        return;
    }
    if (length > room) {
        length = room;
        while (length > 0 && list[length] != '\n') {
            length--;
        }
    }
    char *to = layout_low(BOOT_ORDER);
    memcpy(to, list, length);
    to[length] = 0;
    csm.compat16->boot_order_pointer = BOOT_ORDER;
}

// The CSM16 scans the hierarchy of bus 0 itself, and the other root buses of
// segment 0 only from this list, while it initialises.
static void extra_pci_roots(void) {
    uint8_t *list = layout_low(EXTRA_PCI_ROOT_LIST);
    size_t count = 0;
    const struct pci_topology *topology = pci_enum_topology();
    for (size_t i = 0; topology != NULL && i < topology->root_bus_count; i++) {
        const struct pci_root_bus *rb = &topology->root_buses[i];
        if (rb->segment == 0 && rb->bus != 0 && count < EXTRA_PCI_ROOTS_MAX) {
            list[count++] = rb->bus;
        }
    }
    csm.compat16->extra_pci_root_list_pointer = EXTRA_PCI_ROOT_LIST;
    csm.compat16->extra_pci_root_list_count = (uint8_t)count;

    if (count == 0) {
        printf("Extra PCI root buses for the legacy BIOS: none\n");
    }
    for (size_t first = 0; first < count; first += 16) {
        char text[16 * 3 + 1];
        size_t length = 0;
        for (size_t i = first; i < count && i < first + 16; i++) {
            length += (size_t)snprintf(
                text + length, sizeof(text) - length, " %02x", list[i]
            );
        }
        printf("Extra PCI root buses for the legacy BIOS:%s\n", text);
    }
}

void csm_boot_start(struct vcpu *v) {
    const struct handoff *h = core_handoff;

    intel_pit_ungate();

    size_t image_size = (uint32_t)(seabios_csm_end - seabios_csm);
    if (image_size > ROM_AREA_END - ROM_AREA_BASE) {
        panic("The legacy BIOS image is %zu bytes, too large\n", image_size);
    }
    csm.image_base = ROM_AREA_END - image_size;
    memcpy(layout_low(csm.image_base), seabios_csm, image_size);
    csm.compat16 = csm_find_compat16_table(
        layout_low(csm.image_base), image_size
    );
    if (csm.compat16 == NULL) {
        panic("No CSM16 table in the legacy BIOS image\n");
    }

    uint8_t *thunk = layout_low(THUNK_BASE);
    memcpy(thunk, csm_thunk_start, csm_thunk_end - csm_thunk_start);
    if (!virt_vmx) {
        // VMMCALL in place of VMCALL.
        thunk[csm_thunk_hypercall - csm_thunk_start + 2] = 0xd9;
    }
    uint16_t entry[2] = {
        csm.compat16->compatibility16_call_offset,
        csm.compat16->compatibility16_call_segment
    };
    memcpy(thunk + (csm_thunk_entry - csm_thunk_start), entry, sizeof(entry));

    memset(layout_low(INIT_TABLE), 0, TABLES_END - INIT_TABLE);
    pci_interrupts();
    // The CSM16 copies them where legacy software looks for them.
    size_t mp_length = mptable_build(
        layout_low(MP_TABLE), LEGACY_TABLE_SIZE, MP_TABLE
    );
    size_t pir_length = pir_build(layout_low(PIR_TABLE), LEGACY_TABLE_SIZE);
    if (pir_length != 0) {
        csm.compat16->irq_routing_table_pointer = PIR_TABLE;
        csm.compat16->irq_routing_table_length = pir_length;
    }
    // The copies come before the map that shows them.
    uint8_t rsdp[GUEST_ACPI_RSDP_SIZE];
    bool acpi = guest_acpi_build(rsdp);
    size_t smbios_length;
    uint32_t smbios = smbios_copy(&smbios_length);
    size_t e820_count = e820_build();
    csm.compat16->e820_pointer = E820_TABLE;
    csm.compat16->e820_length = e820_count * sizeof(struct e820_entry);
    // Where bus 0's would be, as the legacy BIOS takes it.
    const struct pci_plan *plan = pci_plan();
    if (plan != NULL && plan->ecam != 0) {
        uint64_t bus0 = plan->ecam - ((uint64_t)plan->ecam_start << 20);
        csm.compat16->pci_express_base = (uint32_t)bus0;
    }
    if (csm.compat16->table_length
     >= offsetof(struct csm_compat16_table, boot_order_pointer)) {
        csm.compat16->hypervisor_interface = HYPERCALL_INTERFACE;
        extra_pci_roots();
    } else {
        printf("The legacy BIOS predates the hypervisor's interface\n");
    }
    if (csm.compat16->table_length >= sizeof(struct csm_compat16_table)) {
        boot_order();
    } else {
        printf(
            "The legacy BIOS takes no boot order, ordering by controller\n"
        );
    }
    bool rsdp_failed = acpi
        && !csm_copy_rsdp(
            layout_low(csm.image_base), csm.image_base, image_size,
            csm.compat16, rsdp
        );
    if (rsdp_failed) {
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

    csm.bbs_count = csm_fill_bbs_table(
        layout_low(BBS_TABLE), CSM_BBS_MAX_ENTRIES, h->block_devices,
        h->block_device_count
    );
    struct csm_boot_params boot = {
        .rsdp = acpi ? csm.compat16->acpi_rsd_ptr_pointer : 0,
        .smbios_entry = smbios,
        .smbios_table_length = smbios_length,
        .bbs_table = BBS_TABLE,
        .bbs_entry_count = csm.bbs_count,
        .mp_floating_pointer = mp_length != 0 ? MP_TABLE : 0,
        .e820_entry_count = e820_count,
    };
    csm_fill_boot_table(layout_low(BOOT_TABLE), &boot);

    printf(
        "Legacy BIOS at %#llx, %zu bytes, entry %04x:%04x, high memory at "
        "%#x\n",
        (unsigned long long)csm.image_base, image_size, entry[1], entry[0],
        high_pmm
    );

    vcpu_set_real_mode(v, THUNK_BASE >> 4, 0);
    struct segment stack = { THUNK_BASE >> 4, 0x93, 0xffff, THUNK_BASE };
    vcpu_set_seg(v, SEG_SS, &stack);
    vcpu_set_rsp(v, THUNK_STACK_TOP - THUNK_BASE);
    csm.rom_end = ROM_AREA_BASE;
    csm.stage = STAGE_INITIALIZE;
    csm_call(v, CSM_COMPAT16_INITIALIZE_YOURSELF, INIT_TABLE);
}
