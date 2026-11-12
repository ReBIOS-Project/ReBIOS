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
#include <efi.h>
#include <hw/pci_assign.h>
#include <core.h>
#include <handoff.h>
#include <hw/pci_cfg.h>
#include <hw/pci_enum.h>
#include <hw/pci_host.h>
#include <hw/pci_regs.h>
#include <lib/log.h>
#include <lib/range.h>
#include <lib/util.h>
#include <sys/mm.h>

// A BAR the firmware left at 0 is one that an OS assigns itself, as Linux
// 7.3's pcibios_allocate_dev_resources() leaves such BARs to
// pci_assign_unassigned_root_bus_resources(). The guest's BARs are ReBIOS's
// own, so ReBIOS gives these their host addresses in the guest's place.

#define BAR_MEMORY_ADDRESS ((uint32_t)0xfffffff0)

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

// Whether g is a bridge between f and its root bus.
static bool above(const struct pci_enum_function *g, const struct pci_enum_function *f) {
    for (const struct pci_enum_function *p = f->parent; p != NULL; p = p->parent) {
        if (p == g) {
            return true;
        }
    }
    return false;
}

// What of [base, end) the host has nothing at: no function's BAR, no window of
// a bridge but those above f, which hold it, no memory of ACPI's devices or of
// the firmware's memory map, and not the framebuffer.
static void room_find(
    struct range_list *room,
    const struct pci_enum_function *f,
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
             && bar->base != 0) {
                take(room, bar->base, bar->size);
            }
        }
        if (g->rom.size != 0 && g->rom.base != 0) {
            take(room, g->rom.base, g->rom.size);
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
    take_ranges(room, host->private, host->private_count);
    take_ranges(room, host->motherboard, host->motherboard_count);
    for (size_t i = 0; ; i++) {
        const struct efi_memory_descriptor *d = handoff_efi_desc(core_handoff, i);
        if (d == NULL) {
            break;
        }
        uint64_t d_end;
        if (handoff_efi_desc_end(d, &d_end)) {
            take(room, d->physical_start, d_end - d->physical_start);
        }
    }
    const struct handoff_framebuffer *fb = &core_handoff->video.framebuffer;
    if (fb->present) {
        take(room, fb->base, fb->size);
    }
}

// The lowest address in room that holds size bytes at their alignment, a page
// at least, so that the BAR shares its pages with nothing.
static bool fit(const struct range_list *room, uint64_t size, uintptr_t *at) {
    uint64_t span = size > PAGE_SIZE ? size : PAGE_SIZE;
    for (size_t i = 0; i < room->count; i++) {
        uint64_t start = align_up(room->r[i].base, span);
        if (start >= room->r[i].base && start < room->r[i].end
         && room->r[i].end - start >= span) {
            *at = (uintptr_t)start;
            return true;
        }
    }
    return false;
}

// Looks for an address in the window [base, end) of the bus that f is on.
static bool window_try(
    const struct pci_enum_function *f,
    const struct pci_enum_bar *bar,
    uintptr_t base,
    uintptr_t end,
    uintptr_t *at
) {
    if (bar->kind == PCI_ENUM_BAR_MEM32 && end > FOUR_GIB) {
        end = FOUR_GIB;
    }
    if (end <= base) {
        return false;
    }
    struct range_list room = { 0 };
    room_find(&room, f, base, end);
    bool found = fit(&room, bar->size, at);
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

// An address for the BAR in a window of the bus that f is on. A prefetchable
// BAR tries the prefetchable windows first, and one that is not prefetchable
// takes none of them, as Linux 7.3's pci_bus_alloc_from_region() has it.
static bool place(
    const struct pci_enum_function *f,
    const struct pci_enum_bar *bar,
    uintptr_t *at
) {
    const struct pci_enum_function *p = f->parent;
    if (p != NULL) {
        if (p->header_type != PCI_HEADER_TYPE_BRIDGE) {
            return false;
        }
        if (bar->prefetchable && p->pref_limit >= p->pref_base
         && window_try(f, bar, p->pref_base, p->pref_limit + 1, at)) {
            return true;
        }
        return p->mem_limit >= p->mem_base
            && window_try(f, bar, p->mem_base, (uintptr_t)p->mem_limit + 1, at);
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
            if (wanted && window_try(f, bar, w->base, w->end, at)) {
                return true;
            }
        }
    }
    return false;
}

// Writes the address to the BAR with the function's memory decoding off, so
// that no half-written address decodes, and reads it back.
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
    uint32_t type = pci_cfg_read(f->addr, bar->reg, 4) & ~BAR_MEMORY_ADDRESS;
    pci_cfg_write(f->addr, bar->reg, 4, ((uint32_t)at & BAR_MEMORY_ADDRESS) | type);
    if (wide) {
        pci_cfg_write(f->addr, bar->reg + 4, 4, (uint32_t)((uint64_t)at >> 32));
    }
    uint64_t read = pci_cfg_read(f->addr, bar->reg, 4) & BAR_MEMORY_ADDRESS;
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

void pci_assign_unassigned_bars(void) {
    const struct pci_enum_topology *t = pci_enum_topology();
    if (t == NULL) {
        return;
    }
    for (size_t i = 0; i < t->function_count; i++) {
        struct pci_enum_function *f = &t->functions[i];
        for (size_t b = 0; b < f->bar_count; b++) {
            struct pci_enum_bar *bar = &f->bars[b];
            if ((bar->kind != PCI_ENUM_BAR_MEM32 && bar->kind != PCI_ENUM_BAR_MEM64)
             || bar->size == 0 || bar->base != 0) {
                continue;
            }
            uintptr_t at;
            if (!place(f, bar, &at)) {
                printf(
                    "PCI " PCI_CFG_ADDR_FMT " BAR%zu: left unassigned by the firmware, and no "
                    "window of its bus has room for its 0x%llx bytes\n",
                    PCI_CFG_ADDR_ARGS(f->addr), b, (unsigned long long)bar->size
                );
                continue;
            }
            if (!program(f, bar, at)) {
                printf(
                    "PCI " PCI_CFG_ADDR_FMT " BAR%zu: left unassigned by the firmware, and "
                    "keeps no address written to it\n", PCI_CFG_ADDR_ARGS(f->addr), b
                );
                continue;
            }
            bar->base = at;
            printf(
                "PCI " PCI_CFG_ADDR_FMT " BAR%zu: left unassigned by the firmware, now at "
                "0x%llx+0x%llx\n", PCI_CFG_ADDR_ARGS(f->addr), b, (unsigned long long)at,
                (unsigned long long)bar->size
            );
        }
    }
}
