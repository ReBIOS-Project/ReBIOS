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
#include <uacpi/acpi.h>
#include <uacpi/namespace.h>
#include <uacpi/resources.h>
#include <uacpi/status.h>
#include <uacpi/tables.h>
#include <uacpi/types.h>
#include <uacpi/uacpi.h>
#include <uacpi/utilities.h>
#include <memory.h>
#include <core.h>
#include <handoff.h>
#include <hw/acpi.h>
#include <hw/pci_cfg.h>
#include <hw/pci_enum.h>
#include <hw/pci_relocate.h>
#include <iommu/iommu_tables.h>
#include <lib/heap.h>
#include <lib/log.h>
#include <lib/printf.h>

// Resizable BARs above this are shrunk to it, and BARs still larger keep their
// place.
#define BAR_SIZE_MAX (256ull << 20)

#define ONE_MIB 0x100000ull
#define FOUR_GIB 0x100000000ull
#define CLAIM_GRANULE 0x1000ull

#define PCI_COMMAND 0x04
#define COMMAND_MEMORY (1u << 1)
#define BAR_MEMORY_ADDRESS 0xfffffff0u
#define ROM_ADDRESS 0xfffff800u
#define ROM_ENABLE (1u << 0)

#define BRIDGE_MEMORY 0x20
#define BRIDGE_PREF 0x24
#define BRIDGE_PREF_BASE_UPPER 0x28
#define BRIDGE_PREF_LIMIT_UPPER 0x2c
#define WINDOW_TYPE 0xfu
#define WINDOW_TYPE_BITS 0x000f000fu
#define WINDOW_PREF_64 0x1u
#define WINDOW_GRANULE ONE_MIB
#define WINDOW_CLOSED_BASE 0xfff00000ull
#define WINDOW_CLOSED_LIMIT 0x000fffffull

#define HEADER_BRIDGE 1
#define HEADER_CARDBUS 2
#define CLASS_BRIDGE 0x06
#define SUBCLASS_HOST_BRIDGE 0x00

// The SR-IOV Control register and its VF Memory Space Enable bit, Intel 82599
// 10 GbE Controller Datasheet 331520-005 rev. 3.4, 9.4.4.3.
#define SRIOV_CONTROL 0x08
#define SRIOV_VF_MSE (1u << 3)

// AMD IOMMU 48882 rev. 3.11, Capability Offset 04h.
#define AMD_IOMMU_MMIO_LARGE 0x80000ull
#define AMD_IOMMU_MMIO_SMALL 0x4000ull

#define APIC_MMIO_SIZE 0x1000ull
#define HPET_MMIO_SIZE 0x1000ull
// SDM 325462-093 Vol. 3A, 13.11.1: the processors take every access to this
// range for an interrupt message.
#define MSI_BASE 0xfee00000ull
#define MSI_SIZE 0x100000ull

#define STA_PRESENT (1u << 0)

#define ITEM_BAR 0
#define ITEM_ROM 1
#define ITEM_MEM_WINDOW 2
#define ITEM_PREF_WINDOW 3

#define CONTAINER_ROOT (-1)
#define CONTAINER_NONE (-2)
#define ROOT_NONE UINT32_MAX
#define PARENT_DEPTH_MAX 256

// Why an item keeps its place.
#define FIXED_NOT 0
#define FIXED_LARGE 1
#define FIXED_LOW 2
#define FIXED_HOST_BRIDGE 3
#define FIXED_CLAIMED 4
#define FIXED_CONTENTS 5
#define FIXED_CONTAINER 6

#define PLAN_DONE 0
#define PLAN_NO_ROOM 1
#define PLAN_NO_GAIN 2

#define NAME_SIZE 48

#define ADDR_FMT "%04x:%02x:%02x.%x"
#define ADDR_ARGS(a) (a).segment, (a).bus, (a).device, (a).function

struct range {
    uint64_t base;
    uint64_t end;
};

// Sorted, neither overlapping nor touching.
struct range_list {
    struct range *r;
    uint32_t count;
    uint32_t capacity;
};

struct window {
    uint64_t base;
    uint64_t end;
    bool prefetchable;
};

struct host_bridge {
    uacpi_namespace_node *node;
    uint16_t segment;
    uint8_t bus;
    // Its root bus has a function decoding memory the records lack: a CardBus
    // bridge, or virtual functions.
    bool undescribed;
    // The memory windows of its _CRS, as given, and how many lie below 4 GiB.
    struct window *windows;
    uint32_t window_count;
    uint32_t window_capacity;
    uint32_t low_window_count;
};

// A host bridge as pci_relocate_reserve() needs it.
struct reserve_bridge {
    uint16_t segment;
    uint8_t bus;
    bool undescribed;
    // Its windows clipped to [1 MiB, 4 GiB), in reserve_state.windows.
    struct window *windows;
    uint32_t window_count;
};

// What pci_relocate() leaves for pci_relocate_reserve().
struct reserve_state {
    struct reserve_bridge *bridges;
    uint32_t bridge_count;
    struct window *windows;
    // Within [1 MiB, 4 GiB): what is claimed, what any function decodes where
    // pci_relocate() left it, and what was reserved.
    struct range_list used;
};

// A memory BAR, an expansion ROM BAR, or a bridge memory window.
struct item {
    struct pci_function *f;
    // Its root bus's index in the topology, or ROOT_NONE.
    uint32_t root;
    uint8_t kind;
    uint8_t bar;
    bool prefetchable;
    // FIXED_*.
    uint8_t fixed;
    // Out of the layout: neither window of its bridge holds it, or the
    // window that does is out of it.
    bool stuck;
    // A window holding nothing but empty windows.
    bool empty;
    bool closed;
    // The number of windows around it.
    uint32_t depth;
    // The window holding it, or CONTAINER_*.
    int32_t container;
    uint64_t base;
    uint64_t size;
    uint64_t align;

    // The plan: a window's new size, the offset in the window holding it, and
    // the new base.
    uint64_t plan_size;
    uint64_t offset;
    uint64_t new_base;
    bool moving;
    bool closing;
};

struct relocation {
    const struct pci_topology *topology;
    struct range_list claimed;
    uint32_t crs_failures;
    struct host_bridge *bridges;
    uint32_t bridge_count;
    uint32_t bridge_capacity;
    struct item *items;
    uint32_t item_count;
    // Per function: its root bus, and its windows' items or -1.
    uint32_t *function_root;
    int32_t *mem_window;
    int32_t *pref_window;
};

struct crs_scan {
    struct relocation *r;
    // NULL for a device other than a host bridge.
    struct host_bridge *hb;
    bool bus_found;
    uint64_t bus;
};

struct saved_command {
    struct pci_function *f;
    uint16_t command;
};

static const char *const host_bridge_ids[] = {"PNP0A03", "PNP0A08", NULL};

static struct reserve_state reserve_state;

static uint64_t range_end(uint64_t base, uint64_t size) {
    return size > UINT64_MAX - base ? UINT64_MAX : base + size;
}

// align is a power of two.
static bool align_up(uint64_t x, uint64_t align, uint64_t *out) {
    uint64_t r = (x + align - 1) & ~(align - 1);
    if (r < x) {
        return false;
    }
    *out = r;
    return true;
}

static uint64_t power_of_two_at_least(uint64_t x) {
    uint64_t p = 1;
    while (p < x && p <= UINT64_MAX / 2) {
        p *= 2;
    }
    return p;
}

static void list_grow(struct range_list *l) {
    if (l->count < l->capacity) {
        return;
    }
    uint32_t capacity = l->capacity == 0 ? 32 : l->capacity * 2;
    struct range *r = kmalloc(capacity * sizeof(*r));
    if (l->count != 0) {
        memcpy(r, l->r, l->count * sizeof(*r));
    }
    kfree(l->r);
    l->r = r;
    l->capacity = capacity;
}

static void list_free(struct range_list *l) {
    kfree(l->r);
    l->r = NULL;
    l->count = 0;
    l->capacity = 0;
}

