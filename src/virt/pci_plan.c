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
#include <uacpi/namespace.h>
#include <uacpi/status.h>
#include <uacpi/types.h>
#include <uacpi/uacpi.h>
#include <virt/pci_plan.h>
#include <core.h>
#include <handoff.h>
#include <hw/pci_cfg.h>
#include <hw/pci_enum.h>
#include <hw/pci_host.h>
#include <lib/heap.h>
#include <lib/log.h>
#include <lib/util.h>
#include <memory.h>
#include <sys/mm.h>

#define ONE_MIB ((uint64_t)0x100000)
#define FOUR_GIB ((uint64_t)0x100000000)
// PCI-to-PCI Bridge Architecture Specification 1.1, 3.2.5.8 and 3.2.5.9:
// memory windows come in 1 MiB granules.
#define WINDOW_GRANULE ONE_MIB

#define HEADER_BRIDGE 1
#define HEADER_CARDBUS 2
#define BRIDGE_PREF 0x24
#define WINDOW_PREF_64 ((uint32_t)0x1)
// PCI-to-PCI Bridge Architecture Specification 1.1, 3.2.5.6: I/O windows come
// in 4 KiB granules.
#define IO_GRANULE ((uint32_t)0x1000)

#define BUSES 256
#define DEVICES 32
#define PORT_FUNCTIONS 8
#define NO_ROOT UINT32_MAX

#define ELEMENT_BAR 0
#define ELEMENT_ROM 1
#define ELEMENT_LOW_WINDOW 2
#define ELEMENT_PREF_WINDOW 3
#define ELEMENT_HIGH_WINDOW 4
#define ELEMENT_LFB 5

// Where in a root bus or bridge an element goes: its memory window, or its one
// prefetchable window, below or above 4 GiB. A root bus has one region for
// each side of 4 GiB, so no SIDE_PREF.
#define SIDE_MEMORY 0
#define SIDE_PREF 1
#define SIDE_HIGH 2
#define SIDES 3

#define NO_CONTAINER UINT32_MAX
// Below this, the top of the guest's RAM is where its PCI memory starts at the
// latest, as the processor's and chipset's fixed ranges are above.
#define TOP_AREA ((uint64_t)0xf0000000)
// A PC's I/O APIC, HPET, local APIC, and firmware flash are from here on, and
// PCI memory below.
#define PLATFORM_BASE ((uint64_t)0xfec00000)

// A BAR, an expansion ROM, a bridge's window, or the emulated VGA's linear
// framebuffer, in the memory of the root bus or bridge it is behind.
struct element {
    uint32_t function;
    uint8_t kind;
    uint8_t bar;
    size_t side;
    size_t size;
    uint64_t align;
    // From the start of the region or window it is in.
    size_t offset;
};

// A root bus's or a bridge's elements, by side.
struct container {
    struct element *elements;
    size_t count;
    size_t capacity;
    size_t size[SIDES];
    uint64_t align[SIDES];
    uintptr_t base[SIDES];
    bool placed[SIDES];
};

struct gap {
    uintptr_t base;
    uintptr_t end;
};

static struct pci_plan plan;
static bool made;

