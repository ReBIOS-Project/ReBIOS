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
// You should have received a copy of the GNU General Public License along with
// ReBIOS. If not, see <https://www.gnu.org/licenses/>.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <efi.h>
#include <hw/pci_assign.h>
#include <core.h>
#include <handoff.h>
#include <hw/pci_cfg.h>
#include <hw/pci_enum.h>
#include <hw/pci_host.h>
#include <hw/pci_regs.h>
#include <hw/smbios.h>
#include <lib/heap.h>
#include <lib/log.h>
#include <lib/printf.h>
#include <lib/range.h>
#include <lib/util.h>
#include <sys/mm.h>

// A BAR the firmware left at 0 is one that an OS assigns itself, as Linux
// 7.3's pcibios_allocate_dev_resources() leaves such BARs to
// pci_assign_unassigned_root_bus_resources(). The guest's BARs are ReBIOS's
// own, so ReBIOS gives these their host addresses in the guest's place.

#define BAR_MEMORY_ADDRESS ((uint32_t)0xfffffff0)
// The group bit and member index of a function's expansion ROM among its BARs.
#define ROM_MEMBER 6
#define MEMBERS_MAX 7

// Why a BAR at 0 gets an address.
#define UNASSIGNED "left unassigned by the firmware"

// The firmware's memory map as Linux 7.3 makes an E820 table of it
// (drivers/firmware/efi/libstub/x86-stub.c, setup_e820()): the descriptors of
// the types it knows, each given an E820 type, adjoining ones of a type
// merged; without the EfiMemoryMappedIO descriptors of 256 KiB or more, which
// its efi_remove_e820_mmio() drops, as firmware describes PCI host bridge
// windows with them.
#define E820_RAM 1
#define E820_RESERVED 2
#define E820_ACPI 3
#define E820_NVS 4
#define E820_UNUSABLE 5
#define E820_PMEM 7
#define E820_MMIO_DROPPED 0x40000

// What Linux 7.3's arch_remove_reservations() keeps out of every free range
// that it allocates from: the BIOS area, the top 2 MiB below 4 GiB.
#define BIOS_ROM_BASE ((uintptr_t)0xffe00000)

// Linux 7.3's E820 clipping of the free ranges in host bridge windows applies
// to firmware from before this year only (arch/x86/pci/acpi.c,
// pci_acpi_crs_quirks()), as later firmware's reserved ranges may cover whole
// windows, which then leave no room.
#define E820_CLIP_YEAR_END 2023

struct e820_entry {
    uint64_t base;
    uint64_t end;
    uint8_t type;
};

// The machines that Linux 7.3's pci_crs_quirks has set_no_e820() for, whose
// reserved ranges cover whole windows, matched as its DMI_MATCH() matches. An
// empty string matches anything.
struct no_e820_quirk {
    const char *sys_vendor;
    const char *product_name;
    const char *product_version;
    const char *board_name;
};

static const struct no_e820_quirk no_e820_quirks[] = {
    { "LENOVO", "", "IIL", "" },
    { "Acer", "Spin SP513-54N", "", "" },
    { "", "", "", "X170KM-G" }
};

static struct e820_entry *e820;
static size_t e820_count;
static bool e820_clips;

// An E820 type for the descriptor, or 0 for one that Linux 7.3 leaves out.
static uint8_t e820_type(const struct efi_memory_descriptor *d, uint64_t size) {
    switch (d->type) {
        case EFI_MEMORY_MAPPED_IO: {
            return size >= E820_MMIO_DROPPED ? 0 : E820_RESERVED;
        }
        case EFI_RESERVED_MEMORY_TYPE:
        case EFI_RUNTIME_SERVICES_CODE:
        case EFI_RUNTIME_SERVICES_DATA:
        case EFI_MEMORY_MAPPED_IO_PORT_SPACE:
        case EFI_PAL_CODE: {
            return E820_RESERVED;
        }
        case EFI_UNUSABLE_MEMORY: {
            return E820_UNUSABLE;
        }
        case EFI_ACPI_RECLAIM_MEMORY: {
            return E820_ACPI;
        }
        case EFI_LOADER_CODE:
        case EFI_LOADER_DATA:
        case EFI_BOOT_SERVICES_CODE:
        case EFI_BOOT_SERVICES_DATA:
        case EFI_CONVENTIONAL_MEMORY:
        case EFI_UNACCEPTED_MEMORY_TYPE: {
            return E820_RAM;
        }
        case EFI_ACPI_MEMORY_NVS: {
            return E820_NVS;
        }
        case EFI_PERSISTENT_MEMORY: {
            return E820_PMEM;
        }
        default: {
            return 0;
        }
    }
}

static bool e820_clips_decide(void) {
    if (core_handoff->bios_year >= E820_CLIP_YEAR_END) {
        printf(
            "PCI: the firmware's memory map does not clip windows, as the firmware is from %u\n",
            core_handoff->bios_year
        );
        return false;
    }
    for (size_t i = 0; i < ARRAY_COUNT(no_e820_quirks); i++) {
        const struct no_e820_quirk *q = &no_e820_quirks[i];
        if (smbios_dmi_contains(core_handoff->dmi_sys_vendor, q->sys_vendor)
         && smbios_dmi_contains(core_handoff->dmi_product_name, q->product_name)
         && smbios_dmi_contains(core_handoff->dmi_product_version, q->product_version)
         && smbios_dmi_contains(core_handoff->dmi_board_name, q->board_name)) {
            printf(
                "PCI: the firmware's memory map does not clip windows, as Linux 7.3 has it for "
                "this machine\n"
            );
            return false;
        }
    }
    return true;
}