// Adds [base, end), merged with the ranges it overlaps or touches.
static void list_add(struct range_list *l, uint64_t base, uint64_t end) {
    if (base >= end) {
        return;
    }
    uint32_t first = 0;
    while (first < l->count && l->r[first].end < base) {
        first++;
    }
    uint32_t last = first;
    while (last < l->count && l->r[last].base <= end) {
        if (l->r[last].base < base) {
            base = l->r[last].base;
        }
        if (l->r[last].end > end) {
            end = l->r[last].end;
        }
        last++;
    }
    if (last == first) {
        list_grow(l);
        memmove(&l->r[first + 1], &l->r[first], (l->count - first) * sizeof(*l->r));
        l->count++;
    } else {
        memmove(&l->r[first + 1], &l->r[last], (l->count - last) * sizeof(*l->r));
        l->count -= last - first - 1;
    }
    l->r[first].base = base;
    l->r[first].end = end;
}

static void list_remove(struct range_list *l, uint64_t base, uint64_t end) {
    uint32_t i = 0;
    while (base < end && i < l->count) {
        struct range *x = &l->r[i];
        if (x->end <= base || x->base >= end) {
            i++;
        } else if (x->base < base && x->end > end) {
            uint64_t old_end = x->end;
            x->end = base;
            list_grow(l);
            memmove(&l->r[i + 2], &l->r[i + 1], (l->count - i - 1) * sizeof(*l->r));
            l->count++;
            l->r[i + 1].base = end;
            l->r[i + 1].end = old_end;
            return;
        } else if (x->base < base) {
            x->end = base;
            i++;
        } else if (x->end > end) {
            x->base = end;
            i++;
        } else {
            memmove(x, x + 1, (l->count - i - 1) * sizeof(*l->r));
            l->count--;
        }
    }
}

static bool list_overlaps(const struct range_list *l, uint64_t base, uint64_t end) {
    for (uint32_t i = 0; i < l->count && l->r[i].base < end; i++) {
        if (l->r[i].end > base) {
            return true;
        }
    }
    return false;
}

// The lowest address of the list where size bytes aligned to align fit.
static bool list_fit(const struct range_list *l, uint64_t size, uint64_t align, uint64_t *at) {
    for (uint32_t i = 0; i < l->count; i++) {
        uint64_t base;
        if (!align_up(l->r[i].base, align, &base) || base >= l->r[i].end) {
            continue;
        }
        if (l->r[i].end - base >= size) {
            *at = base;
            return true;
        }
    }
    return false;
}

// The lowest address of [base, end) where size bytes aligned to align overlap
// none of the list's ranges.
static bool list_fit_between(const struct range_list *l, uint64_t base, uint64_t end,
                             uint64_t size, uint64_t align, uint64_t *at) {
    uint64_t x;
    if (!align_up(base, align, &x)) {
        return false;
    }
    for (uint32_t i = 0; i < l->count; i++) {
        if (x >= end || end - x < size) {
            return false;
        }
        if (l->r[i].end <= x) {
            continue;
        }
        if (l->r[i].base >= x + size) {
            break;
        }
        if (!align_up(l->r[i].end, align, &x)) {
            return false;
        }
    }
    if (x >= end || end - x < size) {
        return false;
    }
    *at = x;
    return true;
}

static void claim(struct relocation *r, uint64_t base, uint64_t size) {
    if (size == 0) {
        return;
    }
    uint64_t end;
    if (!align_up(range_end(base, size), CLAIM_GRANULE, &end)) {
        end = UINT64_MAX;
    }
    list_add(&r->claimed, base & ~(CLAIM_GRANULE - 1), end);
}

static void log_failure(uacpi_namespace_node *node, const char *name, uacpi_status status) {
    const char *path = uacpi_namespace_node_generate_absolute_path(node);
    printf("Cannot evaluate %s.%s: %s\n", path != NULL ? path : "?", name,
           uacpi_status_to_string(status));
    if (path != NULL) {
        uacpi_free_absolute_path(path);
    }
}

// Memory the firmware's runtime code, SMM, or ACPI may use, BARs included,
// is all but what the boot services leave free.
static bool efi_type_free(uint32_t type) {
    switch (type) {
        case EfiConventionalMemory:
        case EfiLoaderCode:
        case EfiLoaderData:
        case EfiBootServicesCode:
        case EfiBootServicesData: {
            return true;
        }
        default: {
            return false;
        }
    }
}

static void claim_memory_map(struct relocation *r) {
    const struct handoff *h = core_handoff;
    uint64_t step = h->efi_mmap_desc_size;
    if (step < sizeof(EFI_MEMORY_DESCRIPTOR)) {
        printf("PCI: no usable firmware memory map to keep BARs off\n");
        return;
    }
    for (uint64_t off = 0; off <= h->efi_mmap_size && h->efi_mmap_size - off >= step;
         off += step) {
        const EFI_MEMORY_DESCRIPTOR *d = (const void *)(uintptr_t)(h->efi_mmap + off);
        if (efi_type_free(d->Type)) {
            continue;
        }
        uint64_t size = d->NumberOfPages > UINT64_MAX / EFI_PAGE_SIZE
                        ? UINT64_MAX : d->NumberOfPages * EFI_PAGE_SIZE;
        claim(r, d->PhysicalStart, size);
    }
}

static void claim_mcfg(struct relocation *r) {
    struct uacpi_table table;
    if (uacpi_table_find_by_signature(ACPI_MCFG_SIGNATURE, &table) != UACPI_STATUS_OK) {
        return;
    }
    uint32_t length = table.hdr->length;
    for (uint32_t off = sizeof(struct acpi_mcfg);
         off <= length && length - off >= sizeof(struct acpi_mcfg_allocation);
         off += sizeof(struct acpi_mcfg_allocation)) {
        struct acpi_mcfg_allocation entry;
        memcpy(&entry, (const uint8_t *)table.ptr + off, sizeof(entry));
        if (entry.start_bus <= entry.end_bus) {
            claim(r, entry.address + ((uint64_t)entry.start_bus << 20),
                  ((uint64_t)entry.end_bus - entry.start_bus + 1) << 20);
        }
    }
    uacpi_table_unref(&table);
}

static void claim_madt(struct relocation *r) {
    struct uacpi_table table;
    if (uacpi_table_find_by_signature(ACPI_MADT_SIGNATURE, &table) != UACPI_STATUS_OK) {
        return;
    }
    const uint8_t *madt = table.ptr;
    uint32_t length = table.hdr->length;
    if (length < sizeof(struct acpi_madt)) {
        uacpi_table_unref(&table);
        return;
    }
    struct acpi_madt header;
    memcpy(&header, madt, sizeof(header));
    uint64_t lapic = header.local_interrupt_controller_address;

    uint32_t off = sizeof(struct acpi_madt);
    while (off <= length && length - off >= sizeof(struct acpi_entry_hdr)) {
        uint8_t type = madt[off];
        uint8_t entry_length = madt[off + 1];
        if (entry_length < sizeof(struct acpi_entry_hdr) || entry_length > length - off) {
            break;
        }
        if (type == ACPI_MADT_ENTRY_TYPE_IOAPIC
            && entry_length >= sizeof(struct acpi_madt_ioapic)) {
            struct acpi_madt_ioapic entry;
            memcpy(&entry, madt + off, sizeof(entry));
            claim(r, entry.address, APIC_MMIO_SIZE);
        } else if (type == ACPI_MADT_ENTRY_TYPE_LAPIC_ADDRESS_OVERRIDE
                   && entry_length >= sizeof(struct acpi_madt_lapic_address_override)) {
            struct acpi_madt_lapic_address_override entry;
            memcpy(&entry, madt + off, sizeof(entry));
            lapic = entry.address;
        }
        off += entry_length;
    }
    claim(r, lapic, APIC_MMIO_SIZE);
    uacpi_table_unref(&table);
}

static void claim_hpets(struct relocation *r) {
    struct uacpi_table table;
    for (uacpi_size n = 0;
         uacpi_table_find_nth_by_signature(ACPI_HPET_SIGNATURE, n, &table) == UACPI_STATUS_OK;
         n++) {
        if (table.hdr->length >= sizeof(struct acpi_hpet)) {
            struct acpi_hpet hpet;
            memcpy(&hpet, table.ptr, sizeof(hpet));
            if (hpet.address.address_space_id == ACPI_AS_ID_SYS_MEM) {
                claim(r, hpet.address.address, HPET_MMIO_SIZE);
            }
        }
        uacpi_table_unref(&table);
    }
}