static void element_add(struct container *c, struct element e) {
    if (c->count == c->capacity) {
        size_t capacity = c->capacity == 0 ? 8 : c->capacity * 2;
        struct element *elements = kmalloc(capacity * sizeof(struct element));
        if (c->count != 0) {
            memcpy(elements, c->elements, c->count * sizeof(struct element));
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
static void pack(struct container *c, size_t side) {
    for (size_t i = 1; i < c->count; i++) {
        struct element e = c->elements[i];
        size_t j = i;
        while (j > 0 && goes_before(&e, &c->elements[j - 1])) {
            c->elements[j] = c->elements[j - 1];
            j--;
        }
        c->elements[j] = e;
    }
    size_t offset = 0;
    uint64_t max_align = 0;
    for (size_t i = 0; i < c->count; i++) {
        struct element *e = &c->elements[i];
        if (e->side != side) {
            continue;
        }
        offset = align_up(offset, e->align);
        e->offset = offset;
        offset += e->size;
        if (e->align > max_align) {
            max_align = e->align;
        }
    }
    c->size[side] = offset;
    c->align[side] = max_align;
}

// A window in a bridge's parent for what the bridge has on one side.
static void window_add(
    struct container *parent, struct container *c, size_t function, uint8_t kind,
    size_t side_inside, size_t side
) {
    if (c->size[side_inside] == 0) {
        return;
    }
    uint64_t align = c->align[side_inside] > WINDOW_GRANULE
                   ? c->align[side_inside]
                   : WINDOW_GRANULE;
    element_add(
        parent,
        (struct element){
            function, kind, 0, side, align_up(c->size[side_inside], WINDOW_GRANULE), align, 0
        }
    );
}

static bool is_bridge(const struct pci_function *f) {
    return f->header_type == HEADER_BRIDGE && f->secondary_bus > f->addr.bus;
}

// The last bus number the function's bus or, for a bridge, the buses behind it
// take.
static uint8_t last_bus(const struct pci_function *f) {
    bool bridge = (f->header_type == HEADER_BRIDGE || f->header_type == HEADER_CARDBUS)
               && f->secondary_bus > f->addr.bus;
    return bridge && f->subordinate_bus > f->addr.bus ? f->subordinate_bus : f->addr.bus;
}

static size_t root_index(const struct pci_topology *t, const struct pci_function *f) {
    for (size_t r = 0; r < t->root_bus_count; r++) {
        if (t->root_buses[r].segment == f->addr.segment && t->root_buses[r].bus == f->addr.bus) {
            return r;
        }
    }
    return NO_ROOT;
}

// The root bus whose hierarchy the function is in.
static size_t root_of(const struct pci_topology *t, const struct pci_function *f) {
    while (f->parent != NULL) {
        f = f->parent;
    }
    return root_index(t, f);
}

static uint32_t depth_of(const struct pci_topology *t, const struct pci_function *f) {
    size_t depth = 0;
    while (f->parent != NULL && depth < t->function_count) {
        f = f->parent;
        depth++;
    }
    return depth;
}

// Only a bridge's prefetchable window can lie above 4 GiB, and only if the
// bridge decodes 64-bit addresses there, as the 64-bit prefetchable window of
// a port does.
static bool path_reaches_high(
    const struct pci_topology *t, const struct pci_function *f, bool prefetchable
) {
    for (const struct pci_function *b = f->parent; b != NULL; b = b->parent) {
        if (!prefetchable || (pci_cfg_read(b->addr, BRIDGE_PREF, 4) & 0xf) != WINDOW_PREF_64) {
            return false;
        }
    }
    return prefetchable || root_of(t, f) == plan.primary;
}

// Whether what is prefetchable below 4 GiB can be in the bridge's prefetchable
// window, as firmware puts it: the bridge and those above it need to have one,
// which reads as 0 where they do not, and not need it above 4 GiB, and so the
// port above them, which always has one. The bridge is NULL for what is on a
// root bus, high_windows[n + r] whether root bus r needs its port's above 4
// GiB.
static bool pref_window_low(
    const struct pci_topology *t, const struct pci_function *bridge, size_t root,
    const bool *high_windows
) {
    bool port = root != plan.primary;
    if (port && high_windows[t->function_count + root]) {
        return false;
    }
    if (bridge == NULL) {
        return port;
    }
    for (const struct pci_function *b = bridge; b != NULL; b = b->parent) {
        if (high_windows[b - t->functions] || pci_cfg_read(b->addr, BRIDGE_PREF, 4) == 0) {
            return false;
        }
    }
    return true;
}

// Whether the host bridge of the function's root bus forwards size bytes from
// base to it, as it does for what the firmware assigned, and not for what a
// sizing left in a register it never assigned. Without host bridges known,
// anything goes.
static bool routed(const struct pci_function *f, uintptr_t base, size_t size) {
    while (f->parent != NULL) {
        f = f->parent;
    }
    const struct pci_host *host = pci_host_scan();
    for (size_t i = 0; i < host->bridge_count; i++) {
        const struct pci_host_bridge *hb = &host->bridges[i];
        if (hb->segment != f->addr.segment || hb->bus != f->addr.bus) {
            continue;
        }
        for (size_t w = 0; w < hb->window_count; w++) {
            if (base >= hb->windows[w].base && base + size <= hb->windows[w].end) {
                return true;
            }
        }
        return false;
    }
    return true;
}

static bool bar_high(
    const struct pci_topology *t, const struct pci_function *f, const struct pci_bar *bar
) {
    return bar->size > PCI_PLAN_LOW_BAR_MAX && bar->kind == PCI_BAR_MEM64
        && path_reaches_high(t, f, bar->prefetchable);
}

// A BAR smaller than a page gets a page of its own, at the same offset in it
// as in the host's page, as the guest's page maps the host's.
static size_t element_size(size_t size) {
    return size < PAGE_SIZE ? PAGE_SIZE : size;
}

// The highest address of the gaps where size bytes aligned to align fit, taken
// out of them.
static bool take_top(struct gap *gaps, size_t *count, size_t size, uint64_t align, uint64_t *at) {
    for (size_t i = *count; i-- > 0; ) {
        if (gaps[i].end - gaps[i].base < size) {
            continue;
        }
        uintptr_t base = align_down(gaps[i].end - size, align);
        if (base < gaps[i].base) {
            continue;
        }
        uintptr_t end = base + size;
        if (end < gaps[i].end) {
            memmove(&gaps[i + 2], &gaps[i + 1], (*count - i - 1) * sizeof(struct gap));
            gaps[i + 1] = (struct gap){ end, gaps[i].end };
            (*count)++;
        }
        gaps[i].end = base;
        *at = base;
        return true;
    }
    return false;
}

// Segment 0's bus 0, or else its lowest root bus, or else the first.
static size_t primary_root(const struct pci_topology *t) {
    for (size_t r = 0; r < t->root_bus_count; r++) {
        if (t->root_buses[r].segment == 0) {
            return r;
        }
    }
    return 0;
}

// The last bus number below each root bus.
static void root_ends(const struct pci_topology *t, uint8_t *ends) {
    for (size_t r = 0; r < t->root_bus_count; r++) {
        ends[r] = t->root_buses[r].bus;
    }
    for (size_t i = 0; i < t->function_count; i++) {
        size_t r = root_of(t, &t->functions[i]);
        uint8_t last = last_bus(&t->functions[i]);
        if (r != NO_ROOT && last > ends[r]) {
            ends[r] = last;
        }
    }
}

static void mark(bool *used, size_t first, size_t last) {
    for (size_t bus = first; bus <= last; bus++) {
        used[bus] = true;
    }
}

static bool numbers_free(const bool *used, size_t base, size_t count) {
    if (base + count > BUSES) {
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        if (used[base + i]) {
            return false;
        }
    }
    return true;
}

// The guest's bus numbers: segment 0's are its own but for the root bus that
// is the guest's bus 0, and the root buses of other segments, with the buses
// below them, take the lowest numbers in a row that segment 0 leaves free.
// Gives each root bus's guest number, -1 where it has none, and returns the
// highest in use.
static uint8_t number_buses(const struct pci_topology *t, const uint8_t *ends, int *guest_roots) {
    bool used[BUSES];
    memset(used, 0, BUSES * sizeof(bool));
    for (size_t i = 0; i < t->function_count; i++) {
        const struct pci_function *f = &t->functions[i];
        if (f->addr.segment == 0) {
            mark(used, f->addr.bus, f->addr.bus);
            if (last_bus(f) != f->addr.bus) {
                mark(used, f->secondary_bus, last_bus(f));
            }
        }
    }
    // Firmware may give a host bridge a bus with nothing on it.
    const struct pci_host *host = pci_host_scan();
    for (size_t i = 0; i < host->bridge_count; i++) {
        if (host->bridges[i].segment == 0) {
            mark(used, host->bridges[i].bus, host->bridges[i].bus);
        }
    }

    const struct pci_root_bus *primary = &t->root_buses[plan.primary];
    for (size_t r = 0; r < t->root_bus_count; r++) {
        const struct pci_root_bus *rb = &t->root_buses[r];
        guest_roots[r] = rb->segment == 0 ? rb->bus : -1;
    }
    if (primary->segment == 0 && primary->bus != 0) {
        plan.buses[0].bus = primary->bus;
        plan.buses[primary->bus].present = false;
        guest_roots[plan.primary] = 0;
        used[0] = true;
    }

    // The guest's bus 0 first, should it be of another segment.
    for (size_t k = 0; k <= t->root_bus_count; k++) {
        size_t r = k == 0 ? plan.primary : k - 1;
        const struct pci_root_bus *rb = &t->root_buses[r];
        if (rb->segment == 0 || (k != 0 && r == plan.primary)) {
            continue;
        }
        size_t count = (size_t)ends[r] - rb->bus + 1;
        size_t base = r == plan.primary ? 0 : 1;
        while (r != plan.primary && base < BUSES && !numbers_free(used, base, count)) {
            base++;
        }
        if (base == BUSES) {
            printf(
                "PCI root bus %04x:%02x: no room for its %zu buses among the guest's bus "
                "numbers\n", rb->segment, rb->bus, count
            );
            continue;
        }
        for (size_t i = 0; i < count; i++) {
            plan.buses[base + i] = (struct pci_plan_bus){
                true, rb->segment, (uint8_t)(rb->bus + i)
            };
        }
        mark(used, base, base + count - 1);
        guest_roots[r] = (int)base;
    }

    uint8_t last = 0;
    for (size_t bus = 0; bus < BUSES; bus++) {
        if (used[bus]) {
            last = (uint8_t)bus;
        }
    }
    return last;
}

// The device numbers of the guest's bus 0 that its functions leave free, and
// the devices below its host bridge in the firmware's namespace where firmware
// is true, lowest first. Device 0 is the host bridge's to software.
static size_t free_devices(const struct pci_topology *t, bool firmware, uint8_t *devices) {
    bool taken[DEVICES];
    memset(taken, 0, DEVICES * sizeof(bool));
    taken[0] = true;
    const struct pci_root_bus *primary = &t->root_buses[plan.primary];
    for (size_t i = 0; i < t->function_count; i++) {
        const struct pci_function *f = &t->functions[i];
        if (f->addr.segment == primary->segment && f->addr.bus == primary->bus) {
            taken[f->addr.device] = true;
        }
    }
    const struct pci_host *host = pci_host_scan();
    for (size_t i = 0; i < host->bridge_count && firmware; i++) {
        const struct pci_host_bridge *hb = &host->bridges[i];
        if (hb->segment != primary->segment || hb->bus != primary->bus) {
            continue;
        }
        uacpi_namespace_node *iter = NULL;
        while (uacpi_namespace_node_next_typed(hb->node, &iter, UACPI_OBJECT_DEVICE_BIT)
            == UACPI_STATUS_OK) {
            uint64_t adr;
            bool known = uacpi_eval_simple_integer(iter, "_ADR", &adr) == UACPI_STATUS_OK;
            if (known && adr >> 16 < DEVICES) {
                taken[adr >> 16] = true;
            }
        }
    }
    size_t count = 0;
    for (size_t device = 0; device < DEVICES; device++) {
        if (!taken[device]) {
            devices[count++] = (uint8_t)device;
        }
    }
    return count;
}

// A port on the guest's bus 0 for every root bus but the primary that has
// guest bus numbers, its functions on the first device numbers free.
static void ports_make(const struct pci_topology *t, const uint8_t *ends, const int *guest_roots) {
    size_t wanted = 0;
    for (size_t r = 0; r < t->root_bus_count; r++) {
        wanted += r != plan.primary && guest_roots[r] >= 0;
    }
    plan.ports = kmalloc((wanted != 0 ? wanted : 1) * sizeof(struct pci_plan_port));
    plan.port_count = 0;
    if (wanted == 0) {
        return;
    }
    uint8_t devices[DEVICES];
    size_t device_count = free_devices(t, true, devices);
    if (device_count * PORT_FUNCTIONS < wanted) {
        device_count = free_devices(t, false, devices);
    }
    for (size_t r = 0; r < t->root_bus_count; r++) {
        const struct pci_root_bus *rb = &t->root_buses[r];
        if (r == plan.primary || guest_roots[r] < 0) {
            continue;
        }
        size_t k = plan.port_count;
        if (k / PORT_FUNCTIONS >= device_count) {
            printf(
                "PCI root bus %04x:%02x: no device number left on the guest's bus 0 for its "
                "port\n", rb->segment, rb->bus
            );
            continue;
        }
        plan.ports[plan.port_count++] = (struct pci_plan_port){
            .root = r,
            .device = devices[k / PORT_FUNCTIONS],
            .function = (uint8_t)(k % PORT_FUNCTIONS),
            .secondary = (uint8_t)guest_roots[r],
            .subordinate = (uint8_t)(guest_roots[r] + (ends[r] - rb->bus))
        };
    }
}

// What the root bus's functions and bridges decode of I/O space, in a window
// of whole granules.
static void io_window(const struct pci_topology *t, size_t root, uint32_t *base, uint32_t *limit) {
    uint32_t low = UINT32_MAX;
    uint32_t high = 0;
    for (size_t i = 0; i < t->function_count; i++) {
        const struct pci_function *f = &t->functions[i];
        if (root_of(t, f) != root) {
            continue;
        }
        for (size_t b = 0; b < f->bar_count && b < 6; b++) {
            const struct pci_bar *bar = &f->bars[b];
            if (bar->kind != PCI_BAR_IO || bar->size == 0 || bar->base == 0) {
                continue;
            }
            if (bar->base < low) {
                low = (uint32_t)bar->base;
            }
            if (bar->base + bar->size > high) {
                high = (uint32_t)(bar->base + bar->size);
            }
        }
        if (f->header_type == HEADER_BRIDGE && f->io_limit >= f->io_base) {
            if (f->io_base < low) {
                low = f->io_base;
            }
            if (f->io_limit + 1 > high) {
                high = f->io_limit + 1;
            }
        }
    }
    if (high == 0) {
        *base = IO_GRANULE;
        *limit = IO_GRANULE - 1;
        return;
    }
    *base = align_down(low, IO_GRANULE);
    *limit = align_up(high, IO_GRANULE) - 1;
}

// A window from a range, closed where it is empty.
static void window_of(struct pci_plan_range r, uintptr_t *base, uint64_t *limit) {
    if (r.end == r.base) {
        *base = WINDOW_GRANULE;
        *limit = WINDOW_GRANULE - 1;
    } else {
        *base = r.base;
        *limit = r.end - 1;
    }
}

static void assign(const struct pci_topology *t, struct container *containers, size_t index) {
    struct container *c = &containers[index];
    for (size_t i = 0; i < c->count; i++) {
        const struct element *e = &c->elements[i];
        if (!c->placed[e->side]) {
            continue;
        }
        uintptr_t base = c->base[e->side] + e->offset;
        if (e->kind == ELEMENT_LFB) {
            plan.lfb = base;
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
                containers[e->function].base[SIDE_MEMORY] = base;
                containers[e->function].placed[SIDE_MEMORY] = true;
                break;
            }
            default: {
                size_t side = e->kind == ELEMENT_PREF_WINDOW ? SIDE_PREF : SIDE_HIGH;
                pf->pref_base = base;
                pf->pref_limit = base + e->size - 1;
                containers[e->function].base[side] = base;
                containers[e->function].placed[side] = true;
                break;
            }
        }
    }
}

// The lowest base from at, of size and align, that no obstacle overlaps.
static uintptr_t high_base(
    const struct pci_plan_range *obstacles, size_t count, uint64_t at, size_t size, uint64_t align
) {
    uintptr_t base = align_up(at, align);
    for (size_t i = 0; i < count; i++) {
        if (obstacles[i].end > base && obstacles[i].base < base + size) {
            base = align_up(obstacles[i].end, align);
        }
    }
    return base;
}

// The container of the root bus or bridge the display's VGA device is on, or
// else of the guest's bus 0.
static size_t vga_container(const struct pci_topology *t, const uint32_t *parents) {
    const struct handoff_pci_address *vga = &core_handoff->video.vga_device;
    for (size_t i = 0; i < t->function_count; i++) {
        const struct pci_addr addr = t->functions[i].addr;
        if (addr.segment == vga->segment && addr.bus == vga->bus && addr.device == vga->devfn >> 3
         && addr.function == (vga->devfn & 7) && parents[i] != NO_CONTAINER) {
            return parents[i];
        }
    }
    return t->root_bus_count != 0 ? t->function_count + plan.primary : NO_CONTAINER;
}

static const struct pci_plan_port *port_of(size_t root) {
    for (size_t k = 0; k < plan.port_count; k++) {
        if (plan.ports[k].root == root) {
            return &plan.ports[k];
        }
    }
    return NULL;
}

const struct pci_plan *pci_plan_make(
    const struct pci_plan_range *obstacles, size_t count, size_t lfb_size, uint64_t high_floor
) {
    const struct pci_topology *t = pci_enum_topology();
    size_t n = t->function_count;
    uint32_t roots = t->root_bus_count;
    made = true;
    plan.functions = kmalloc((n != 0 ? n : 1) * sizeof(struct pci_plan_function));
    plan.roots = kmalloc((roots != 0 ? roots : 1) * sizeof(struct pci_plan_root));
    plan.root_count = roots;
    plan.high_floor = high_floor;
    plan.primary = primary_root(t);
    for (size_t i = 0; i < n; i++) {
        plan.functions[i] = (struct pci_plan_function){
            .mem_base = WINDOW_GRANULE,
            .mem_limit = WINDOW_GRANULE - 1,
            .pref_base = WINDOW_GRANULE,
            .pref_limit = WINDOW_GRANULE - 1
        };
    }

    // The guest's bus numbers, and the ports in place of the root buses but
    // the primary.
    for (size_t bus = 0; bus < BUSES; bus++) {
        plan.buses[bus] = (struct pci_plan_bus){ true, 0, (uint8_t)bus };
    }
    uint8_t *ends = kmalloc((roots != 0 ? roots : 1) * sizeof(uint8_t));
    int *guest_roots = kmalloc((roots != 0 ? roots : 1) * sizeof(int));
    if (roots != 0) {
        root_ends(t, ends);
        plan.bus_end = number_buses(t, ends, guest_roots);
        ports_make(t, ends, guest_roots);
    }
    kfree(ends);
    kfree(guest_roots);

    // The bridges' containers by function index, then the root buses'.
    struct container *containers = kmalloc(
        (n + roots != 0 ? n + roots : 1) * sizeof(struct container)
    );
    for (size_t i = 0; i < n + roots; i++) {
        containers[i] = (struct container){ .elements = NULL };
    }
    uint32_t *parents = kmalloc((n != 0 ? n : 1) * sizeof(uint32_t));
    for (size_t i = 0; i < n; i++) {
        const struct pci_function *f = &t->functions[i];
        uint32_t r = root_index(t, f);
        if (f->parent != NULL) {
            parents[i] = (uint32_t)(f->parent - t->functions);
        } else {
            parents[i] = r != NO_ROOT ? n + r : NO_CONTAINER;
        }
    }

    // The bridges, and the ports, with something above 4 GiB behind them.
    bool *high_windows = kmalloc((n + roots != 0 ? n + roots : 1) * sizeof(bool));
    for (size_t i = 0; i < n + roots; i++) {
        high_windows[i] = false;
    }
    for (size_t i = 0; i < n; i++) {
        const struct pci_function *f = &t->functions[i];
        for (size_t b = 0; b < f->bar_count && b < 6; b++) {
            if (!bar_high(t, f, &f->bars[b])) {
                continue;
            }
            for (const struct pci_function *up = f->parent; up != NULL; up = up->parent) {
                high_windows[up - t->functions] = true;
            }
            size_t r = root_of(t, f);
            if (r != NO_ROOT) {
                high_windows[n + r] = true;
            }
        }
    }

    for (size_t i = 0; i < n; i++) {
        const struct pci_function *f = &t->functions[i];
        if (parents[i] == NO_CONTAINER) {
            continue;
        }
        struct container *c = &containers[parents[i]];
        bool pref_low = pref_window_low(t, f->parent, root_of(t, f), high_windows);
        for (size_t b = 0; b < f->bar_count && b < 6; b++) {
            const struct pci_bar *bar = &f->bars[b];
            if ((bar->kind != PCI_BAR_MEM32 && bar->kind != PCI_BAR_MEM64) || bar->size == 0
             || bar->base == 0) {
                continue;
            }
            size_t side = bar_high(t, f, bar)
                        ? SIDE_HIGH
                        : bar->prefetchable && pref_low ? SIDE_PREF : SIDE_MEMORY;
            size_t size = element_size(bar->size);
            element_add(c, (struct element){ i, ELEMENT_BAR, b, side, size, size, 0 });
        }
        if (f->rom.kind == PCI_BAR_MEM32 && f->rom.size != 0 && f->rom.base != 0
         && routed(f, f->rom.base, f->rom.size)) {
            size_t size = element_size(f->rom.size);
            element_add(c, (struct element){ i, ELEMENT_ROM, 0, SIDE_MEMORY, size, size, 0 });
        }
    }
    // In the memory of what the VGA's card is on: an installed Windows 7 keeps
    // to 16 colours with the linear framebuffer in reserved memory, and on a
    // root bus's when the card is behind a bridge.
    size_t lfb_container = lfb_size != 0 ? vga_container(t, parents) : NO_CONTAINER;
    if (lfb_container != NO_CONTAINER) {
        element_add(
            &containers[lfb_container],
            (struct element){ 0, ELEMENT_LFB, 0, SIDE_MEMORY, lfb_size, 2 * ONE_MIB, 0 }
        );
    }

    // The deepest bridges first, so that each window holds its bridges'.
    uint32_t *order = kmalloc((n != 0 ? n : 1) * sizeof(uint32_t));
    uint32_t *depths = kmalloc((n != 0 ? n : 1) * sizeof(uint32_t));
    uint32_t bridges = 0;
    for (size_t i = 0; i < n; i++) {
        if (is_bridge(&t->functions[i]) && parents[i] != NO_CONTAINER) {
            depths[i] = depth_of(t, &t->functions[i]);
            size_t j = bridges++;
            while (j > 0 && depths[order[j - 1]] < depths[i]) {
                order[j] = order[j - 1];
                j--;
            }
            order[j] = i;
        }
    }
    for (size_t k = 0; k < bridges; k++) {
        size_t i = order[k];
        struct container *c = &containers[i];
        pack(c, SIDE_MEMORY);
        pack(c, SIDE_PREF);
        pack(c, SIDE_HIGH);
        struct container *parent = &containers[parents[i]];
        // A root bus has a region below 4 GiB, a port two windows.
        bool on_primary = t->functions[i].parent == NULL
                       && root_of(t, &t->functions[i]) == plan.primary;
        size_t pref_side = on_primary ? SIDE_MEMORY : SIDE_PREF;
        window_add(parent, c, i, ELEMENT_LOW_WINDOW, SIDE_MEMORY, SIDE_MEMORY);
        window_add(parent, c, i, ELEMENT_PREF_WINDOW, SIDE_PREF, pref_side);
        window_add(parent, c, i, ELEMENT_HIGH_WINDOW, SIDE_HIGH, SIDE_HIGH);
    }

    // The guest's PCI hole: what the obstacles leave below the platform's
    // ranges.
    struct gap *gaps = kmalloc((count + 2 * roots + 4) * sizeof(struct gap));
    size_t gap_count = 0;
    uint64_t cursor = ONE_MIB;
    for (size_t i = 0; i <= count; i++) {
        uintptr_t end = i < count ? obstacles[i].base : PLATFORM_BASE;
        if (end > PLATFORM_BASE) {
            end = PLATFORM_BASE;
        }
        if (end > cursor) {
            gaps[gap_count++] = (struct gap){ cursor, end };
        }
        if (i < count && obstacles[i].end > cursor) {
            cursor = obstacles[i].end;
        }
        if (cursor >= PLATFORM_BASE) {
            break;
        }
    }

    // The guest's configuration space window, where the host has one.
    bool ecam_present = false;
    for (size_t bus = 0; bus <= plan.bus_end && roots != 0; bus++) {
        const struct pci_plan_bus *b = &plan.buses[bus];
        if (b->present && pci_cfg_ecam_base(b->segment, b->bus) != 0) {
            ecam_present = true;
        }
    }
    size_t ecam_size = ((size_t)plan.bus_end + 1) * ONE_MIB;

    // The guest's bus 0's region below 4 GiB, the ports' windows there, and
    // the configuration space window, largest alignment first, from the top
    // down. Root buses that have no port are not the guest's.
    uint64_t high_cursor = high_floor;
    plan.tolud = FOUR_GIB;
    size_t ecam_piece = 2 * (size_t)roots;
    size_t pieces = ecam_piece + 1;
    uint32_t *placing = kmalloc(pieces * sizeof(uint32_t));
    uint64_t *piece_align = kmalloc(pieces * sizeof(uint64_t));
    uint64_t *piece_size = kmalloc(pieces * sizeof(uint64_t));
    uint32_t placed = 0;
    for (size_t r = 0; r < roots; r++) {
        struct container *c = &containers[n + r];
        pack(c, SIDE_MEMORY);
        pack(c, SIDE_PREF);
        pack(c, SIDE_HIGH);
        plan.roots[r] = (struct pci_plan_root){
            .segment = t->root_buses[r].segment,
            .bus = t->root_buses[r].bus
        };
        piece_size[2 * r] = 0;
        piece_size[2 * r + 1] = 0;
        if (r != plan.primary && port_of(r) == NULL) {
            continue;
        }
        if (c->size[SIDE_HIGH] != 0) {
            c->base[SIDE_HIGH] = high_base(
                obstacles, count, high_cursor, c->size[SIDE_HIGH], c->align[SIDE_HIGH]
            );
            c->placed[SIDE_HIGH] = true;
            high_cursor = c->base[SIDE_HIGH] + c->size[SIDE_HIGH];
            plan.roots[r].high = (struct pci_plan_range){ c->base[SIDE_HIGH], high_cursor };
        }
        for (size_t s = 0; s < 2; s++) {
            size_t side = s == 0 ? SIDE_MEMORY : SIDE_PREF;
            piece_size[2 * r + s] = align_up(c->size[side], ONE_MIB);
            piece_align[2 * r + s] = c->align[side] > ONE_MIB ? c->align[side] : ONE_MIB;
        }
    }
    piece_size[ecam_piece] = ecam_present ? ecam_size : 0;
    piece_align[ecam_piece] = ONE_MIB;
    for (size_t p = 0; p < pieces; p++) {
        if (piece_size[p] == 0) {
            continue;
        }
        size_t j = placed++;
        while (j > 0
            && (piece_align[placing[j - 1]] < piece_align[p]
             || (piece_align[placing[j - 1]] == piece_align[p]
              && piece_size[placing[j - 1]] < piece_size[p]))) {
            placing[j] = placing[j - 1];
            j--;
        }
        placing[j] = p;
    }
    for (size_t k = 0; k < placed; k++) {
        uint32_t p = placing[k];
        uint64_t at;
        if (!take_top(gaps, &gap_count, piece_size[p], piece_align[p], &at)) {
            if (p == ecam_piece) {
                printf("PCI: no room below 4 GiB for the guest's configuration space window\n");
            } else {
                printf(
                    "PCI root bus %04x:%02x: no room below 4 GiB for 0x%llx bytes of BARs\n",
                    plan.roots[p / 2].segment, plan.roots[p / 2].bus,
                    (unsigned long long)piece_size[p]
                );
            }
            continue;
        }
        if (at < plan.tolud) {
            plan.tolud = at;
        }
        if (p == ecam_piece) {
            plan.ecam = at;
            continue;
        }
        size_t r = p / 2;
        size_t side = p % 2 == 0 ? SIDE_MEMORY : SIDE_PREF;
        containers[n + r].base[side] = at;
        containers[n + r].placed[side] = true;
        struct pci_plan_range range = { at, at + piece_size[p] };
        if (side == SIDE_MEMORY) {
            plan.roots[r].low = range;
        } else {
            plan.roots[r].pref = range;
        }
    }

    for (size_t i = 0; i < count; i++) {
        if (obstacles[i].base >= TOP_AREA && obstacles[i].base < plan.tolud) {
            plan.tolud = obstacles[i].base;
        }
    }
    if (plan.tolud > TOP_AREA && plan.ecam == 0 && placed == 0) {
        plan.tolud = TOP_AREA;
    }

    // From the root buses down, so that each window has its place first.
    for (size_t r = 0; r < roots; r++) {
        assign(t, containers, n + r);
    }
    for (size_t k = bridges; k-- > 0; ) {
        assign(t, containers, order[k]);
    }
    for (size_t k = 0; k < plan.port_count; k++) {
        struct pci_plan_port *port = &plan.ports[k];
        const struct pci_plan_root *pr = &plan.roots[port->root];
        window_of(pr->low, &port->mem_base, &port->mem_limit);
        window_of(
            pr->pref.end != pr->pref.base ? pr->pref : pr->high, &port->pref_base,
            &port->pref_limit
        );
        io_window(t, port->root, &port->io_base, &port->io_limit);
    }

    for (size_t i = 0; i < n + roots; i++) {
        kfree(containers[i].elements);
    }
    kfree(containers);
    kfree(high_windows);
    kfree(parents);
    kfree(order);
    kfree(depths);
    kfree(gaps);
    kfree(placing);
    kfree(piece_align);
    kfree(piece_size);

    for (size_t r = 0; r < roots; r++) {
        const struct pci_plan_root *pr = &plan.roots[r];
        printf(
            "PCI root bus %04x:%02x: guest memory 0x%llx-0x%llx", pr->segment, pr->bus,
            (unsigned long long)pr->low.base, (unsigned long long)pr->low.end
        );
        if (pr->pref.end != pr->pref.base) {
            printf(
                ", prefetchable 0x%llx-0x%llx", (unsigned long long)pr->pref.base,
                (unsigned long long)pr->pref.end
            );
        }
        if (pr->high.end != pr->high.base) {
            printf(
                ", and 0x%llx-0x%llx", (unsigned long long)pr->high.base,
                (unsigned long long)pr->high.end
            );
        }
        printf("\n");
    }
    for (size_t k = 0; k < plan.port_count; k++) {
        const struct pci_plan_port *port = &plan.ports[k];
        const struct pci_plan_root *pr = &plan.roots[port->root];
        printf(
            "PCI root bus %04x:%02x: the guest's buses %02x-%02x, behind its port at 00:%02x.%x\n",
            pr->segment, pr->bus, port->secondary, port->subordinate, port->device, port->function
        );
    }
    if (plan.ecam != 0) {
        printf(
            "PCI: guest configuration space for buses 00-%02x at 0x%llx\n", plan.bus_end,
            (unsigned long long)plan.ecam
        );
    }
    printf("PCI: the guest's PCI hole starts at 0x%llx\n", (unsigned long long)plan.tolud);
    return &plan;
}

const struct pci_plan *pci_plan(void) {
    return made ? &plan : NULL;
}

int pci_plan_guest_bus(uint16_t segment, uint8_t bus) {
    if (!made) {
        return segment == 0 ? bus : -1;
    }
    for (size_t g = 0; g < BUSES; g++) {
        const struct pci_plan_bus *b = &plan.buses[g];
        if (b->present && b->segment == segment && b->bus == bus) {
            return (int)g;
        }
    }
    return -1;
}