// The E820 table, sorted and merged as Linux 7.3's e820__update_table() has
// it, made once.
static void e820_make(void) {
    if (e820 != NULL) {
        return;
    }
    size_t capacity = 0;
    for (size_t i = 0; ; i++) {
        const struct efi_memory_descriptor *d = handoff_efi_desc(core_handoff, i);
        if (d == NULL) {
            break;
        }
        uint64_t d_end;
        if (!handoff_efi_desc_end(d, &d_end) || d_end == d->physical_start) {
            continue;
        }
        uint8_t type = e820_type(d, d_end - d->physical_start);
        if (type == 0) {
            continue;
        }
        e820 = heap_grow(e820, e820_count, &capacity, sizeof(struct e820_entry), 64);
        size_t at = e820_count++;
        while (at > 0 && e820[at - 1].base > d->physical_start) {
            e820[at] = e820[at - 1];
            at--;
        }
        e820[at] = (struct e820_entry){ d->physical_start, d_end, type };
    }
    size_t kept = 0;
    for (size_t i = 0; i < e820_count; i++) {
        if (kept != 0 && e820[kept - 1].type == e820[i].type
         && e820[kept - 1].end >= e820[i].base) {
            if (e820[i].end > e820[kept - 1].end) {
                e820[kept - 1].end = e820[i].end;
            }
            continue;
        }
        e820[kept++] = e820[i];
    }
    e820_count = kept;
    e820_clips = e820_clips_decide();
}

// Takes [base, base + size) out of room, to whole pages.
static void take(struct range_list *room, uintptr_t base, uint64_t size) {
    if (size == 0) {
        return;
    }
    uintptr_t end = size < UINTPTR_MAX - base ? base + size : UINTPTR_MAX;
    if (end <= UINTPTR_MAX - PAGE_SIZE) {
        end = page_up(end);
    }
    range_list_remove(room, page_down(base), end);
}

static void take_ranges(
    struct range_list *room,
    const struct pci_host_range *ranges,
    size_t count
) {
    for (size_t i = 0; i < count; i++) {
        range_list_remove(room, ranges[i].base, ranges[i].end);
    }
}

// How far Linux 7.3's e820__reserve_resources_late() pads the end of RAM
// there, against firmware that lists no stolen RAM.
static uint64_t ram_alignment(uint64_t at) {
    if (at < ONE_MIB) {
        return 0x10000;
    }
    if (at < 16 * ONE_MIB) {
        return ONE_MIB;
    }
    return 64 * ONE_MIB;
}

// Takes out of room, for the window [base, end), what of the E820 table blocks
// Linux 7.3's allocations there: the RAM, ACPI, NVS, and unusable ranges that
// e820__reserve_resources() makes busy, and any range below 1 MiB; the RAM's
// ends padded; and the other ranges that lie within the window, short of all
// of it, which e820__reserve_resources_late() makes its children, where those
// that hold all of it or overlap an end grow to be its parents and block
// nothing (insert_resource_expand_to_fit()).
static void room_take_e820(struct range_list *room, uintptr_t base, uintptr_t end) {
    e820_make();
    for (size_t i = 0; i < e820_count; i++) {
        const struct e820_entry *e = &e820[i];
        bool busy = e->type == E820_RAM || e->type == E820_ACPI || e->type == E820_NVS
                 || e->type == E820_UNUSABLE || e->base < ONE_MIB;
        bool child = e->base >= base && e->end <= end && (e->base != base || e->end != end);
        if (busy || child) {
            take(room, e->base, e->end - e->base);
        }
        if (e->type == E820_RAM) {
            uint64_t padded = align_up(e->end, ram_alignment(e->end));
            take(room, e->end, padded - e->end);
        }
    }
}

// Clips each free range in room around [base, end) as Linux 7.3's
// resource_clip() does, which keeps only the larger part of a range that it
// overlaps, as Linux holds each free range as one.
static void room_clip(struct range_list *room, uint64_t base, uint64_t end) {
    size_t i = 0;
    while (i < room->count) {
        struct range r = room->r[i];
        if (r.end <= base || r.base >= end) {
            i++;
            continue;
        }
        // What is left at i, if anything, overlaps no more.
        uint64_t low = r.base < base ? base - r.base : 0;
        uint64_t high = r.end > end ? r.end - end : 0;
        if (low > high) {
            range_list_remove(room, (uintptr_t)base, r.end);
        } else {
            range_list_remove(room, r.base, end < r.end ? (uintptr_t)end : r.end);
        }
    }
}

// What Linux 7.3's arch_remove_reservations() keeps out of each free range:
// the BIOS area, and, where it clips with the firmware's memory map, each of
// its ranges.
static void room_reserve(struct range_list *room) {
    e820_make();
    room_clip(room, BIOS_ROM_BASE, FOUR_GIB);
    for (size_t i = 0; e820_clips && i < e820_count; i++) {
        room_clip(room, e820[i].base, e820[i].end);
    }
}

