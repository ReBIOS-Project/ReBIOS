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

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <memory.h>
#include <hw/pci_cfg.h>
#include <hw/pci_enum.h>
#include <hw/pci_host.h>
#include <lib/heap.h>
#include <lib/log.h>
#include <sys/mm.h>
#include <virt/pci_plan.h>

#define ONE_MIB 0x100000ull
#define FOUR_GIB 0x100000000ull
// PCI-to-PCI Bridge Architecture Specification 1.1, 3.2.5.8 and 3.2.5.9:
// memory windows come in 1 MiB granules.
#define WINDOW_GRANULE ONE_MIB

#define HEADER_BRIDGE 1
#define BRIDGE_PREF 0x24
#define WINDOW_PREF_64 0x1u

#define ELEMENT_BAR 0
#define ELEMENT_ROM 1
#define ELEMENT_LOW_WINDOW 2
#define ELEMENT_HIGH_WINDOW 3

#define VIRT_FUNCTION (-1)
#define NO_CONTAINER UINT32_MAX
// Below this, the top of the guest's RAM is where its PCI memory starts at
// the latest, as the processor's and chipset's fixed ranges are above.
#define TOP_AREA 0xf0000000ull
// A PC's I/O APIC, HPET, local APIC, and firmware flash are from here on, and
// PCI memory below.
#define PLATFORM_BASE 0xfec00000ull

// A BAR, an expansion ROM, or a bridge's window, in the memory of the root bus
// or bridge it is behind.
struct element {
    int32_t function;
    uint8_t kind;
    uint8_t bar;
    bool high;
    uint64_t size;
    uint64_t align;
    // From the start of the region or window it is in.
    uint64_t offset;
};

// A root bus's or a bridge's elements, below and above 4 GiB.
struct container {
    struct element *elements;
    uint32_t count;
    uint32_t capacity;
    uint64_t low_size;
    uint64_t low_align;
    uint64_t high_size;
    uint64_t high_align;
    uint64_t low_base;
    uint64_t high_base;
    bool low_placed;
    bool high_placed;
};

struct gap {
    uint64_t base;
    uint64_t end;
};

static struct pci_plan plan;
static bool made;

static uint64_t align_up(uint64_t x, uint64_t align) {
    return (x + align - 1) & ~(align - 1);
}

static uint64_t align_down(uint64_t x, uint64_t align) {
    return x & ~(align - 1);
}

static void element_add(struct container *c, struct element e) {
    if (c->count == c->capacity) {
        uint32_t capacity = c->capacity == 0 ? 8 : c->capacity * 2;
        struct element *elements = kmalloc(capacity * sizeof(*elements));
        if (c->count != 0) {
            memcpy(elements, c->elements, c->count * sizeof(*elements));
        }
        kfree(c->elements);
        c->elements = elements;
        c->capacity = capacity;
    }
    c->elements[c->count++] = e;
}

static bool goes_before(const struct element *a, const struct element *b) {
    if (a->align != b->align) {
        return a->align > b->align;
    }
    return a->size > b->size;
}

// The container's elements of one side, largest alignment first, one after the
// other: none then needs padding but for the alignment of what follows.
static void pack(struct container *c, bool high, uint64_t *size, uint64_t *align) {
    for (uint32_t i = 1; i < c->count; i++) {
        struct element e = c->elements[i];
        uint32_t j = i;
        while (j > 0 && goes_before(&e, &c->elements[j - 1])) {
            c->elements[j] = c->elements[j - 1];
            j--;
        }
        c->elements[j] = e;
    }
    uint64_t offset = 0;
    uint64_t max_align = 0;
    for (uint32_t i = 0; i < c->count; i++) {
        struct element *e = &c->elements[i];
        if (e->high != high) {
            continue;
        }
        offset = align_up(offset, e->align);
        e->offset = offset;
        offset += e->size;
        if (e->align > max_align) {
            max_align = e->align;
        }
    }
    *size = offset;
    *align = max_align;
}

static bool is_bridge(const struct pci_function *f) {
    return f->header_type == HEADER_BRIDGE && f->secondary_bus > f->addr.bus;
}