static void claim_iommus(struct relocation *r) {
    const struct iommu_tables *tables = iommu_tables_parse();
    for (uint32_t i = 0; i < tables->unit_count; i++) {
        const struct iommu_unit *u = &tables->units[i];
        if (tables->kind == IOMMU_KIND_VTD) {
            claim(r, u->base, u->vtd_size);
        } else if (tables->kind == IOMMU_KIND_AMD) {
            claim(r, u->base, u->base % AMD_IOMMU_MMIO_LARGE == 0
                              ? AMD_IOMMU_MMIO_LARGE : AMD_IOMMU_MMIO_SMALL);
        }
    }
}

static void window_add(struct host_bridge *hb, uint64_t base, uint64_t end, bool prefetchable) {
    if (hb->window_count == hb->window_capacity) {
        uint32_t capacity = hb->window_capacity == 0 ? 8 : hb->window_capacity * 2;
        struct window *windows = kmalloc(capacity * sizeof(*windows));
        if (hb->window_count != 0) {
            memcpy(windows, hb->windows, hb->window_count * sizeof(*windows));
        }
        kfree(hb->windows);
        hb->windows = windows;
        hb->window_capacity = capacity;
    }
    hb->windows[hb->window_count++] = (struct window){base, end, prefetchable};
}

static void crs_address(struct crs_scan *s, const uacpi_resource_address_common *common,
                        uint64_t minimum, uint64_t length, uint64_t translation) {
    if (common->type == UACPI_RANGE_BUS) {
        if (s->hb != NULL && !s->bus_found) {
            s->bus_found = true;
            s->bus = minimum;
        }
        return;
    }
    if (common->type != UACPI_RANGE_MEMORY || length == 0) {
        return;
    }
    if (s->hb == NULL || common->direction != UACPI_PRODUCER) {
        claim(s->r, minimum, length);
        return;
    }
    if (translation != 0) {
        const char *path = uacpi_namespace_node_generate_absolute_path(s->hb->node);
        printf("PCI host bridge %s: window 0x%llx+0x%llx translated by 0x%llx, not used\n",
               path != NULL ? path : "?", (unsigned long long)minimum,
               (unsigned long long)length, (unsigned long long)translation);
        if (path != NULL) {
            uacpi_free_absolute_path(path);
        }
        return;
    }
    // ACPI 6.6, 6.4.3.5.5: for PCI windows, cacheable and write-combining mean
    // prefetchable too.
    window_add(s->hb, minimum, range_end(minimum, length),
               common->attribute.memory.caching != UACPI_NON_CACHEABLE);
}

static uacpi_iteration_decision crs_resource(void *user, uacpi_resource *res) {
    struct crs_scan *s = user;
    switch (res->type) {
        case UACPI_RESOURCE_TYPE_ADDRESS16: {
            const uacpi_resource_address16 *a = &res->address16;
            crs_address(s, &a->common, a->minimum, a->address_length, a->translation_offset);
            break;
        }
        case UACPI_RESOURCE_TYPE_ADDRESS32: {
            const uacpi_resource_address32 *a = &res->address32;
            crs_address(s, &a->common, a->minimum, a->address_length, a->translation_offset);
            break;
        }
        case UACPI_RESOURCE_TYPE_ADDRESS64: {
            const uacpi_resource_address64 *a = &res->address64;
            crs_address(s, &a->common, a->minimum, a->address_length, a->translation_offset);
            break;
        }
        case UACPI_RESOURCE_TYPE_ADDRESS64_EXTENDED: {
            const uacpi_resource_address64_extended *a = &res->address64_extended;
            crs_address(s, &a->common, a->minimum, a->address_length, a->translation_offset);
            break;
        }
        case UACPI_RESOURCE_TYPE_MEMORY24: {
            // In 256-byte units, ACPI 6.6, 6.4.3.1.
            claim(s->r, (uint64_t)res->memory24.minimum << 8,
                  (uint64_t)res->memory24.length << 8);
            break;
        }
        case UACPI_RESOURCE_TYPE_MEMORY32: {
            claim(s->r, res->memory32.minimum, res->memory32.length);
            break;
        }
        case UACPI_RESOURCE_TYPE_FIXED_MEMORY32: {
            claim(s->r, res->fixed_memory32.address, res->fixed_memory32.length);
            break;
        }
        default: {
            break;
        }
    }
    return UACPI_ITERATION_DECISION_CONTINUE;
}

// A device without _CRS has no resources to walk.
static uacpi_status crs_walk(struct crs_scan *s, uacpi_namespace_node *node) {
    uacpi_resources *resources;
    uacpi_status status = uacpi_get_current_resources(node, &resources);
    if (status == UACPI_STATUS_NOT_FOUND) {
        return UACPI_STATUS_OK;
    }
    if (status != UACPI_STATUS_OK) {
        s->r->crs_failures++;
        return status;
    }
    uacpi_for_each_resource(resources, crs_resource, s);
    uacpi_free_resources(resources);
    return UACPI_STATUS_OK;
}

// _SEG and _BBN, which are 0 where a host bridge has none. found may be NULL.
static bool eval_optional(uacpi_namespace_node *node, const char *name, uint64_t *value,
                          bool *found) {
    uacpi_status status = uacpi_eval_simple_integer(node, name, value);
    if (found != NULL) {
        *found = status == UACPI_STATUS_OK;
    }
    if (status == UACPI_STATUS_NOT_FOUND) {
        *value = 0;
        return true;
    }
    if (status != UACPI_STATUS_OK) {
        log_failure(node, name, status);
        return false;
    }
    return true;
}

static void host_bridge_log(const struct host_bridge *hb, const char *path) {
    printf("PCI host bridge %s: segment %04x, bus %02x, %s windows below 4 GiB\n", path,
           hb->segment, hb->bus, hb->low_window_count == 0 ? "no" : "with");
    for (uint32_t i = 0; i < hb->window_count; i++) {
        const struct window *w = &hb->windows[i];
        uint64_t base = w->base < ONE_MIB ? ONE_MIB : w->base;
        uint64_t end = w->end > FOUR_GIB ? FOUR_GIB : w->end;
        if (base < end) {
            printf("PCI host bridge %s: window 0x%llx-0x%llx%s\n", path,
                   (unsigned long long)base, (unsigned long long)(end - 1),
                   w->prefetchable ? ", prefetchable" : "");
        }
    }
}

static bool host_bridge_known(const struct relocation *r, const struct host_bridge *hb,
                              const char *name) {
    for (uint32_t i = 0; i < r->bridge_count; i++) {
        if (r->bridges[i].segment == hb->segment && r->bridges[i].bus == hb->bus) {
            printf("PCI host bridge %s: segment %04x, bus %02x has a host bridge already, "
                   "ignoring this one\n", name, hb->segment, hb->bus);
            return true;
        }
    }
    return false;
}