// Whether g is a bridge between f and its root bus.
static bool above(const struct pci_enum_function *g, const struct pci_enum_function *f) {
    for (const struct pci_enum_function *p = f->parent; p != NULL; p = p->parent) {
        if (p == g) {
            return true;
        }
    }
    return false;
}

// What a BAR takes on its own: a page at least.
static uint64_t span_of(const struct pci_enum_bar *bar) {
    return bar->size > PAGE_SIZE ? bar->size : PAGE_SIZE;
}

// A member of a group of f's that moves together: one of its BARs by index,
// or, as ROM_MEMBER, its expansion ROM.
static const struct pci_enum_bar *member_of(const struct pci_enum_function *f, size_t index) {
    return index == ROM_MEMBER ? &f->rom : &f->bars[index];
}

static bool assigned(const struct pci_enum_bar *bar) {
    bool memory = bar->kind == PCI_ENUM_BAR_MEM32 || bar->kind == PCI_ENUM_BAR_MEM64;
    return memory && bar->size != 0 && bar->base != 0;
}

// What of the window [base, end) the host has nothing at: no function's BAR,
// no window of a bridge but those above f, which hold it, no memory of ACPI's
// devices or of the firmware's memory map as Linux 7.3 keeps out of a window,
// and not the framebuffer, where they do not move: the BARs of f that moving
// has a bit for, and the framebuffer in them, do.
static void room_find(
    struct range_list *room,
    const struct pci_enum_function *f,
    uint32_t moving,
    uintptr_t base,
    uintptr_t end
) {
    range_list_add(room, base, end);
    range_list_remove(room, 0, ONE_MIB);
    const struct pci_enum_topology *t = pci_enum_topology();
    for (size_t i = 0; i < t->function_count; i++) {
        const struct pci_enum_function *g = &t->functions[i];
        for (size_t b = 0; b < g->bar_count; b++) {
            const struct pci_enum_bar *bar = &g->bars[b];
            if ((bar->kind == PCI_ENUM_BAR_MEM32 || bar->kind == PCI_ENUM_BAR_MEM64)
             && bar->base != 0
             && !(g == f && (moving & (uint32_t)1 << b) != 0)) {
                take(room, bar->base, bar->size);
            }
        }
        if (g->rom.size != 0 && g->rom.base != 0
         && !(g == f && (moving & (uint32_t)1 << ROM_MEMBER) != 0)) {
            take(room, g->rom.base, g->rom.size);
        }
        for (size_t b = 0; b < PCI_SRIOV_NUM_BARS; b++) {
            if (g->vf_bars[b].base != 0) {
                take(room, g->vf_bars[b].base, pci_enum_vf_span(g, b));
            }
        }
        if (g->header_type != PCI_HEADER_TYPE_BRIDGE || above(g, f)) {
            continue;
        }
        if (g->mem_limit >= g->mem_base) {
            take(room, g->mem_base, (uint64_t)g->mem_limit - g->mem_base + 1);
        }
        if (g->pref_limit >= g->pref_base) {
            take(room, g->pref_base, g->pref_limit - g->pref_base + 1);
        }
    }
    const struct pci_host *host = pci_host_scan();
    take_ranges(room, host->interrupts, host->interrupt_count);
    take_ranges(room, host->devices, host->device_count);
    take_ranges(room, host->tpms, host->tpm_count);
    take_ranges(room, host->tables, host->table_count);
    take_ranges(room, host->private, host->private_count);
    take_ranges(room, host->motherboard, host->motherboard_count);
    room_take_e820(room, base, end);
    const struct handoff_framebuffer *fb = &core_handoff->video.framebuffer;
    bool fb_moves = false;
    for (size_t b = 0; b < f->bar_count; b++) {
        const struct pci_enum_bar *bar = &f->bars[b];
        fb_moves = fb_moves
                || ((moving & (uint32_t)1 << b) != 0 && fb->base >= bar->base
                 && fb->base - bar->base < bar->size);
    }
    if (fb->present && !fb_moves) {
        take(room, fb->base, fb->size);
    }
}

// The lowest address in room that holds size bytes at align, each a page at
// least, so that the BAR shares its pages with nothing.
static bool fit(const struct range_list *room, uint64_t size, uint64_t align, uintptr_t *at) {
    uint64_t span = size > PAGE_SIZE ? size : PAGE_SIZE;
    align = align > PAGE_SIZE ? align : PAGE_SIZE;
    for (size_t i = 0; i < room->count; i++) {
        uint64_t start = align_up(room->r[i].base, align);
        if (start >= room->r[i].base && start < room->r[i].end
         && room->r[i].end - start >= span) {
            *at = (uintptr_t)start;
            return true;
        }
    }
    return false;
}

// Looks for an address in the window [base, end) of the bus that f is on, for
// size bytes of the BAR's kind at align.
static bool window_try(
    const struct pci_enum_function *f,
    const struct pci_enum_bar *bar,
    uint64_t size,
    uint64_t align,
    uintptr_t base,
    uintptr_t end,
    uintptr_t *at
) {
    uintptr_t limit = bar->kind == PCI_ENUM_BAR_MEM32 && end > FOUR_GIB ? FOUR_GIB : end;
    if (limit <= base) {
        return false;
    }
    struct range_list room = { 0 };
    room_find(&room, f, 0, base, end);
    range_list_remove(&room, limit, end);
    room_reserve(&room);
    bool found = fit(&room, size, align, at);
    range_list_free(&room);
    return found;
}