static uint32_t root_index(const struct pci_topology *t, const struct pci_function *f) {
    for (uint32_t r = 0; r < t->root_bus_count; r++) {
        if (t->root_buses[r].segment == f->addr.segment && t->root_buses[r].bus == f->addr.bus) {
            return r;
        }
    }
    return UINT32_MAX;
}

static uint32_t depth_of(const struct pci_topology *t, const struct pci_function *f) {
    uint32_t depth = 0;
    while (f->parent != NULL && depth < t->function_count) {
        f = f->parent;
        depth++;
    }
    return depth;
}

// Only a bridge's prefetchable window can lie above 4 GiB, and only if the
// bridge decodes 64-bit addresses there.
static bool path_reaches_high(const struct pci_function *f, bool prefetchable) {
    for (const struct pci_function *b = f->parent; b != NULL; b = b->parent) {
        if (!prefetchable
            || (pci_cfg_read(b->addr, BRIDGE_PREF, 4) & 0xf) != WINDOW_PREF_64) {
            return false;
        }
    }
    return true;
}

// Whether the host bridge of the function's root bus forwards [base, base +
// size) to it, as it does for what the firmware assigned, and not for what a
// sizing left in a register it never assigned. Without host bridges known,
// anything goes.
static bool routed(const struct pci_function *f, uint64_t base, uint64_t size) {
    while (f->parent != NULL) {
        f = f->parent;
    }
    const struct pci_host *host = pci_host_scan();
    for (uint32_t i = 0; i < host->bridge_count; i++) {
        const struct pci_host_bridge *hb = &host->bridges[i];
        if (hb->segment != f->addr.segment || hb->bus != f->addr.bus) {
            continue;
        }
        for (uint32_t w = 0; w < hb->window_count; w++) {
            if (base >= hb->windows[w].base && base + size <= hb->windows[w].end) {
                return true;
            }
        }
        return false;
    }
    return true;
}

// A BAR smaller than a page gets a page of its own, at the same offset in it as
// in the host's page, as the guest's page maps the host's.
static uint64_t element_size(uint64_t size) {
    return size < PAGE_SIZE ? PAGE_SIZE : size;
}

// The highest address of the gaps where size bytes aligned to align fit, taken
// out of them.
static bool take_top(struct gap *gaps, uint32_t *count, uint64_t size, uint64_t align,
                     uint64_t *at) {
    for (uint32_t i = *count; i-- > 0;) {
        if (gaps[i].end - gaps[i].base < size) {
            continue;
        }
        uint64_t base = align_down(gaps[i].end - size, align);
        if (base < gaps[i].base) {
            continue;
        }
        uint64_t end = base + size;
        if (end < gaps[i].end) {
            memmove(&gaps[i + 2], &gaps[i + 1], (*count - i - 1) * sizeof(*gaps));
            gaps[i + 1] = (struct gap){end, gaps[i].end};
            (*count)++;
        }
        gaps[i].end = base;
        *at = base;
        return true;
    }
    return false;
}

// What decodes in segment 0: its configuration space window's buses from the
// first it covers to the highest a function or bridge uses.
static void ecam_extent(const struct pci_topology *t, bool *present, uint8_t *start, uint8_t *end) {
    *present = false;
    for (uint32_t bus = 0; bus < 256; bus++) {
        if (pci_cfg_ecam_base(0, (uint8_t)bus) != 0) {
            *present = true;
            *start = (uint8_t)bus;
            break;
        }
    }
    if (!*present) {
        return;
    }
    uint8_t last = *start;
    for (uint32_t i = 0; i < t->function_count; i++) {
        const struct pci_function *f = &t->functions[i];
        if (f->addr.segment != 0) {
            continue;
        }
        uint8_t bus = is_bridge(f) && f->subordinate_bus > f->addr.bus ? f->subordinate_bus
                                                                       : f->addr.bus;
        if (bus > last) {
            last = bus;
        }
    }
    *end = last;
}

// The lowest device number of segment 0's bus 0 that nothing answers on.
static int free_device(const struct pci_topology *t) {
    for (uint8_t d = 1; d < 32; d++) {
        bool used = false;
        for (uint32_t i = 0; i < t->function_count && !used; i++) {
            const struct pci_function *f = &t->functions[i];
            used = f->addr.segment == 0 && f->addr.bus == 0 && f->addr.device == d;
        }
        if (!used && pci_cfg_read((struct pci_addr){0, 0, d, 0}, 0, 2) == 0xffff) {
            return d;
        }
    }
    printf("No free device number on PCI bus 0 for a virtual device\n");
    return -1;
}