static void host_bridge_found(struct relocation *r, uacpi_namespace_node *node) {
    struct host_bridge hb = {.node = node};
    struct crs_scan scan = {.r = r, .hb = &hb};
    uacpi_status status = crs_walk(&scan, node);
    if (status != UACPI_STATUS_OK) {
        log_failure(node, "_CRS", status);
    }

    uint64_t segment;
    uint64_t bbn;
    bool has_bbn;
    if (!eval_optional(node, "_SEG", &segment, NULL)
        || !eval_optional(node, "_BBN", &bbn, &has_bbn)) {
        kfree(hb.windows);
        return;
    }
    // The low bits only, ACPI 6.6, 6.5.5 and 6.5.6.
    hb.segment = (uint16_t)segment;
    hb.bus = scan.bus_found ? (uint8_t)scan.bus : (uint8_t)bbn;
    for (uint32_t i = 0; i < hb.window_count; i++) {
        if (hb.windows[i].base < FOUR_GIB && hb.windows[i].end > ONE_MIB) {
            hb.low_window_count++;
        }
    }

    const char *path = uacpi_namespace_node_generate_absolute_path(node);
    const char *name = path != NULL ? path : "?";
    if (scan.bus_found && has_bbn && (uint8_t)bbn != hb.bus) {
        printf("PCI host bridge %s: _CRS starts at bus %02x, _BBN says %02x\n", name, hb.bus,
               (uint8_t)bbn);
    }
    host_bridge_log(&hb, name);
    bool known = host_bridge_known(r, &hb, name);
    if (path != NULL) {
        uacpi_free_absolute_path(path);
    }
    if (known) {
        kfree(hb.windows);
        return;
    }

    if (r->bridge_count == r->bridge_capacity) {
        uint32_t capacity = r->bridge_capacity == 0 ? 8 : r->bridge_capacity * 2;
        struct host_bridge *bridges = kmalloc(capacity * sizeof(*bridges));
        if (r->bridge_count != 0) {
            memcpy(bridges, r->bridges, r->bridge_count * sizeof(*bridges));
        }
        kfree(r->bridges);
        r->bridges = bridges;
        r->bridge_capacity = capacity;
    }
    r->bridges[r->bridge_count++] = hb;
}

static uacpi_iteration_decision device_found(void *user, uacpi_namespace_node *node,
                                             uacpi_u32 depth) {
    (void)depth;
    struct relocation *r = user;
    bool host_bridge = uacpi_device_matches_pnp_id(node, host_bridge_ids);
    uacpi_u32 sta;
    uacpi_status status = uacpi_eval_sta(node, &sta);
    if (status != UACPI_STATUS_OK) {
        log_failure(node, "_STA", status);
        // A host bridge that may be absent gives no windows, and another
        // device's memory is kept clear all the same.
        if (host_bridge) {
            return UACPI_ITERATION_DECISION_CONTINUE;
        }
        sta = STA_PRESENT;
    }
    if (!(sta & STA_PRESENT)) {
        return UACPI_ITERATION_DECISION_CONTINUE;
    }

    if (host_bridge) {
        host_bridge_found(r, node);
    } else {
        struct crs_scan scan = {.r = r, .hb = NULL};
        crs_walk(&scan, node);
    }
    return UACPI_ITERATION_DECISION_CONTINUE;
}

static struct host_bridge *host_bridge_for(struct relocation *r, uint16_t segment, uint8_t bus) {
    for (uint32_t i = 0; i < r->bridge_count; i++) {
        if (r->bridges[i].segment == segment && r->bridges[i].bus == bus) {
            return &r->bridges[i];
        }
    }
    return NULL;
}

static bool is_window(const struct item *it) {
    return it->kind == ITEM_MEM_WINDOW || it->kind == ITEM_PREF_WINDOW;
}

static bool is_movable(const struct item *it) {
    return it->fixed == FIXED_NOT && !it->stuck;
}

static uint64_t item_end(const struct item *it) {
    return range_end(it->base, it->size);
}

static uint64_t item_plan_size(const struct item *it) {
    return is_window(it) ? it->plan_size : it->size;
}

static void item_name(const struct item *it, char *buf) {
    const struct pci_addr a = it->f->addr;
    switch (it->kind) {
        case ITEM_BAR: {
            snprintf(buf, NAME_SIZE, ADDR_FMT " BAR%u", ADDR_ARGS(a), it->bar);
            break;
        }
        case ITEM_ROM: {
            snprintf(buf, NAME_SIZE, ADDR_FMT " ROM", ADDR_ARGS(a));
            break;
        }
        case ITEM_MEM_WINDOW: {
            snprintf(buf, NAME_SIZE, ADDR_FMT " mem window", ADDR_ARGS(a));
            break;
        }
        default: {
            snprintf(buf, NAME_SIZE, ADDR_FMT " pref window", ADDR_ARGS(a));
            break;
        }
    }
}

static const char *fixed_reason(uint8_t fixed) {
    switch (fixed) {
        case FIXED_LARGE: {
            return "larger than 256 MiB";
        }
        case FIXED_LOW: {
            return "below 1 MiB";
        }
        case FIXED_HOST_BRIDGE: {
            return "of a host bridge";
        }
        case FIXED_CLAIMED: {
            return "overlaps memory in use";
        }
        case FIXED_CONTENTS: {
            return "holds a fixed item";
        }
        default: {
            return "in a fixed window";
        }
    }
}

static bool is_host_bridge_function(const struct pci_function *f) {
    return f->base_class == CLASS_BRIDGE && f->subclass == SUBCLASS_HOST_BRIDGE;
}

static uint32_t root_of(const struct pci_topology *t, const struct pci_function *f) {
    for (unsigned i = 0; f->parent != NULL && i < PARENT_DEPTH_MAX; i++) {
        f = f->parent;
    }
    for (uint32_t i = 0; i < t->root_bus_count; i++) {
        if (t->root_buses[i].segment == f->addr.segment && t->root_buses[i].bus == f->addr.bus) {
            return i;
        }
    }
    return ROOT_NONE;
}

static int32_t item_add(struct relocation *r, struct pci_function *f, uint32_t root, uint8_t kind,
                        uint8_t bar, bool prefetchable, uint64_t base, uint64_t size) {
    struct item *it = &r->items[r->item_count];
    *it = (struct item){
        .f = f,
        .root = root,
        .kind = kind,
        .bar = bar,
        .prefetchable = prefetchable,
        .container = CONTAINER_ROOT,
        .base = base,
        .size = size,
        .align = power_of_two_at_least(size),
    };
    return (int32_t)r->item_count++;
}

static void items_build(struct relocation *r) {
    const struct pci_topology *t = r->topology;
    uint32_t n = t->function_count;
    r->function_root = kmalloc(n * sizeof(*r->function_root));
    r->mem_window = kmalloc(n * sizeof(*r->mem_window));
    r->pref_window = kmalloc(n * sizeof(*r->pref_window));
    // Its BARs, its ROM BAR, and two windows at most for each function.
    r->items = kmalloc((size_t)n * (6 + 3) * sizeof(*r->items));

    for (uint32_t i = 0; i < n; i++) {
        struct pci_function *f = &t->functions[i];
        uint32_t root = root_of(t, f);
        r->function_root[i] = root;
        r->mem_window[i] = -1;
        r->pref_window[i] = -1;

        for (uint8_t b = 0; b < f->bar_count && b < 6; b++) {
            const struct pci_bar *bar = &f->bars[b];
            // A base of 0 is a BAR the firmware did not assign.
            if ((bar->kind == PCI_BAR_MEM32 || bar->kind == PCI_BAR_MEM64) && bar->size != 0
                && bar->base != 0) {
                item_add(r, f, root, ITEM_BAR, b, bar->prefetchable, bar->base, bar->size);
            }
        }
        // PCI Local Bus Specification 3.0, 6.2.5.2: only an enabled ROM BAR
        // decodes.
        if (f->rom.kind == PCI_BAR_MEM32 && f->rom.size != 0
            && (pci_cfg_read(f->addr, f->rom.reg, 4) & ROM_ENABLE)) {
            item_add(r, f, root, ITEM_ROM, 0, false, f->rom.base, f->rom.size);
        }
        if (f->header_type == HEADER_BRIDGE) {
            if (f->mem_limit >= f->mem_base) {
                r->mem_window[i] = item_add(r, f, root, ITEM_MEM_WINDOW, 0, false, f->mem_base,
                                            (uint64_t)f->mem_limit - f->mem_base + 1);
            }
            if (f->pref_limit >= f->pref_base) {
                uint64_t size = f->pref_limit - f->pref_base;
                r->pref_window[i] = item_add(r, f, root, ITEM_PREF_WINDOW, 0, true, f->pref_base,
                                             size == UINT64_MAX ? size : size + 1);
            }
        }
    }
}

static bool window_holds(const struct item *w, const struct item *it) {
    return !w->closed && it->base >= w->base && item_end(it) <= item_end(w);
}