static const struct pci_host_bridge *host_bridge_of(const struct pci_enum_function *f) {
    const struct pci_host *host = pci_host_scan();
    for (size_t i = 0; i < host->bridge_count; i++) {
        const struct pci_host_bridge *hb = &host->bridges[i];
        if (hb->segment == f->addr.segment && hb->bus == f->addr.bus) {
            return hb;
        }
    }
    return NULL;
}

// An address for size bytes of the BAR at align in a window of the bus that f
// is on. A prefetchable BAR tries the prefetchable windows first, and one that
// is not prefetchable takes none of them, as Linux 7.3's
// pci_bus_alloc_from_region() has it.
static bool place(
    const struct pci_enum_function *f,
    const struct pci_enum_bar *bar,
    uint64_t size,
    uint64_t align,
    uintptr_t *at
) {
    const struct pci_enum_function *p = f->parent;
    if (p != NULL) {
        if (p->header_type != PCI_HEADER_TYPE_BRIDGE) {
            return false;
        }
        if (bar->prefetchable && p->pref_limit >= p->pref_base
         && window_try(f, bar, size, align, p->pref_base, p->pref_limit + 1, at)) {
            return true;
        }
        return p->mem_limit >= p->mem_base
            && window_try(f, bar, size, align, p->mem_base, (uintptr_t)p->mem_limit + 1, at);
    }
    const struct pci_host_bridge *hb = host_bridge_of(f);
    if (hb == NULL) {
        return false;
    }
    for (size_t pass = 0; pass < 2; pass++) {
        for (size_t i = 0; i < hb->window_count; i++) {
            const struct pci_host_window *w = &hb->windows[i];
            bool wanted = pass == 0
                        ? w->prefetchable == bar->prefetchable
                        : bar->prefetchable && !w->prefetchable;
            if (wanted && window_try(f, bar, size, align, w->base, w->end, at)) {
                return true;
            }
        }
    }
    return false;
}

// Writes the address to the BAR with the function's memory decoding off, so
// that no half-written address decodes, and reads it back. An expansion ROM
// BAR keeps its enable bit, its bits 10:1 being reserved (PCI Local Bus
// Specification 3.0, 6.2.5.2).
static bool program(
    const struct pci_enum_function *f,
    const struct pci_enum_bar *bar,
    uintptr_t at
) {
    uint32_t command = pci_cfg_read(f->addr, PCI_COMMAND, 2);
    bool decoding = (command & PCI_COMMAND_MEMORY) != 0;
    if (decoding) {
        pci_cfg_write(f->addr, PCI_COMMAND, 2, command & ~PCI_COMMAND_MEMORY);
    }
    bool wide = bar->kind == PCI_ENUM_BAR_MEM64;
    uint32_t address = bar == &f->rom ? PCI_ROM_ADDRESS_MASK : BAR_MEMORY_ADDRESS;
    uint32_t kept = bar == &f->rom ? PCI_ROM_ADDRESS_ENABLE : ~BAR_MEMORY_ADDRESS;
    uint32_t type = pci_cfg_read(f->addr, bar->reg, 4) & kept;
    pci_cfg_write(f->addr, bar->reg, 4, ((uint32_t)at & address) | type);
    if (wide) {
        pci_cfg_write(f->addr, bar->reg + 4, 4, (uint32_t)((uint64_t)at >> 32));
    }
    uint64_t read = pci_cfg_read(f->addr, bar->reg, 4) & address;
    if (wide) {
        read |= (uint64_t)pci_cfg_read(f->addr, bar->reg + 4, 4) << 32;
    }
    bool stuck = read == at;
    if (!stuck) {
        pci_cfg_write(f->addr, bar->reg, 4, type);
        if (wide) {
            pci_cfg_write(f->addr, bar->reg + 4, 4, 0);
        }
    }
    if (decoding) {
        pci_cfg_write(f->addr, PCI_COMMAND, 2, command);
    }
    return stuck;
}

// Gives the BAR, of size bytes at align, an address in a window of its bus
// that suits a BAR of kind's kind and prefetchability, saying why it needed
// one.
static void assign(
    struct pci_enum_function *f,
    struct pci_enum_bar *bar,
    const struct pci_enum_bar *kind,
    const char *name,
    const char *why,
    uint64_t size,
    uint64_t align
) {
    uintptr_t at;
    if (!place(f, kind, size, align, &at)) {
        printf(
            "PCI " PCI_CFG_ADDR_FMT " %s: %s, and no window of its bus has room for its 0x%llx "
            "bytes\n",
            PCI_CFG_ADDR_ARGS(f->addr), name, why, (unsigned long long)size
        );
        return;
    }
    if (!program(f, bar, at)) {
        printf(
            "PCI " PCI_CFG_ADDR_FMT " %s: %s, and keeps no address written to it\n",
            PCI_CFG_ADDR_ARGS(f->addr), name, why
        );
        return;
    }
    bar->base = at;
    printf(
        "PCI " PCI_CFG_ADDR_FMT " %s: %s, now at 0x%llx+0x%llx\n",
        PCI_CFG_ADDR_ARGS(f->addr), name, why, (unsigned long long)at, (unsigned long long)size
    );
}

static bool overlaps(uint64_t base, uint64_t last, uint64_t other, uint64_t size) {
    return size != 0 && other <= last && other + size - 1 >= base;
}

