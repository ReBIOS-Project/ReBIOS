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

// What a BAR takes on its own: a page at least.
static uint64_t span_of(const struct pci_enum_bar *bar) {
    return bar->size > PAGE_SIZE ? bar->size : PAGE_SIZE;
}

static bool assigned(const struct pci_enum_bar *bar) {
    bool memory = bar->kind == PCI_ENUM_BAR_MEM32 || bar->kind == PCI_ENUM_BAR_MEM64;
    return memory && bar->size != 0 && bar->base != 0;
}

// What of [base, end) the host has nothing at: no function's BAR, no window of
// a bridge but those above f, which hold it, no memory of ACPI's devices or of
// the firmware's memory map, and not the framebuffer, where they do not move:
// the BARs of f that moving has a bit for, and the framebuffer in them, do.
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
    take_ranges(room, host->tables, host->table_count);
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
    room_find(&room, f, 0, base, end);
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
             || bar->size == 0
             || bar->base != 0) {
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
                    "keeps no address written to it\n",
                    PCI_CFG_ADDR_ARGS(f->addr), b
                );
                continue;
            }
            bar->base = at;
            printf(
                "PCI " PCI_CFG_ADDR_FMT " BAR%zu: left unassigned by the firmware, now at "
                "0x%llx+0x%llx\n",
                PCI_CFG_ADDR_ARGS(f->addr), b, (unsigned long long)at,
                (unsigned long long)bar->size
            );
        }
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

static bool overlaps(uint64_t base, uint64_t last, uint64_t other, uint64_t size) {
    return size != 0 && other <= last && other + size - 1 >= base;
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
        if (g->rom.base != 0 && overlaps(base, last, g->rom.base, g->rom.size)) {
            return false;
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
        uint64_t span = span_of(&f->bars[order[k]]);
        if (cursor < begin + span) {
            return false;
        }
        at[k] = align_down(cursor - span, span);
        cursor = at[k];
    }
    *low = align_down(cursor, granule);
    *high = align_up(at[0] + span_of(&f->bars[order[0]]), granule);
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
    uint32_t type = pci_cfg_read(f->addr, bar->reg, 4) & ~BAR_MEMORY_ADDRESS;
    pci_cfg_write(f->addr, bar->reg, 4, ((uint32_t)at & BAR_MEMORY_ADDRESS) | type);
    if (bar->kind == PCI_ENUM_BAR_MEM64) {
        pci_cfg_write(f->addr, bar->reg + 4, 4, (uint32_t)(at >> 32));
    }
}

static uint64_t bar_word_read(const struct pci_enum_function *f, const struct pci_enum_bar *bar) {
    uint64_t at = pci_cfg_read(f->addr, bar->reg, 4) & BAR_MEMORY_ADDRESS;
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
        const struct pci_enum_bar *bar = &f->bars[order[k]];
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
            bar_word_write(f, &f->bars[order[k]], f->bars[order[k]].base);
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
    for (size_t k = 0; k < count; k++) {
        f->bars[order[k]].base = at[k];
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
    size_t order[6];
    size_t count = 0;
    order[count++] = index;
    for (size_t b = 0; b < f->bar_count && b < 6; b++) {
        if (b == index || (group & (uint32_t)1 << b) == 0) {
            continue;
        }
        size_t k = count++;
        while (k > 1 && f->bars[order[k - 1]].size < f->bars[b].size) {
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
        uint64_t w_end = w->end < FOUR_GIB ? w->end : FOUR_GIB;
        if (w->base < w_end) {
            room_find(&room, f, group, w->base, (uintptr_t)w_end);
        }
    }
    depth = 0;
    for (const struct pci_enum_function *p = f->parent; p != NULL; p = p->parent) {
        uint64_t other_base;
        uint64_t other_last;
        if (window_get(p, window_other(kinds[depth]), &other_base, &other_last)) {
            take(&room, other_base, other_last - other_base + 1);
        }
        depth++;
    }
    uint64_t at[6];
    uint64_t new_low = 0;
    uint64_t new_high = 0;
    bool found = false;
    for (size_t r = room.count; r-- > 0 && !found; ) {
        found = lay_down(
            f, order, count, bridged, room.r[r].base, room.r[r].end, at, &new_low, &new_high
        );
    }
    range_list_free(&room);

    if (found && at[0] > first->base
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