static void assign(const struct pci_topology *t, struct container *containers, uint32_t index) {
    struct container *c = &containers[index];
    for (uint32_t i = 0; i < c->count; i++) {
        const struct element *e = &c->elements[i];
        if (e->high ? !c->high_placed : !c->low_placed) {
            continue;
        }
        uint64_t base = (e->high ? c->high_base : c->low_base) + e->offset;
        if (e->function == VIRT_FUNCTION) {
            plan.virt_bar = base;
            continue;
        }
        const struct pci_function *f = &t->functions[e->function];
        struct pci_plan_function *pf = &plan.functions[e->function];
        switch (e->kind) {
            case ELEMENT_BAR: {
                pf->bars[e->bar] = base + (f->bars[e->bar].base & (e->size - 1));
                break;
            }
            case ELEMENT_ROM: {
                pf->rom = base + (f->rom.base & (e->size - 1));
                break;
            }
            case ELEMENT_LOW_WINDOW: {
                pf->mem_base = base;
                pf->mem_limit = base + e->size - 1;
                containers[e->function].low_base = base;
                containers[e->function].low_placed = true;
                break;
            }
            default: {
                pf->pref_base = base;
                pf->pref_limit = base + e->size - 1;
                containers[e->function].high_base = base;
                containers[e->function].high_placed = true;
                break;
            }
        }
    }
}