// Why the address the firmware left in the ROM of f, the function at index, is
// not one that Linux 7.3 claims for it (arch/x86/pci/i386.c's
// pcibios_allocate_dev_rom_resource()), NULL where it is: in a window of its
// bus, prefetchable or not, as pci_find_parent_resource() takes either for a
// ROM, and clear of every BAR, of the windows of the bridges not above f, and
// of the ROMs before it. Without host bridges known, any window goes.
static const char *rom_unclaimed(const struct pci_enum_function *f, size_t index) {
    uint64_t base = f->rom.base;
    uint64_t last = base + f->rom.size - 1;
    const struct pci_enum_function *p = f->parent;
    bool held = true;
    if (p != NULL) {
        held = p->header_type == PCI_HEADER_TYPE_BRIDGE
            && ((p->mem_limit >= p->mem_base && base >= p->mem_base && last <= p->mem_limit)
             || (p->pref_limit >= p->pref_base && base >= p->pref_base
              && last <= p->pref_limit));
    } else {
        const struct pci_host_bridge *hb = host_bridge_of(f);
        held = hb == NULL;
        for (size_t w = 0; hb != NULL && w < hb->window_count; w++) {
            held = held || (base >= hb->windows[w].base && last < hb->windows[w].end);
        }
    }
    if (!held) {
        return "outside the windows of its bus";
    }
    const char *over = "over another BAR, ROM, or bridge window";
    const struct pci_enum_topology *t = pci_enum_topology();
    for (size_t i = 0; i < t->function_count; i++) {
        const struct pci_enum_function *g = &t->functions[i];
        for (size_t b = 0; b < g->bar_count; b++) {
            const struct pci_enum_bar *bar = &g->bars[b];
            if (assigned(bar) && overlaps(base, last, bar->base, bar->size)) {
                return over;
            }
        }
        if (i < index && g->rom.base != 0 && overlaps(base, last, g->rom.base, g->rom.size)) {
            return over;
        }
        for (size_t b = 0; b < PCI_SRIOV_NUM_BARS; b++) {
            const struct pci_enum_bar *bar = &g->vf_bars[b];
            if (bar->base != 0 && overlaps(base, last, bar->base, pci_enum_vf_span(g, b))) {
                return over;
            }
        }
        if (g->header_type != PCI_HEADER_TYPE_BRIDGE || above(g, f)) {
            continue;
        }
        if ((g->mem_limit >= g->mem_base
          && overlaps(base, last, g->mem_base, (uint64_t)g->mem_limit - g->mem_base + 1))
         || (g->pref_limit >= g->pref_base
          && overlaps(base, last, g->pref_base, g->pref_limit - g->pref_base + 1))) {
            return over;
        }
    }
    return NULL;
}

// Gives the ROM of f, the function at index, an address where the firmware
// left it none that Linux 7.3 claims, as Linux then gives it one, with the ROM
// taken as prefetchable as Linux takes it (drivers/pci/probe.c's
// pci_read_bases()); else it has none.
static void rom_assign(struct pci_enum_function *f, size_t index) {
    struct pci_enum_bar *rom = &f->rom;
    if (rom->kind != PCI_ENUM_BAR_MEM32 || rom->size == 0) {
        return;
    }
    char why[96];
    if (rom->base == 0) {
        snprintf(why, sizeof(why), UNASSIGNED);
    } else {
        const char *unclaimed = rom_unclaimed(f, index);
        if (unclaimed == NULL) {
            return;
        }
        snprintf(
            why, sizeof(why), "left by the firmware at 0x%llx, %s",
            (unsigned long long)rom->base, unclaimed
        );
    }
    rom->base = 0;
    struct pci_enum_bar kind = *rom;
    kind.prefetchable = true;
    assign(f, rom, &kind, "ROM", why, rom->size, rom->size);
}

// A VF BAR spans its VFs' memory from its base, aligned as one VF's BAR is
// (Single Root I/O Virtualization and Sharing Specification 1.1, 3.3.14).
void pci_assign_unassigned_bars(void) {
    const struct pci_enum_topology *t = pci_enum_topology();
    if (t == NULL) {
        return;
    }
    for (size_t i = 0; i < t->function_count; i++) {
        struct pci_enum_function *f = &t->functions[i];
        for (size_t b = 0; b < f->bar_count; b++) {
            struct pci_enum_bar *bar = &f->bars[b];
            bool memory = bar->kind == PCI_ENUM_BAR_MEM32 || bar->kind == PCI_ENUM_BAR_MEM64;
            if (memory && bar->size != 0 && bar->base == 0) {
                char name[16];
                snprintf(name, sizeof(name), "BAR%zu", b);
                assign(f, bar, bar, name, UNASSIGNED, bar->size, bar->size);
            }
        }
        for (size_t b = 0; b < PCI_SRIOV_NUM_BARS; b++) {
            struct pci_enum_bar *bar = &f->vf_bars[b];
            uint64_t span = pci_enum_vf_span(f, b);
            if (span != 0 && bar->base == 0) {
                char name[16];
                snprintf(name, sizeof(name), "VF BAR%zu", b);
                assign(f, bar, bar, name, UNASSIGNED, span, bar->size);
            }
        }
    }
    // A ROM that is off is optional, as Linux 7.3 has it
    // (pci_resource_is_optional()): its address comes after every BAR's.
    for (size_t i = 0; i < t->function_count; i++) {
        rom_assign(&t->functions[i], i);
    }
}

