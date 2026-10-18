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
#include <uacpi/acpi.h>
#include <uacpi/namespace.h>
#include <uacpi/resources.h>
#include <uacpi/status.h>
#include <uacpi/tables.h>
#include <uacpi/types.h>
#include <uacpi/uacpi.h>
#include <uacpi/utilities.h>
#include <memory.h>
#include <hw/acpi.h>
#include <hw/pci_host.h>
#include <iommu/iommu_tables.h>
#include <lib/heap.h>
#include <lib/log.h>

#define ONE_MIB 0x100000ull
#define FOUR_GIB 0x100000000ull
#define CLAIM_GRANULE 0x1000ull

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

struct host_bridge {
    uacpi_namespace_node *node;
    uint16_t segment;
    uint8_t bus;
    // The memory windows of its _CRS, as given, and how many lie below 4 GiB.
    struct pci_host_window *windows;
    uint32_t window_count;
    uint32_t window_capacity;
    uint32_t low_window_count;
};

struct scan {
    struct range_list claimed;
    struct range_list private;
    uint32_t crs_failures;
    struct host_bridge *bridges;
    uint32_t bridge_count;
    uint32_t bridge_capacity;
};

struct crs_scan {
    struct scan *r;
    // NULL for a device other than a host bridge.
    struct host_bridge *hb;
    bool bus_found;
    uint64_t bus;
};

static const char *const host_bridge_ids[] = {"PNP0A03", "PNP0A08", NULL};

static struct pci_host host;
static bool scanned;

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

static void claim_into(struct range_list *list, uint64_t base, uint64_t size) {
    if (size == 0) {
        return;
    }
    uint64_t end;
    if (!align_up(range_end(base, size), CLAIM_GRANULE, &end)) {
        end = UINT64_MAX;
    }
    list_add(list, base & ~(CLAIM_GRANULE - 1), end);
}

static void claim(struct scan *r, uint64_t base, uint64_t size) {
    claim_into(&r->claimed, base, size);
}

static void log_failure(uacpi_namespace_node *node, const char *name, uacpi_status status) {
    const char *path = uacpi_namespace_node_generate_absolute_path(node);
    printf("Cannot evaluate %s.%s: %s\n", path != NULL ? path : "?", name,
           uacpi_status_to_string(status));
    if (path != NULL) {
        uacpi_free_absolute_path(path);
    }
}

static void private_mcfg(struct scan *r) {
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
            claim_into(&r->private, entry.address + ((uint64_t)entry.start_bus << 20),
                  ((uint64_t)entry.end_bus - entry.start_bus + 1) << 20);
        }
    }
    uacpi_table_unref(&table);
}

static void claim_madt(struct scan *r) {
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

static void claim_hpets(struct scan *r) {
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

static void private_iommus(struct scan *r) {
    const struct iommu_tables *tables = iommu_tables_parse();
    for (uint32_t i = 0; i < tables->unit_count; i++) {
        const struct iommu_unit *u = &tables->units[i];
        if (tables->kind == IOMMU_KIND_VTD) {
            claim_into(&r->private, u->base, u->vtd_size);
        } else if (tables->kind == IOMMU_KIND_AMD) {
            claim_into(&r->private, u->base, u->base % AMD_IOMMU_MMIO_LARGE == 0
                              ? AMD_IOMMU_MMIO_LARGE : AMD_IOMMU_MMIO_SMALL);
        }
    }
}

static void window_add(struct host_bridge *hb, uint64_t base, uint64_t end, bool prefetchable) {
    if (hb->window_count == hb->window_capacity) {
        uint32_t capacity = hb->window_capacity == 0 ? 8 : hb->window_capacity * 2;
        struct pci_host_window *windows = kmalloc(capacity * sizeof(*windows));
        if (hb->window_count != 0) {
            memcpy(windows, hb->windows, hb->window_count * sizeof(*windows));
        }
        kfree(hb->windows);
        hb->windows = windows;
        hb->window_capacity = capacity;
    }
    hb->windows[hb->window_count++] = (struct pci_host_window){base, end, prefetchable};
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
        const struct pci_host_window *w = &hb->windows[i];
        uint64_t base = w->base < ONE_MIB ? ONE_MIB : w->base;
        uint64_t end = w->end > FOUR_GIB ? FOUR_GIB : w->end;
        if (base < end) {
            printf("PCI host bridge %s: window 0x%llx-0x%llx%s\n", path,
                   (unsigned long long)base, (unsigned long long)(end - 1),
                   w->prefetchable ? ", prefetchable" : "");
        }
    }
}

static bool host_bridge_known(const struct scan *r, const struct host_bridge *hb,
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

static void host_bridge_found(struct scan *r, uacpi_namespace_node *node) {
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
    struct scan *r = user;
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

static void ranges_out(const struct range_list *l, struct pci_host_range **out, uint32_t *count) {
    *out = kmalloc((l->count != 0 ? l->count : 1) * sizeof(**out));
    for (uint32_t i = 0; i < l->count; i++) {
        (*out)[i] = (struct pci_host_range){l->r[i].base, l->r[i].end};
    }
    *count = l->count;
}

const struct pci_host *pci_host_scan(void) {
    if (scanned) {
        return &host;
    }
    scanned = true;
    if (!acpi_init()) {
        printf("PCI: no ACPI namespace, so no host bridges\n");
        return &host;
    }

    struct scan r = {.crs_failures = 0};
    private_mcfg(&r);
    private_iommus(&r);
    claim_madt(&r);
    claim_hpets(&r);
    claim(&r, MSI_BASE, MSI_SIZE);
    uacpi_namespace_for_each_child(uacpi_namespace_root(), device_found, NULL,
                                   UACPI_OBJECT_DEVICE_BIT, UACPI_MAX_DEPTH_ANY, &r);
    // Devices may list them among what they use, as motherboard resources do.
    for (uint32_t i = 0; i < r.private.count; i++) {
        list_remove(&r.claimed, r.private.r[i].base, r.private.r[i].end);
    }
    printf("PCI: %u ranges of memory claimed, %u devices whose _CRS fails\n", r.claimed.count,
           r.crs_failures);

    ranges_out(&r.claimed, &host.claimed, &host.claimed_count);
    ranges_out(&r.private, &host.private, &host.private_count);
    host.bridges = kmalloc((r.bridge_count != 0 ? r.bridge_count : 1) * sizeof(*host.bridges));
    for (uint32_t i = 0; i < r.bridge_count; i++) {
        const struct host_bridge *hb = &r.bridges[i];
        host.bridges[i] = (struct pci_host_bridge){
            .node = hb->node,
            .segment = hb->segment,
            .bus = hb->bus,
            .windows = hb->windows,
            .window_count = hb->window_count,
        };
    }
    host.bridge_count = r.bridge_count;
    kfree(r.bridges);
    list_free(&r.claimed);
    list_free(&r.private);
    return &host;
}