static void containers_find(struct relocation *r) {
    const struct pci_topology *t = r->topology;
    for (uint32_t i = 0; i < r->item_count; i++) {
        struct item *it = &r->items[i];
        if (it->f->parent == NULL) {
            continue;
        }
        uint32_t p = (uint32_t)(it->f->parent - t->functions);
        int32_t first = it->prefetchable ? r->pref_window[p] : r->mem_window[p];
        int32_t second = it->prefetchable ? r->mem_window[p] : r->pref_window[p];
        if (first >= 0 && window_holds(&r->items[first], it)) {
            it->container = first;
        } else if (second >= 0 && window_holds(&r->items[second], it)) {
            it->container = second;
        } else {
            it->container = CONTAINER_NONE;
            char name[NAME_SIZE];
            item_name(it, name);
            printf("PCI %s: 0x%llx-0x%llx is outside the windows of " ADDR_FMT
                   ", left as it is\n", name, (unsigned long long)it->base,
                   (unsigned long long)(item_end(it) - 1), ADDR_ARGS(it->f->parent->addr));
        }
    }

    for (uint32_t i = 0; i < r->item_count; i++) {
        struct item *it = &r->items[i];
        int32_t c = it->container;
        while (c >= 0 && it->depth < r->item_count) {
            it->depth++;
            c = r->items[c].container;
        }
        it->stuck = c == CONTAINER_NONE;
    }
}

static void fixed_find(struct relocation *r) {
    for (uint32_t i = 0; i < r->item_count; i++) {
        struct item *it = &r->items[i];
        if (it->kind == ITEM_BAR && it->size > BAR_SIZE_MAX) {
            it->fixed = FIXED_LARGE;
        } else if ((it->kind == ITEM_BAR || it->kind == ITEM_ROM) && it->base < ONE_MIB) {
            it->fixed = FIXED_LOW;
        } else if (is_host_bridge_function(it->f)) {
            it->fixed = FIXED_HOST_BRIDGE;
        } else if (list_overlaps(&r->claimed, it->base, item_end(it))) {
            it->fixed = FIXED_CLAIMED;
        }
    }
    // A fixed item keeps the windows around it, and a fixed window everything
    // in it.
    for (uint32_t i = 0; i < r->item_count; i++) {
        struct item *it = &r->items[i];
        if (it->fixed == FIXED_NOT || it->fixed == FIXED_CONTENTS) {
            continue;
        }
        for (int32_t c = it->container; c >= 0; c = r->items[c].container) {
            if (r->items[c].fixed != FIXED_NOT) {
                break;
            }
            r->items[c].fixed = FIXED_CONTENTS;
        }
    }
    for (uint32_t i = 0; i < r->item_count; i++) {
        struct item *it = &r->items[i];
        if (it->fixed != FIXED_NOT) {
            continue;
        }
        for (int32_t c = it->container; c >= 0; c = r->items[c].container) {
            if (r->items[c].fixed != FIXED_NOT) {
                it->fixed = FIXED_CONTAINER;
                break;
            }
        }
    }
}

static void sort_by_depth(const struct relocation *r, int32_t *list, uint32_t count,
                          bool deepest_first) {
    for (uint32_t i = 1; i < count; i++) {
        int32_t x = list[i];
        uint32_t depth = r->items[x].depth;
        uint32_t j = i;
        while (j > 0) {
            uint32_t other = r->items[list[j - 1]].depth;
            if (deepest_first ? other >= depth : other <= depth) {
                break;
            }
            list[j] = list[j - 1];
            j--;
        }
        list[j] = x;
    }
}

// A window is empty when nothing but empty windows is in it.
static void empty_find(struct relocation *r) {
    int32_t *list = kmalloc(r->item_count * sizeof(*list));
    uint32_t *held = kmalloc(r->item_count * sizeof(*held));
    for (uint32_t i = 0; i < r->item_count; i++) {
        list[i] = (int32_t)i;
    }
    sort_by_depth(r, list, r->item_count, true);
    for (uint32_t k = 0; k < r->item_count; k++) {
        struct item *it = &r->items[list[k]];
        if (is_window(it)) {
            it->empty = held[list[k]] == 0;
        }
        if (it->container >= 0 && !(is_window(it) && it->empty)) {
            held[it->container]++;
        }
    }
    kfree(held);
    kfree(list);
}

// Decreasing alignment, then size, then increasing base.
static bool goes_before(const struct item *a, const struct item *b) {
    if (a->align != b->align) {
        return a->align > b->align;
    }
    uint64_t size_a = item_plan_size(a);
    uint64_t size_b = item_plan_size(b);
    if (size_a != size_b) {
        return size_a > size_b;
    }
    if (a->base != b->base) {
        return a->base < b->base;
    }
    return a < b;
}

static void sort_for_placement(const struct relocation *r, int32_t *list, uint32_t count) {
    for (uint32_t i = 1; i < count; i++) {
        int32_t x = list[i];
        uint32_t j = i;
        while (j > 0 && goes_before(&r->items[x], &r->items[list[j - 1]])) {
            list[j] = list[j - 1];
            j--;
        }
        list[j] = x;
    }
}

// The lowest offset, a multiple of align, where size bytes overlap none of
// the sorted ranges placed so far: 0, or the end of one of them aligned up.
static bool lowest_free(const struct range *placed, uint32_t count, uint64_t size,
                        uint64_t align, uint64_t *out) {
    for (uint32_t k = 0; k <= count; k++) {
        uint64_t at = 0;
        if (k != 0 && !align_up(placed[k - 1].end, align, &at)) {
            return false;
        }
        if (size > UINT64_MAX - at) {
            return false;
        }
        bool clear = true;
        for (uint32_t j = 0; j < count; j++) {
            if (placed[j].base < at + size && placed[j].end > at) {
                clear = false;
                break;
            }
        }
        if (clear) {
            *out = at;
            return true;
        }
    }
    return false;
}

static bool placeable_in(const struct item *it, int32_t window) {
    return it->container == window && !(is_window(it) && it->empty);
}

// PCI-to-PCI Bridge Architecture Specification 1.1, 3.2.5.8 and 3.2.5.9:
// windows come in 1 MiB granules.
static void plan_window(struct relocation *r, int32_t w) {
    struct item *window = &r->items[w];
    int32_t *list = kmalloc(r->item_count * sizeof(*list));
    uint32_t count = 0;
    for (uint32_t i = 0; i < r->item_count; i++) {
        if (placeable_in(&r->items[i], w)) {
            list[count++] = (int32_t)i;
        }
    }
    sort_for_placement(r, list, count);

    struct range *placed = kmalloc(count * sizeof(*placed));
    uint32_t placed_count = 0;
    uint64_t end = 0;
    uint64_t align = WINDOW_GRANULE;
    bool fits = true;
    for (uint32_t k = 0; k < count; k++) {
        struct item *it = &r->items[list[k]];
        uint64_t size = item_plan_size(it);
        uint64_t offset;
        if (!lowest_free(placed, placed_count, size, it->align, &offset)) {
            fits = false;
            break;
        }
        it->offset = offset;
        uint32_t at = placed_count;
        while (at > 0 && placed[at - 1].base > offset) {
            placed[at] = placed[at - 1];
            at--;
        }
        placed[at] = (struct range){offset, offset + size};
        placed_count++;
        if (offset + size > end) {
            end = offset + size;
        }
        if (it->align > align) {
            align = it->align;
        }
    }
    window->align = align;
    if (!fits || !align_up(end, WINDOW_GRANULE, &window->plan_size)) {
        window->plan_size = UINT64_MAX;
    }
    kfree(placed);
    kfree(list);
}

static void plan_windows(struct relocation *r, uint32_t root) {
    int32_t *list = kmalloc(r->item_count * sizeof(*list));
    uint32_t count = 0;
    for (uint32_t i = 0; i < r->item_count; i++) {
        const struct item *it = &r->items[i];
        if (it->root == root && is_window(it) && is_movable(it) && !it->empty) {
            list[count++] = (int32_t)i;
        }
    }
    sort_by_depth(r, list, count, true);
    for (uint32_t k = 0; k < count; k++) {
        plan_window(r, list[k]);
    }
    kfree(list);
}