#define WINDOW_MEMORY ((size_t)0)
#define WINDOW_PREF ((size_t)1)
// The bridges above a function that moves, at most.
#define CHAIN_MAX 16

static size_t window_other(size_t which) {
    return which == WINDOW_PREF ? WINDOW_MEMORY : WINDOW_PREF;
}

static bool window_get(
    const struct pci_enum_function *b,
    size_t which,
    uint64_t *base,
    uint64_t *last
) {
    if (which == WINDOW_PREF) {
        *base = b->pref_base;
        *last = b->pref_limit;
    } else {
        *base = b->mem_base;
        *last = b->mem_limit;
    }
    return *last >= *base;
}

// Which window of the bridge holds [base, last], the prefetchable one first,
// false for neither.
static bool window_holding(
    const struct pci_enum_function *b,
    uint64_t base,
    uint64_t last,
    size_t *which
) {
    const size_t tries[] = { WINDOW_PREF, WINDOW_MEMORY };
    for (size_t i = 0; i < ARRAY_COUNT(tries); i++) {
        uint64_t w_base;
        uint64_t w_last;
        if (b->header_type == PCI_HEADER_TYPE_BRIDGE && window_get(b, tries[i], &w_base, &w_last)
         && base >= w_base && last <= w_last) {
            *which = tries[i];
            return true;
        }
    }
    return false;
}

// Whether a window of the bridge holds f's BARs that group has a bit for and
// nothing else but the windows of the bridges between the bridge and f.
static bool window_only(
    const struct pci_enum_function *bridge,
    size_t which,
    const struct pci_enum_function *f,
    uint32_t group
) {
    uint64_t base;
    uint64_t last;
    uint64_t other_base;
    uint64_t other_last;
    if (!window_get(bridge, which, &base, &last)
     || (window_get(bridge, window_other(which), &other_base, &other_last)
      && overlaps(base, last, other_base, other_last - other_base + 1))) {
        return false;
    }
    const struct pci_enum_topology *t = pci_enum_topology();
    for (size_t i = 0; i < t->function_count; i++) {
        const struct pci_enum_function *g = &t->functions[i];
        for (size_t b = 0; b < g->bar_count; b++) {
            const struct pci_enum_bar *bar = &g->bars[b];
            if (!assigned(bar)) {
                continue;
            }
            bool member = g == f && (group & (uint32_t)1 << b) != 0;
            bool inside = bar->base >= base && bar->base + bar->size - 1 <= last;
            if (member ? !inside : overlaps(base, last, bar->base, bar->size)) {
                return false;
            }
        }
        bool rom_member = g == f && (group & (uint32_t)1 << ROM_MEMBER) != 0;
        bool rom_inside = g->rom.base >= base && g->rom.base + g->rom.size - 1 <= last;
        if (g->rom.base != 0
         && (rom_member ? !rom_inside : overlaps(base, last, g->rom.base, g->rom.size))) {
            return false;
        }
        for (size_t b = 0; b < PCI_SRIOV_NUM_BARS; b++) {
            const struct pci_enum_bar *bar = &g->vf_bars[b];
            if (bar->base != 0 && overlaps(base, last, bar->base, pci_enum_vf_span(g, b))) {
                return false;
            }
        }
        bool between = g == bridge || (above(g, f) && above(bridge, g));
        if (g->header_type != PCI_HEADER_TYPE_BRIDGE || between) {
            continue;
        }
        for (size_t w = WINDOW_MEMORY; w <= WINDOW_PREF; w++) {
            if (window_get(g, w, &other_base, &other_last)
             && overlaps(base, last, other_base, other_last - other_base + 1)) {
                return false;
            }
        }
    }
    return true;
}

// Lays the BARs in order out downwards from the top of [begin, end), each as
// high as its alignment lets it go below the one before, at whole bridge
// window granules where bridged: the range they take is [*low, *high).
static bool lay_down(
    const struct pci_enum_function *f,
    const size_t *order,
    size_t count,
    bool bridged,
    uint64_t begin,
    uint64_t end,
    uint64_t *at,
    uint64_t *low,
    uint64_t *high
) {
    uint64_t granule = bridged ? PCI_MEMORY_GRANULE : PAGE_SIZE;
    uint64_t cursor = align_down(end, granule);
    for (size_t k = 0; k < count; k++) {
        uint64_t span = span_of(member_of(f, order[k]));
        if (cursor < begin + span) {
            return false;
        }
        at[k] = align_down(cursor - span, span);
        cursor = at[k];
    }
    *low = align_down(cursor, granule);
    *high = align_up(at[0] + span_of(member_of(f, order[0])), granule);
    return *low >= begin && *high <= end;
}

// A memory or prefetchable window register's word for [low, high), with the
// type bits of the old one.
static uint32_t window_word(uint64_t low, uint64_t high, uint32_t old) {
    return (uint32_t)((low >> 16) & 0xfff0) | (uint32_t)(((high - 1) >> 16) & 0xfff0) << 16
         | (old & 0x000f000f);
}