const struct pci_plan *pci_plan_make(const struct pci_plan_range *obstacles, uint32_t count,
                                     uint64_t virt_bar_size, uint64_t high_floor) {
    const struct pci_topology *t = pci_enum_topology();
    uint32_t n = t->function_count;
    uint32_t roots = t->root_bus_count;
    made = true;
    plan.functions = kmalloc((n != 0 ? n : 1) * sizeof(*plan.functions));
    plan.roots = kmalloc((roots != 0 ? roots : 1) * sizeof(*plan.roots));
    plan.root_count = roots;
    plan.high_floor = high_floor;
    for (uint32_t i = 0; i < n; i++) {
        plan.functions[i] = (struct pci_plan_function){
            .mem_base = WINDOW_GRANULE,
            .mem_limit = WINDOW_GRANULE - 1,
            .pref_base = WINDOW_GRANULE,
            .pref_limit = WINDOW_GRANULE - 1,
        };
    }

    // The bridges' containers by function index, then the root buses'.
    struct container *containers = kmalloc((n + roots) * sizeof(*containers));
    for (uint32_t i = 0; i < n + roots; i++) {
        containers[i] = (struct container){.elements = NULL};
    }
    uint32_t *parents = kmalloc((n != 0 ? n : 1) * sizeof(*parents));
    for (uint32_t i = 0; i < n; i++) {
        const struct pci_function *f = &t->functions[i];
        uint32_t r = root_index(t, f);
        if (f->parent != NULL) {
            parents[i] = (uint32_t)(f->parent - t->functions);
        } else {
            parents[i] = r != UINT32_MAX ? n + r : NO_CONTAINER;
        }
    }

    for (uint32_t i = 0; i < n; i++) {
        const struct pci_function *f = &t->functions[i];
        if (parents[i] == NO_CONTAINER) {
            continue;
        }
        struct container *c = &containers[parents[i]];
        for (uint8_t b = 0; b < f->bar_count && b < 6; b++) {
            const struct pci_bar *bar = &f->bars[b];
            if ((bar->kind != PCI_BAR_MEM32 && bar->kind != PCI_BAR_MEM64) || bar->size == 0
                || bar->base == 0) {
                continue;
            }
            bool high = bar->size > PCI_PLAN_LOW_BAR_MAX && bar->kind == PCI_BAR_MEM64
                        && path_reaches_high(f, bar->prefetchable);
            uint64_t size = element_size(bar->size);
            element_add(c, (struct element){(int32_t)i, ELEMENT_BAR, b, high, size, size, 0});
        }
        if (f->rom.kind == PCI_BAR_MEM32 && f->rom.size != 0 && f->rom.base != 0
            && routed(f, f->rom.base, f->rom.size)) {
            uint64_t size = element_size(f->rom.size);
            element_add(c, (struct element){(int32_t)i, ELEMENT_ROM, 0, false, size, size, 0});
        }
    }
    plan.virt_device = virt_bar_size != 0 ? free_device(t) : -1;
    if (plan.virt_device >= 0) {
        for (uint32_t r = 0; r < roots; r++) {
            if (t->root_buses[r].segment == 0 && t->root_buses[r].bus == 0) {
                element_add(&containers[n + r], (struct element){VIRT_FUNCTION, ELEMENT_BAR, 0,
                                                                 false, virt_bar_size,
                                                                 virt_bar_size, 0});
            }
        }
    }

    // The deepest bridges first, so that each window holds its bridges'.
    uint32_t *order = kmalloc((n != 0 ? n : 1) * sizeof(*order));
    uint32_t *depths = kmalloc((n != 0 ? n : 1) * sizeof(*depths));
    uint32_t bridges = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (is_bridge(&t->functions[i]) && parents[i] != NO_CONTAINER) {
            depths[i] = depth_of(t, &t->functions[i]);
            uint32_t j = bridges++;
            while (j > 0 && depths[order[j - 1]] < depths[i]) {
                order[j] = order[j - 1];
                j--;
            }
            order[j] = i;
        }
    }
    for (uint32_t k = 0; k < bridges; k++) {
        uint32_t i = order[k];
        struct container *c = &containers[i];
        pack(c, false, &c->low_size, &c->low_align);
        pack(c, true, &c->high_size, &c->high_align);
        struct container *parent = &containers[parents[i]];
        if (c->low_size != 0) {
            uint64_t align = c->low_align > WINDOW_GRANULE ? c->low_align : WINDOW_GRANULE;
            element_add(parent, (struct element){(int32_t)i, ELEMENT_LOW_WINDOW, 0, false,
                                                 align_up(c->low_size, WINDOW_GRANULE), align,
                                                 0});
        }
        if (c->high_size != 0) {
            uint64_t align = c->high_align > WINDOW_GRANULE ? c->high_align : WINDOW_GRANULE;
            element_add(parent, (struct element){(int32_t)i, ELEMENT_HIGH_WINDOW, 0, true,
                                                 align_up(c->high_size, WINDOW_GRANULE), align,
                                                 0});
        }
    }

    // The guest's PCI hole: what the obstacles leave below the platform's
    // ranges.
    struct gap *gaps = kmalloc((count + roots + 4) * sizeof(*gaps));
    uint32_t gap_count = 0;
    uint64_t cursor = ONE_MIB;
    for (uint32_t i = 0; i <= count; i++) {
        uint64_t end = i < count ? obstacles[i].base : PLATFORM_BASE;
        if (end > PLATFORM_BASE) {
            end = PLATFORM_BASE;
        }
        if (end > cursor) {
            gaps[gap_count++] = (struct gap){cursor, end};
        }
        if (i < count && obstacles[i].end > cursor) {
            cursor = obstacles[i].end;
        }
        if (cursor >= PLATFORM_BASE) {
            break;
        }
    }

    bool ecam_present;
    uint8_t ecam_start = 0;
    uint8_t ecam_end = 0;
    ecam_extent(t, &ecam_present, &ecam_start, &ecam_end);
    uint64_t ecam_size = ((uint64_t)ecam_end - ecam_start + 1) * ONE_MIB;

    // The root buses' regions below 4 GiB and the configuration space window,
    // largest alignment first, from the top down.
    uint64_t high_cursor = high_floor;
    plan.tolud = FOUR_GIB;
    uint32_t pieces = roots + 1;
    uint32_t *placing = kmalloc(pieces * sizeof(*placing));
    uint64_t *piece_align = kmalloc(pieces * sizeof(*piece_align));
    uint64_t *piece_size = kmalloc(pieces * sizeof(*piece_size));
    uint32_t placed = 0;
    for (uint32_t r = 0; r < roots; r++) {
        struct container *c = &containers[n + r];
        pack(c, false, &c->low_size, &c->low_align);
        pack(c, true, &c->high_size, &c->high_align);
        plan.roots[r] = (struct pci_plan_root){
            .segment = t->root_buses[r].segment,
            .bus = t->root_buses[r].bus,
        };
        if (c->high_size != 0) {
            c->high_base = align_up(high_cursor, c->high_align);
            c->high_placed = true;
            high_cursor = c->high_base + c->high_size;
            plan.roots[r].high = (struct pci_plan_range){c->high_base, high_cursor};
        }
        piece_size[r] = align_up(c->low_size, ONE_MIB);
        piece_align[r] = c->low_align > ONE_MIB ? c->low_align : ONE_MIB;
    }
    piece_size[roots] = ecam_present ? ecam_size : 0;
    piece_align[roots] = ONE_MIB;
    for (uint32_t p = 0; p < pieces; p++) {
        if (piece_size[p] == 0) {
            continue;
        }
        uint32_t j = placed++;
        while (j > 0 && (piece_align[placing[j - 1]] < piece_align[p]
                         || (piece_align[placing[j - 1]] == piece_align[p]
                             && piece_size[placing[j - 1]] < piece_size[p]))) {
            placing[j] = placing[j - 1];
            j--;
        }
        placing[j] = p;
    }
    for (uint32_t k = 0; k < placed; k++) {
        uint32_t p = placing[k];
        uint64_t at;
        if (!take_top(gaps, &gap_count, piece_size[p], piece_align[p], &at)) {
            if (p == roots) {
                printf("PCI: no room below 4 GiB for the guest's configuration space window\n");
            } else {
                printf("PCI root bus %04x:%02x: no room below 4 GiB for 0x%llx bytes of BARs\n",
                       plan.roots[p].segment, plan.roots[p].bus,
                       (unsigned long long)piece_size[p]);
            }
            continue;
        }
        if (at < plan.tolud) {
            plan.tolud = at;
        }
        if (p == roots) {
            plan.ecam = at;
            plan.ecam_start = ecam_start;
            plan.ecam_end = ecam_end;
        } else {
            containers[n + p].low_base = at;
            containers[n + p].low_placed = true;
            plan.roots[p].low = (struct pci_plan_range){at, at + piece_size[p]};
        }
    }

    for (uint32_t i = 0; i < count; i++) {
        if (obstacles[i].base >= TOP_AREA && obstacles[i].base < plan.tolud) {
            plan.tolud = obstacles[i].base;
        }
    }
    if (plan.tolud > TOP_AREA && plan.ecam == 0 && placed == 0) {
        plan.tolud = TOP_AREA;
    }

    // From the root buses down, so that each window has its place first.
    for (uint32_t r = 0; r < roots; r++) {
        assign(t, containers, n + r);
    }
    for (uint32_t k = bridges; k-- > 0;) {
        assign(t, containers, order[k]);
    }

    for (uint32_t i = 0; i < n + roots; i++) {
        kfree(containers[i].elements);
    }
    kfree(containers);
    kfree(parents);
    kfree(order);
    kfree(depths);
    kfree(gaps);
    kfree(placing);
    kfree(piece_align);
    kfree(piece_size);

    for (uint32_t r = 0; r < roots; r++) {
        const struct pci_plan_root *pr = &plan.roots[r];
        printf("PCI root bus %04x:%02x: guest memory 0x%llx-0x%llx", pr->segment, pr->bus,
               (unsigned long long)pr->low.base, (unsigned long long)pr->low.end);
        if (pr->high.end != pr->high.base) {
            printf(", and 0x%llx-0x%llx", (unsigned long long)pr->high.base,
                   (unsigned long long)pr->high.end);
        }
        printf("\n");
    }
    if (plan.ecam != 0) {
        printf("PCI: guest configuration space for buses %02x-%02x at 0x%llx\n", plan.ecam_start,
               plan.ecam_end, (unsigned long long)plan.ecam);
    }
    printf("PCI: the guest's PCI hole starts at 0x%llx\n", (unsigned long long)plan.tolud);
    return &plan;
}

const struct pci_plan *pci_plan(void) {
    return made ? &plan : NULL;
}