static void space_build(const struct relocation *r, uint32_t root, const struct host_bridge *hb,
                        struct range_list *pref, struct range_list *nonpref) {
    for (uint32_t i = 0; i < hb->window_count; i++) {
        const struct window *w = &hb->windows[i];
        uint64_t base = w->base < ONE_MIB ? ONE_MIB : w->base;
        uint64_t end = w->end > FOUR_GIB ? FOUR_GIB : w->end;
        list_add(w->prefetchable ? pref : nonpref, base, end);
    }
    for (uint32_t i = 0; i < r->claimed.count; i++) {
        list_remove(pref, r->claimed.r[i].base, r->claimed.r[i].end);
        list_remove(nonpref, r->claimed.r[i].base, r->claimed.r[i].end);
    }
    // All of the root bus's movable items are moved, closed, or above 4 GiB,
    // and every other item stays where it is.
    for (uint32_t i = 0; i < r->item_count; i++) {
        const struct item *it = &r->items[i];
        if (it->closed || (it->root == root && is_movable(it))) {
            continue;
        }
        list_remove(pref, it->base, item_end(it));
        list_remove(nonpref, it->base, item_end(it));
    }
}

static bool space_take(struct range_list *from, struct range_list *other, uint64_t size,
                       uint64_t align, uint64_t *at) {
    if (!list_fit(from, size, align, at)) {
        return false;
    }
    list_remove(from, *at, *at + size);
    list_remove(other, *at, *at + size);
    return true;
}

static bool in_prefetchable_window(const struct host_bridge *hb, const struct item *it) {
    for (uint32_t i = 0; i < hb->window_count; i++) {
        const struct window *w = &hb->windows[i];
        if (w->prefetchable && it->base >= w->base && item_end(it) <= w->end) {
            return true;
        }
    }
    return false;
}

static void log_kept(const struct relocation *r, uint32_t root) {
    for (uint32_t i = 0; i < r->item_count; i++) {
        const struct item *it = &r->items[i];
        if (it->root != root || it->container != CONTAINER_ROOT || it->fixed == FIXED_NOT
            || item_end(it) > FOUR_GIB) {
            continue;
        }
        char name[NAME_SIZE];
        item_name(it, name);
        printf("PCI %s: keeps 0x%llx-0x%llx, %s\n", name, (unsigned long long)it->base,
               (unsigned long long)(item_end(it) - 1), fixed_reason(it->fixed));
    }
}

// Places the root bus's items in the usable space, and the items of the
// windows that move at their offsets in them. Returns PLAN_DONE, or why the
// root bus stays as it is, with the item that found no room in *blocking.
static int plan_root(struct relocation *r, uint32_t root, const struct host_bridge *hb,
                     int32_t *blocking) {
    for (uint32_t i = 0; i < r->item_count; i++) {
        struct item *it = &r->items[i];
        if (it->root == root) {
            it->moving = false;
            it->closing = false;
        }
    }
    plan_windows(r, root);

    struct range_list pref = {.r = NULL};
    struct range_list nonpref = {.r = NULL};
    space_build(r, root, hb, &pref, &nonpref);

    int32_t *list = kmalloc(r->item_count * sizeof(*list));
    uint32_t count = 0;
    for (uint32_t i = 0; i < r->item_count; i++) {
        const struct item *it = &r->items[i];
        if (it->root == root && is_movable(it) && placeable_in(it, CONTAINER_ROOT)) {
            list[count++] = (int32_t)i;
        }
    }
    sort_for_placement(r, list, count);

    bool high_placed = false;
    int status = PLAN_DONE;
    for (uint32_t k = 0; k < count && status == PLAN_DONE; k++) {
        struct item *it = &r->items[list[k]];
        uint64_t size = item_plan_size(it);
        uint64_t at;
        bool placed;
        if (it->prefetchable) {
            placed = space_take(&pref, &nonpref, size, it->align, &at)
                     || space_take(&nonpref, &pref, size, it->align, &at);
        } else {
            placed = space_take(&nonpref, &pref, size, it->align, &at)
                     || (in_prefetchable_window(hb, it)
                         && space_take(&pref, &nonpref, size, it->align, &at));
        }
        if (placed) {
            it->moving = true;
            it->new_base = at;
            high_placed |= item_end(it) > FOUR_GIB;
        } else if (it->base < FOUR_GIB) {
            // Below 4 GiB, it could clash with what moves there.
            *blocking = list[k];
            status = PLAN_NO_ROOM;
        }
    }
    list_free(&pref);
    list_free(&nonpref);
    if (status == PLAN_DONE && !high_placed) {
        status = PLAN_NO_GAIN;
    }
    if (status != PLAN_DONE) {
        for (uint32_t k = 0; k < count; k++) {
            r->items[list[k]].moving = false;
        }
        kfree(list);
        return status;
    }

    // Each window has its place before what it holds.
    count = 0;
    for (uint32_t i = 0; i < r->item_count; i++) {
        if (r->items[i].root == root && is_movable(&r->items[i])) {
            list[count++] = (int32_t)i;
        }
    }
    sort_by_depth(r, list, count, false);
    for (uint32_t k = 0; k < count; k++) {
        struct item *it = &r->items[list[k]];
        if (is_window(it) && it->empty) {
            it->closing = it->base < FOUR_GIB;
        } else if (it->container >= 0 && r->items[it->container].moving) {
            it->moving = true;
            it->new_base = r->items[it->container].new_base + it->offset;
        }
    }
    kfree(list);
    return PLAN_DONE;
}

static bool item_changes(const struct item *it) {
    if (it->closing) {
        return true;
    }
    if (!it->moving) {
        return false;
    }
    return it->new_base != it->base || (is_window(it) && it->plan_size != it->size);
}

static void write_checked(struct pci_function *f, uint32_t reg, unsigned size, uint32_t value) {
    pci_cfg_write(f->addr, reg, size, value);
    uint32_t now = pci_cfg_read(f->addr, reg, size);
    if (now != value) {
        printf("PCI " ADDR_FMT ": register 0x%02x reads 0x%08x after writing 0x%08x\n",
               ADDR_ARGS(f->addr), reg, now, value);
    }
}

// PCI Local Bus Specification 3.0, 6.2.5.1 and 6.2.5.2: the low bits are
// read-only, or the ROM's enable bit and reserved bits.
static void program_bar(struct item *it, uint64_t base) {
    struct pci_function *f = it->f;
    if (it->kind == ITEM_ROM) {
        uint32_t old = pci_cfg_read(f->addr, f->rom.reg, 4);
        write_checked(f, f->rom.reg, 4, ((uint32_t)base & ROM_ADDRESS) | (old & ~ROM_ADDRESS));
        return;
    }
    const struct pci_bar *bar = &f->bars[it->bar];
    uint32_t old = pci_cfg_read(f->addr, bar->reg, 4);
    if (bar->kind == PCI_BAR_MEM64) {
        write_checked(f, bar->reg + 4u, 4, (uint32_t)(base >> 32));
    }
    write_checked(f, bar->reg, 4,
                  ((uint32_t)base & BAR_MEMORY_ADDRESS) | (old & ~BAR_MEMORY_ADDRESS));
}

// PCI-to-PCI Bridge Architecture Specification 1.1, 3.2.5.8 to 3.2.5.10: each
// register's bits 15:4 hold address bits 31:20, bits 3:0 are read-only, and a
// limit's address bits 19:0 are ones.
static void program_window(struct item *it, uint64_t base, uint64_t limit) {
    struct pci_function *f = it->f;
    uint32_t reg = it->kind == ITEM_PREF_WINDOW ? BRIDGE_PREF : BRIDGE_MEMORY;
    uint32_t old = pci_cfg_read(f->addr, reg, 4);
    uint32_t value = (uint32_t)((base >> 16) & 0xfff0) | ((uint32_t)((limit >> 16) & 0xfff0) << 16)
                     | (old & WINDOW_TYPE_BITS);
    if (it->kind == ITEM_PREF_WINDOW && (old & WINDOW_TYPE) == WINDOW_PREF_64) {
        write_checked(f, BRIDGE_PREF_BASE_UPPER, 4, (uint32_t)(base >> 32));
        write_checked(f, BRIDGE_PREF_LIMIT_UPPER, 4, (uint32_t)(limit >> 32));
    }
    write_checked(f, reg, 4, value);
}