static void window_write(
    const struct pci_enum_function *p,
    size_t which,
    uint64_t low,
    uint64_t high
) {
    uint32_t reg = which == WINDOW_PREF ? PCI_PREF_MEMORY_BASE : PCI_MEMORY_BASE;
    uint32_t old = pci_cfg_read(p->addr, reg, 4);
    bool wide = which == WINDOW_PREF && (old & 0xf) == PCI_PREF_RANGE_TYPE_64;
    // The upper limit first, as Linux 7.3's pci_setup_bridge_mmio_pref()
    // writes it: a window that was above 4 GiB, its base then above its limit,
    // decodes nothing until the rest of it is in, rather than a range from
    // below 4 GiB to above it.
    if (wide) {
        pci_cfg_write(p->addr, PCI_PREF_LIMIT_UPPER32, 4, 0);
    }
    pci_cfg_write(p->addr, reg, 4, window_word(low, high, old));
    if (wide) {
        pci_cfg_write(p->addr, PCI_PREF_BASE_UPPER32, 4, (uint32_t)(low >> 32));
        pci_cfg_write(p->addr, PCI_PREF_LIMIT_UPPER32, 4, (uint32_t)((high - 1) >> 32));
    }
}

static bool window_took(
    const struct pci_enum_function *p,
    size_t which,
    uint64_t low,
    uint64_t high
) {
    uint32_t reg = which == WINDOW_PREF ? PCI_PREF_MEMORY_BASE : PCI_MEMORY_BASE;
    uint32_t word = pci_cfg_read(p->addr, reg, 4);
    return word == window_word(low, high, word);
}

static void bar_word_write(
    const struct pci_enum_function *f,
    const struct pci_enum_bar *bar,
    uint64_t at
) {
    uint32_t address = bar == &f->rom ? PCI_ROM_ADDRESS_MASK : BAR_MEMORY_ADDRESS;
    uint32_t kept = bar == &f->rom ? PCI_ROM_ADDRESS_ENABLE : ~BAR_MEMORY_ADDRESS;
    uint32_t type = pci_cfg_read(f->addr, bar->reg, 4) & kept;
    pci_cfg_write(f->addr, bar->reg, 4, ((uint32_t)at & address) | type);
    if (bar->kind == PCI_ENUM_BAR_MEM64) {
        pci_cfg_write(f->addr, bar->reg + 4, 4, (uint32_t)(at >> 32));
    }
}

static uint64_t bar_word_read(const struct pci_enum_function *f, const struct pci_enum_bar *bar) {
    uint32_t address = bar == &f->rom ? PCI_ROM_ADDRESS_MASK : BAR_MEMORY_ADDRESS;
    uint64_t at = pci_cfg_read(f->addr, bar->reg, 4) & address;
    if (bar->kind == PCI_ENUM_BAR_MEM64) {
        at |= (uint64_t)pci_cfg_read(f->addr, bar->reg + 4, 4) << 32;
    }
    return at;
}

// Writes the BARs in order at their addresses, and [low, high) to the window
// that kinds names of every bridge above f, with f's memory decoding off, and
// reads them back: where something does not take, what was there goes back.
static bool group_program(
    struct pci_enum_function *f,
    const size_t *order,
    size_t count,
    const uint64_t *at,
    const size_t *kinds,
    uint64_t low,
    uint64_t high
) {
    uint32_t command = pci_cfg_read(f->addr, PCI_COMMAND, 2);
    pci_cfg_write(f->addr, PCI_COMMAND, 2, command & ~PCI_COMMAND_MEMORY);
    bool took = true;
    for (size_t k = 0; k < count; k++) {
        const struct pci_enum_bar *bar = member_of(f, order[k]);
        bar_word_write(f, bar, at[k]);
        took = took && bar_word_read(f, bar) == at[k];
    }
    size_t depth = 0;
    for (struct pci_enum_function *p = f->parent; p != NULL; p = p->parent) {
        window_write(p, kinds[depth], low, high);
        took = took && window_took(p, kinds[depth], low, high);
        depth++;
    }
    if (!took) {
        for (size_t k = 0; k < count; k++) {
            bar_word_write(f, member_of(f, order[k]), member_of(f, order[k])->base);
        }
        depth = 0;
        for (struct pci_enum_function *p = f->parent; p != NULL; p = p->parent) {
            uint64_t old_base;
            uint64_t old_last;
            window_get(p, kinds[depth], &old_base, &old_last);
            window_write(p, kinds[depth], old_base, old_last + 1);
            depth++;
        }
    }
    pci_cfg_write(f->addr, PCI_COMMAND, 2, command);
    if (!took) {
        return false;
    }
    // A Resizable BAR there has room for its size and no more.
    for (size_t k = 0; k < count; k++) {
        struct pci_enum_bar *bar = order[k] == ROM_MEMBER ? &f->rom : &f->bars[order[k]];
        bar->base = at[k];
        bar->room = 0;
    }
    depth = 0;
    for (struct pci_enum_function *p = f->parent; p != NULL; p = p->parent) {
        if (kinds[depth] == WINDOW_PREF) {
            p->pref_base = low;
            p->pref_limit = high - 1;
        } else {
            p->mem_base = low;
            p->mem_limit = (uint32_t)(high - 1);
        }
        depth++;
    }
    return true;
}

bool pci_assign_top(struct pci_enum_function *f, size_t index, uintptr_t *base, uintptr_t *end) {
    if (index >= f->bar_count || !assigned(&f->bars[index])) {
        printf(
            "PCI " PCI_CFG_ADDR_FMT " BAR%zu: no memory that the firmware assigned\n",
            PCI_CFG_ADDR_ARGS(f->addr), index
        );
        return false;
    }

    // What moves with BAR index: on a root bus its function's prefetchable
    // BARs, and behind a bridge its function's BARs in the bridge's window
    // that holds it.
    const struct pci_enum_bar *first = &f->bars[index];
    uint64_t low = first->base;
    uint64_t last = first->base + first->size - 1;
    size_t which;
    uint64_t w_base = 0;
    uint64_t w_last = 0;
    if (f->parent != NULL && window_holding(f->parent, low, last, &which)) {
        window_get(f->parent, which, &w_base, &w_last);
    }
    uint32_t group = 0;
    for (size_t b = 0; b < f->bar_count && b < 6; b++) {
        const struct pci_enum_bar *bar = &f->bars[b];
        bool moving = f->parent == NULL
                    ? b == index || bar->prefetchable
                    : bar->base >= w_base && bar->base + bar->size - 1 <= w_last;
        if (assigned(bar) && moving) {
            group |= (uint32_t)1 << b;
        }
    }
    // Behind a bridge, f's expansion ROM in that window moves with them, such
    // as one that ReBIOS gave an address there.
    const struct pci_enum_bar *rom = &f->rom;
    if (f->parent != NULL && rom->base != 0 && rom->size != 0 && rom->base >= w_base
     && rom->base + rom->size - 1 <= w_last) {
        group |= (uint32_t)1 << ROM_MEMBER;
    }

    // The window of each bridge above f that holds what is below it.
    size_t kinds[CHAIN_MAX];
    size_t depth = 0;
    const struct pci_enum_function *top = f;
    for (const struct pci_enum_function *p = f->parent; p != NULL; p = p->parent) {
        if (!window_holding(p, low, last, &which) || depth == CHAIN_MAX
         || !window_only(p, which, f, group)) {
            printf(
                "PCI " PCI_CFG_ADDR_FMT " BAR%zu: no window of " PCI_CFG_ADDR_FMT " holds its "
                "function's memory alone\n",
                PCI_CFG_ADDR_ARGS(f->addr), index, PCI_CFG_ADDR_ARGS(p->addr)
            );
            return false;
        }
        kinds[depth++] = which;
        window_get(p, which, &low, &last);
        top = p;
    }
    uint64_t high = last + 1;
    bool bridged = top != f;
    if (!bridged) {
        low = UINT64_MAX;
        high = 0;
        for (size_t b = 0; b < f->bar_count; b++) {
            const struct pci_enum_bar *bar = &f->bars[b];
            uint64_t bar_end = page_up(bar->base + bar->size);
            if ((group & (uint32_t)1 << b) != 0) {
                low = page_down(bar->base) < low ? page_down(bar->base) : low;
                high = bar_end > high ? bar_end : high;
            }
        }
    }

    // BAR index first, the others after it, largest first.
    size_t order[MEMBERS_MAX];
    size_t count = 0;
    order[count++] = index;
    for (size_t b = 0; b < MEMBERS_MAX; b++) {
        bool none = b < ROM_MEMBER && b >= f->bar_count;
        if (b == index || none || (group & (uint32_t)1 << b) == 0) {
            continue;
        }
        size_t k = count++;
        while (k > 1 && member_of(f, order[k - 1])->size < member_of(f, b)->size) {
            order[k] = order[k - 1];
            k--;
        }
        order[k] = b;
    }

    // The highest room below 4 GiB in the windows of the root bus's host
    // bridge, apart from the other windows of the bridges above f.
    const struct pci_host_bridge *hb = host_bridge_of(top);
    struct range_list room = { 0 };
    for (size_t i = 0; hb != NULL && i < hb->window_count; i++) {
        const struct pci_host_window *w = &hb->windows[i];
        if (w->base < FOUR_GIB) {
            room_find(&room, f, group, w->base, w->end);
        }
    }
    range_list_remove(&room, FOUR_GIB, UINTPTR_MAX);
    depth = 0;
    for (const struct pci_enum_function *p = f->parent; p != NULL; p = p->parent) {
        uint64_t other_base;
        uint64_t other_last;
        if (window_get(p, window_other(kinds[depth]), &other_base, &other_last)) {
            take(&room, other_base, other_last - other_base + 1);
        }
        depth++;
    }
    room_reserve(&room);
    uint64_t at[MEMBERS_MAX];
    uint64_t new_low = 0;
    uint64_t new_high = 0;
    bool found = false;
    for (size_t r = room.count; r-- > 0 && !found; ) {
        found = lay_down(
            f, order, count, bridged, room.r[r].base, room.r[r].end, at, &new_low, &new_high
        );
    }
    range_list_free(&room);

    // Memory that reaches above 4 GiB takes any room below it; memory below it
    // only room higher up.
    if (found && (high > FOUR_GIB || at[0] > first->base)
     && group_program(f, order, count, at, kinds, new_low, new_high)) {
        low = new_low;
        high = new_high;
    } else if (high > FOUR_GIB) {
        printf(
            "PCI " PCI_CFG_ADDR_FMT " BAR%zu: no room for it below 4 GiB\n",
            PCI_CFG_ADDR_ARGS(f->addr), index
        );
        return false;
    }
    *base = (uintptr_t)low;
    *end = (uintptr_t)high;
    return true;
}