static void record_update(struct item *it) {
    struct pci_function *f = it->f;
    uint64_t base = it->closing ? WINDOW_CLOSED_BASE : it->new_base;
    uint64_t limit = it->closing ? WINDOW_CLOSED_LIMIT : it->new_base + it->plan_size - 1;
    switch (it->kind) {
        case ITEM_BAR: {
            f->bars[it->bar].base = base;
            break;
        }
        case ITEM_ROM: {
            f->rom.base = base;
            break;
        }
        case ITEM_MEM_WINDOW: {
            f->mem_base = (uint32_t)base;
            f->mem_limit = (uint32_t)limit;
            break;
        }
        default: {
            f->pref_base = base;
            f->pref_limit = limit;
            break;
        }
    }
    if (it->closing) {
        it->closed = true;
        it->size = 0;
    } else {
        it->base = base;
        if (is_window(it)) {
            it->size = it->plan_size;
        }
    }
}

static void framebuffer_follow(const struct item *it, uint64_t old_base) {
    struct handoff_framebuffer *fb = &core_handoff->video.framebuffer;
    if (!fb->present || it->kind != ITEM_BAR || fb->base < old_base
        || fb->base - old_base >= it->size) {
        return;
    }
    uint64_t base = fb->base - old_base + it->new_base;
    printf("Framebuffer moves with PCI " ADDR_FMT " BAR%u, from 0x%llx to 0x%llx\n",
           ADDR_ARGS(it->f->addr), it->bar, (unsigned long long)fb->base,
           (unsigned long long)base);
    fb->base = base;
}

// The functions whose registers change stop decoding memory until all of the
// root bus's are written, PCI Local Bus Specification 3.0, 6.2.2.
static void program_root(struct relocation *r, uint32_t root) {
    struct saved_command *saved = kmalloc(r->topology->function_count * sizeof(*saved));
    uint32_t saved_count = 0;
    for (uint32_t i = 0; i < r->item_count; i++) {
        struct item *it = &r->items[i];
        if (it->root != root || !item_changes(it)) {
            continue;
        }
        if (saved_count != 0 && saved[saved_count - 1].f == it->f) {
            continue;
        }
        uint16_t command = (uint16_t)pci_cfg_read(it->f->addr, PCI_COMMAND, 2);
        saved[saved_count++] = (struct saved_command){it->f, command};
        if (command & COMMAND_MEMORY) {
            write_checked(it->f, PCI_COMMAND, 2, command & ~COMMAND_MEMORY);
        }
    }

    for (uint32_t i = 0; i < r->item_count; i++) {
        struct item *it = &r->items[i];
        if (it->root != root || !item_changes(it)) {
            continue;
        }
        char name[NAME_SIZE];
        item_name(it, name);
        if (it->closing) {
            printf("PCI %s: closed, was 0x%llx-0x%llx\n", name, (unsigned long long)it->base,
                   (unsigned long long)(item_end(it) - 1));
            program_window(it, WINDOW_CLOSED_BASE, WINDOW_CLOSED_LIMIT);
        } else {
            uint64_t size = item_plan_size(it);
            printf("PCI %s: 0x%llx-0x%llx -> 0x%llx-0x%llx\n", name,
                   (unsigned long long)it->base, (unsigned long long)(item_end(it) - 1),
                   (unsigned long long)it->new_base,
                   (unsigned long long)(it->new_base + size - 1));
            if (is_window(it)) {
                program_window(it, it->new_base, it->new_base + size - 1);
            } else {
                program_bar(it, it->new_base);
            }
        }
    }

    for (uint32_t i = 0; i < saved_count; i++) {
        if (saved[i].command & COMMAND_MEMORY) {
            write_checked(saved[i].f, PCI_COMMAND, 2, saved[i].command);
        }
    }
    kfree(saved);

    for (uint32_t i = 0; i < r->item_count; i++) {
        struct item *it = &r->items[i];
        if (it->root != root || !item_changes(it)) {
            continue;
        }
        uint64_t old_base = it->base;
        record_update(it);
        if (!it->closing) {
            framebuffer_follow(it, old_base);
        }
    }
}

// What a window left above 4 GiB holds goes without saying.
static void log_left_high(const struct relocation *r, uint32_t root, bool laid_out) {
    for (uint32_t i = 0; i < r->item_count; i++) {
        const struct item *it = &r->items[i];
        if (it->root != root || it->container == CONTAINER_NONE || it->closed
            || item_end(it) <= FOUR_GIB
            || (it->container >= 0 && item_end(&r->items[it->container]) > FOUR_GIB)) {
            continue;
        }
        const char *reason = "";
        if (it->fixed != FIXED_NOT) {
            reason = fixed_reason(it->fixed);
        } else if (is_window(it) && it->empty) {
            reason = "empty";
        } else if (laid_out) {
            reason = "no room below 4 GiB";
        }
        char name[NAME_SIZE];
        item_name(it, name);
        printf("PCI %s: stays above 4 GiB at 0x%llx-0x%llx%s%s\n", name,
               (unsigned long long)it->base, (unsigned long long)(item_end(it) - 1),
               reason[0] != '\0' ? ", " : "", reason);
    }
}

// CardBus windows and the BARs of decoding virtual functions are not in the
// records, so nothing on such a root bus may move.
static bool root_blocked(const struct relocation *r, uint32_t root) {
    const struct pci_topology *t = r->topology;
    const struct pci_root_bus *rb = &t->root_buses[root];
    for (uint32_t i = 0; i < t->function_count; i++) {
        const struct pci_function *f = &t->functions[i];
        if (r->function_root[i] != root) {
            continue;
        }
        if (f->header_type == HEADER_CARDBUS) {
            printf("PCI root bus %04x:%02x: CardBus bridge " ADDR_FMT ", left as it is\n",
                   rb->segment, rb->bus, ADDR_ARGS(f->addr));
            return true;
        }
        if (f->sriov_ext_cap != 0
            && (pci_cfg_read(f->addr, f->sriov_ext_cap + SRIOV_CONTROL, 2) & SRIOV_VF_MSE)) {
            printf("PCI root bus %04x:%02x: virtual functions of " ADDR_FMT " decode memory, "
                   "left as it is\n", rb->segment, rb->bus, ADDR_ARGS(f->addr));
            return true;
        }
    }
    return false;
}

static bool root_wanted(const struct relocation *r, uint32_t root) {
    for (uint32_t i = 0; i < r->item_count; i++) {
        const struct item *it = &r->items[i];
        if (it->root == root && is_movable(it) && !(is_window(it) && it->empty)
            && item_end(it) > FOUR_GIB) {
            return true;
        }
    }
    return false;
}

static void root_lay_out(struct relocation *r, uint32_t root, const struct host_bridge *hb) {
    const struct pci_root_bus *rb = &r->topology->root_buses[root];
    int32_t blocking = -1;
    int status = plan_root(r, root, hb, &blocking);
    switch (status) {
        case PLAN_DONE: {
            printf("PCI root bus %04x:%02x: laid out again\n", rb->segment, rb->bus);
            break;
        }
        case PLAN_NO_ROOM: {
            const struct item *it = &r->items[blocking];
            char name[NAME_SIZE];
            item_name(it, name);
            printf("PCI root bus %04x:%02x: no room below 4 GiB for %s, 0x%llx-0x%llx, "
                   "left as it is\n", rb->segment, rb->bus, name, (unsigned long long)it->base,
                   (unsigned long long)(item_end(it) - 1));
            break;
        }
        default: {
            printf("PCI root bus %04x:%02x: nothing above 4 GiB fits below it, left as it is\n",
                   rb->segment, rb->bus);
            break;
        }
    }
    log_kept(r, root);
    if (status == PLAN_DONE) {
        program_root(r, root);
    }
    log_left_high(r, root, status == PLAN_DONE);
}

static void root_process(struct relocation *r, uint32_t root) {
    const struct pci_root_bus *rb = &r->topology->root_buses[root];
    struct host_bridge *hb = host_bridge_for(r, rb->segment, rb->bus);
    if (hb == NULL) {
        printf("PCI root bus %04x:%02x: no host bridge, left as it is\n", rb->segment, rb->bus);
    } else if (hb->low_window_count == 0) {
        printf("PCI root bus %04x:%02x: no windows below 4 GiB, left as it is\n", rb->segment,
               rb->bus);
    } else if (root_blocked(r, root)) {
        hb->undescribed = true;
    } else if (root_wanted(r, root)) {
        root_lay_out(r, root, hb);
        return;
    } else {
        printf("PCI root bus %04x:%02x: nothing to move above 4 GiB\n", rb->segment, rb->bus);
    }
    log_left_high(r, root, false);
}

static void relocation_free(struct relocation *r) {
    for (uint32_t i = 0; i < r->bridge_count; i++) {
        kfree(r->bridges[i].windows);
    }
    kfree(r->bridges);
    kfree(r->items);
    kfree(r->function_root);
    kfree(r->mem_window);
    kfree(r->pref_window);
    list_free(&r->claimed);
}

static void used_add(uint64_t base, uint64_t end) {
    list_add(&reserve_state.used, base < ONE_MIB ? ONE_MIB : base,
             end > FOUR_GIB ? FOUR_GIB : end);
}

// A root bus with a function decoding memory the records lack may decode
// anywhere in its host bridge's windows. A disabled expansion ROM BAR still
// holds the address an operating system would enable it at.
static void reserve_keep(const struct relocation *r) {
    struct reserve_state *s = &reserve_state;
    uint32_t window_total = 0;
    for (uint32_t i = 0; i < r->bridge_count; i++) {
        window_total += r->bridges[i].window_count;
    }
    s->bridges = kmalloc(r->bridge_count * sizeof(*s->bridges));
    s->windows = kmalloc(window_total * sizeof(*s->windows));
    s->bridge_count = r->bridge_count;
    uint32_t window_count = 0;
    for (uint32_t i = 0; i < r->bridge_count; i++) {
        const struct host_bridge *hb = &r->bridges[i];
        struct reserve_bridge *b = &s->bridges[i];
        *b = (struct reserve_bridge){
            .segment = hb->segment,
            .bus = hb->bus,
            .undescribed = hb->undescribed,
            .windows = &s->windows[window_count],
        };
        for (uint32_t k = 0; k < hb->window_count; k++) {
            const struct window *w = &hb->windows[k];
            uint64_t base = w->base < ONE_MIB ? ONE_MIB : w->base;
            uint64_t end = w->end > FOUR_GIB ? FOUR_GIB : w->end;
            if (base >= end) {
                continue;
            }
            b->windows[b->window_count++] = (struct window){base, end, w->prefetchable};
            if (hb->undescribed) {
                used_add(base, end);
            }
        }
        window_count += b->window_count;
    }

    for (uint32_t i = 0; i < r->claimed.count; i++) {
        used_add(r->claimed.r[i].base, r->claimed.r[i].end);
    }
    const struct pci_topology *t = r->topology;
    for (uint32_t i = 0; i < t->function_count; i++) {
        const struct pci_function *f = &t->functions[i];
        for (uint8_t n = 0; n < f->bar_count && n < 6; n++) {
            const struct pci_bar *bar = &f->bars[n];
            if ((bar->kind == PCI_BAR_MEM32 || bar->kind == PCI_BAR_MEM64) && bar->size != 0
                && bar->base != 0) {
                used_add(bar->base, range_end(bar->base, bar->size));
            }
        }
        if (f->rom.kind == PCI_BAR_MEM32 && f->rom.size != 0 && f->rom.base != 0) {
            used_add(f->rom.base, range_end(f->rom.base, f->rom.size));
        }
        if (f->header_type == HEADER_BRIDGE) {
            if (f->mem_limit >= f->mem_base) {
                used_add(f->mem_base, (uint64_t)f->mem_limit + 1);
            }
            if (f->pref_limit >= f->pref_base) {
                used_add(f->pref_base, range_end(f->pref_limit, 1));
            }
        }
    }
}

static void reserve_free(void) {
    kfree(reserve_state.bridges);
    kfree(reserve_state.windows);
    list_free(&reserve_state.used);
    reserve_state = (struct reserve_state){.bridges = NULL};
}

void pci_relocate(void) {
    reserve_free();
    const struct pci_topology *t = pci_enum_scan();
    pci_enum_shrink_rebars(BAR_SIZE_MAX);
    if (!acpi_init()) {
        printf("PCI: no ACPI namespace, so the BARs stay where they are\n");
        return;
    }
    if (t->function_count == 0) {
        return;
    }

    struct relocation r = {.topology = t};
    claim_memory_map(&r);
    claim_mcfg(&r);
    claim_madt(&r);
    claim_hpets(&r);
    claim_iommus(&r);
    claim(&r, MSI_BASE, MSI_SIZE);
    uacpi_namespace_for_each_child(uacpi_namespace_root(), device_found, NULL,
                                   UACPI_OBJECT_DEVICE_BIT, UACPI_MAX_DEPTH_ANY, &r);
    printf("PCI: %u ranges of memory in use, %u devices whose _CRS fails\n", r.claimed.count,
           r.crs_failures);

    items_build(&r);
    containers_find(&r);
    fixed_find(&r);
    empty_find(&r);
    for (uint32_t i = 0; i < t->root_bus_count; i++) {
        root_process(&r, i);
    }

    const struct handoff_framebuffer *fb = &core_handoff->video.framebuffer;
    if (fb->present && range_end(fb->base, fb->size) > FOUR_GIB) {
        printf("Framebuffer stays above 4 GiB at 0x%llx\n", (unsigned long long)fb->base);
    }
    reserve_keep(&r);
    relocation_free(&r);
}

// The lowest address where size bytes aligned to size lie in one of the
// bridge's windows of this kind, clear of what is used.
static const struct window *reserve_fit(const struct reserve_bridge *b, bool prefetchable,
                                        uint64_t size, uint64_t *at) {
    const struct window *found = NULL;
    for (uint32_t i = 0; i < b->window_count; i++) {
        const struct window *w = &b->windows[i];
        uint64_t x;
        if (w->prefetchable == prefetchable
            && list_fit_between(&reserve_state.used, w->base, w->end, size, size, &x)
            && (found == NULL || x < *at)) {
            found = w;
            *at = x;
        }
    }
    return found;
}

bool pci_relocate_reserve(uint16_t segment, uint8_t bus, uint64_t size, uint64_t *base) {
    const struct reserve_bridge *b = NULL;
    for (uint32_t i = 0; i < reserve_state.bridge_count && b == NULL; i++) {
        if (reserve_state.bridges[i].segment == segment && reserve_state.bridges[i].bus == bus) {
            b = &reserve_state.bridges[i];
        }
    }
    const char *why = NULL;
    const struct window *w = NULL;
    uint64_t at = 0;
    if (size == 0 || (size & (size - 1)) != 0) {
        why = "not a power of two";
    } else if (b == NULL) {
        why = "no host bridge";
    } else if (b->window_count == 0) {
        why = "no windows below 4 GiB";
    } else if (b->undescribed) {
        why = "not all the memory it decodes is known";
    } else {
        // Non-prefetchable space goes to the BARs that need it.
        w = reserve_fit(b, true, size, &at);
        if (w == NULL) {
            w = reserve_fit(b, false, size, &at);
        }
        if (w == NULL) {
            why = "no room below 4 GiB";
        }
    }
    if (why != NULL) {
        printf("PCI root bus %04x:%02x: cannot reserve 0x%llx bytes, %s\n", segment, bus,
               (unsigned long long)size, why);
        return false;
    }

    list_add(&reserve_state.used, at, at + size);
    printf("PCI root bus %04x:%02x: reserved 0x%llx-0x%llx in %swindow 0x%llx-0x%llx\n",
           segment, bus, (unsigned long long)at, (unsigned long long)(at + size - 1),
           w->prefetchable ? "prefetchable " : "", (unsigned long long)w->base,
           (unsigned long long)(w->end - 1));
    *base = at;
    return true;
}
