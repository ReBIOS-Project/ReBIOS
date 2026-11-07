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
#include <uacpi/acpi.h>
#include <uacpi/namespace.h>
#include <uacpi/platform/types.h>
#include <uacpi/resources.h>
#include <uacpi/status.h>
#include <uacpi/tables.h>
#include <uacpi/types.h>
#include <uacpi/utilities.h>
#include <hw/pci_host.h>
#include <hw/acpi.h>
#include <iommu/iommu_tables.h>
#include <lib/heap.h>
#include <lib/log.h>
#include <lib/util.h>
#include <memory.h>

#define CLAIM_GRANULE ((uint64_t)0x1000)

// AMD IOMMU 48882 rev. 3.11, Capability Offset 04h.
#define AMD_IOMMU_MMIO_LARGE ((uint64_t)0x80000)
#define AMD_IOMMU_MMIO_SMALL ((uint64_t)0x4000)

#define APIC_MMIO_SIZE ((uint64_t)0x1000)
#define HPET_MMIO_SIZE ((uint64_t)0x1000)
// SDM 325462-093 Vol. 3A, 13.11.1: the processors take every access to this
// range for an interrupt message.
#define MSI_BASE ((uint64_t)0xfee00000)
#define MSI_SIZE ((uint64_t)0x100000)

struct range {
    uintptr_t base;
    uintptr_t end;
};

// Sorted, neither overlapping nor touching.
struct range_list {
    struct range *r;
    size_t count;
    size_t capacity;
};

struct host_bridge {
    uacpi_namespace_node *node;
    uint16_t segment;
    uint8_t bus;
    // The memory windows of its _CRS, as given, and how many lie below 4 GiB.
    struct pci_host_window *windows;
    size_t window_count;
    size_t window_capacity;
    size_t low_window_count;
};

struct scan {
    struct range_list claimed;
    struct range_list private;
    uint32_t crs_failures;
    struct host_bridge *bridges;
    size_t bridge_count;
    size_t bridge_capacity;
};

struct crs_scan {
    struct scan *r;
    // NULL for a device other than a host bridge.
    struct host_bridge *hb;
    bool bus_found;
    uint64_t bus;
};

static struct pci_host host;
static bool scanned;

static uintptr_t range_end(uintptr_t base, size_t size) {
    return size > UINT64_MAX - base ? UINT64_MAX : base + size;
}

static void list_grow(struct range_list *l) {
    l->r = heap_grow(l->r, l->count, &l->capacity, sizeof(struct range), 32);
}

static void list_free(struct range_list *l) {
    kfree(l->r);
    l->r = NULL;
    l->count = 0;
    l->capacity = 0;
}

// Adds [base, end), merged with the ranges it overlaps or touches.
static void list_add(struct range_list *l, uintptr_t base, uintptr_t end) {
    if (base >= end) {
        return;
    }
    size_t first = 0;
    while (first < l->count && l->r[first].end < base) {
        first++;
    }
    size_t last = first;
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
        memmove(&l->r[first + 1], &l->r[first], (l->count - first) * sizeof(struct range));
        l->count++;
    } else {
        memmove(&l->r[first + 1], &l->r[last], (l->count - last) * sizeof(struct range));
        l->count -= last - first - 1;
    }
    l->r[first].base = base;
    l->r[first].end = end;
}

static void list_remove(struct range_list *l, uintptr_t base, uintptr_t end) {
    size_t i = 0;
    while (base < end && i < l->count) {
        struct range *x = &l->r[i];
        if (x->end <= base || x->base >= end) {
            i++;
        } else if (x->base < base && x->end > end) {
            uintptr_t old_end = x->end;
            x->end = base;
            list_grow(l);
            memmove(&l->r[i + 2], &l->r[i + 1], (l->count - i - 1) * sizeof(struct range));
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
            memmove(x, x + 1, (l->count - i - 1) * sizeof(struct range));
            l->count--;
        }
    }
}

static void claim_into(struct range_list *list, uintptr_t base, size_t size) {
    if (size == 0) {
        return;
    }
    uintptr_t last = range_end(base, size);
    uintptr_t end = align_up(last, CLAIM_GRANULE);
    if (end < last) {
        end = UINT64_MAX;
    }
    list_add(list, align_down(base, CLAIM_GRANULE), end);
}

static void claim(struct scan *r, uintptr_t base, size_t size) {
    claim_into(&r->claimed, base, size);
}

static void private_mcfg(struct scan *r) {
    struct uacpi_table table;
    if (uacpi_table_find_by_signature(ACPI_MCFG_SIGNATURE, &table) != UACPI_STATUS_OK) {
        return;
    }
    size_t length = table.hdr->length;
    for (size_t off = sizeof(struct acpi_mcfg);
         off <= length && length - off >= sizeof(struct acpi_mcfg_allocation);
         off += sizeof(struct acpi_mcfg_allocation)) {
        struct acpi_mcfg_allocation entry;
        memcpy(&entry, (const uint8_t *)table.ptr + off, sizeof(struct acpi_mcfg_allocation));
        if (entry.start_bus <= entry.end_bus) {
            claim_into(
                &r->private, entry.address + ((uint64_t)entry.start_bus << 20),
                ((uint64_t)entry.end_bus - entry.start_bus + 1) << 20
            );
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
    size_t length = table.hdr->length;
    if (length < sizeof(struct acpi_madt)) {
        uacpi_table_unref(&table);
        return;
    }
    struct acpi_madt header;
    memcpy(&header, madt, sizeof(struct acpi_madt));
    uintptr_t lapic = header.local_interrupt_controller_address;

    size_t off = sizeof(struct acpi_madt);
    while (off <= length && length - off >= sizeof(struct acpi_entry_hdr)) {
        uint8_t type = madt[off];
        size_t entry_length = madt[off + 1];
        if (entry_length < sizeof(struct acpi_entry_hdr) || entry_length > length - off) {
            break;
        }
        if (type == ACPI_MADT_ENTRY_TYPE_IOAPIC
         && entry_length >= sizeof(struct acpi_madt_ioapic)) {
            struct acpi_madt_ioapic entry;
            memcpy(&entry, madt + off, sizeof(struct acpi_madt_ioapic));
            claim(r, entry.address, APIC_MMIO_SIZE);
        } else if (type == ACPI_MADT_ENTRY_TYPE_LAPIC_ADDRESS_OVERRIDE
                && entry_length >= sizeof(struct acpi_madt_lapic_address_override)) {
            struct acpi_madt_lapic_address_override entry;
            memcpy(&entry, madt + off, sizeof(struct acpi_madt_lapic_address_override));
            lapic = entry.address;
        }
        off += entry_length;
    }
    claim(r, lapic, APIC_MMIO_SIZE);
    uacpi_table_unref(&table);
}

static void claim_hpets(struct scan *r) {
    struct uacpi_table table;
    for (size_t n = 0;
         uacpi_table_find_nth_by_signature(ACPI_HPET_SIGNATURE, n, &table) == UACPI_STATUS_OK;
         n++) {
        if (table.hdr->length >= sizeof(struct acpi_hpet)) {
            struct acpi_hpet hpet;
            memcpy(&hpet, table.ptr, sizeof(struct acpi_hpet));
            if (hpet.address.address_space_id == ACPI_AS_ID_SYS_MEM) {
                claim(r, hpet.address.address, HPET_MMIO_SIZE);
            }
        }
        uacpi_table_unref(&table);
    }
}

static void private_iommus(struct scan *r) {
    const struct iommu_tables *tables = iommu_tables_parse();
    for (size_t i = 0; i < tables->unit_count; i++) {
        const struct iommu_tables_unit *u = &tables->units[i];
        if (tables->kind == IOMMU_TABLES_KIND_VTD) {
            claim_into(&r->private, u->base, u->vtd_size);
        } else if (tables->kind == IOMMU_TABLES_KIND_AMD) {
            claim_into(
                &r->private, u->base,
                u->base % AMD_IOMMU_MMIO_LARGE == 0 ? AMD_IOMMU_MMIO_LARGE : AMD_IOMMU_MMIO_SMALL
            );
        }
    }
}

static void window_add(struct host_bridge *hb, uintptr_t base, uintptr_t end, bool prefetchable) {
    hb->windows = heap_grow(
        hb->windows, hb->window_count, &hb->window_capacity, sizeof(struct pci_host_window), 8
    );
    hb->windows[hb->window_count++] = (struct pci_host_window){ base, end, prefetchable };
}

static void crs_address(
    struct crs_scan *s, const uacpi_resource_address_common *common, uintptr_t minimum,
    size_t length, uint64_t translation
) {
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
        const char *path = acpi_path(s->hb->node);
        printf(
            "PCI host bridge %s: window 0x%llx+0x%llx translated by 0x%llx, not used\n", path,
            (unsigned long long)minimum, (unsigned long long)length,
            (unsigned long long)translation
        );
        acpi_path_free(path);
        return;
    }
    // ACPI 6.6, 6.4.3.5.5: for PCI windows, cacheable and write-combining mean
    // prefetchable too.
    window_add(
        s->hb, minimum, range_end(minimum, length),
        common->attribute.memory.caching != UACPI_NON_CACHEABLE
    );
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
            claim(s->r, (uint64_t)res->memory24.minimum << 8, (uint64_t)res->memory24.length << 8);
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

static void host_bridge_log(const struct host_bridge *hb, const char *path) {
    printf(
        "PCI host bridge %s: segment %04x, bus %02x, %s windows below 4 GiB\n", path, hb->segment,
        hb->bus, hb->low_window_count == 0 ? "no" : "with"
    );
    for (size_t i = 0; i < hb->window_count; i++) {
        const struct pci_host_window *w = &hb->windows[i];
        uintptr_t base = w->base < ONE_MIB ? ONE_MIB : w->base;
        uintptr_t end = w->end > FOUR_GIB ? FOUR_GIB : w->end;
        if (base < end) {
            printf(
                "PCI host bridge %s: window 0x%llx-0x%llx%s\n", path, (unsigned long long)base,
                (unsigned long long)(end - 1), w->prefetchable ? ", prefetchable" : ""
            );
        }
    }
}

static bool host_bridge_known(
    const struct scan *r, const struct host_bridge *hb, const char *name
) {
    for (size_t i = 0; i < r->bridge_count; i++) {
        if (r->bridges[i].segment == hb->segment && r->bridges[i].bus == hb->bus) {
            printf(
                "PCI host bridge %s: segment %04x, bus %02x has a host bridge already, ignoring "
                "this one\n", name, hb->segment, hb->bus
            );
            return true;
        }
    }
    return false;
}

static void host_bridge_found(struct scan *r, uacpi_namespace_node *node) {
    struct host_bridge hb = { .node = node };
    struct crs_scan scan = { .r = r, .hb = &hb };
    uacpi_status status = crs_walk(&scan, node);
    if (status != UACPI_STATUS_OK) {
        acpi_log_failure(node, "_CRS", status);
    }

    uint64_t segment;
    uint64_t bbn;
    bool has_bbn;
    if (!acpi_eval_optional(node, "_SEG", &segment, NULL)
     || !acpi_eval_optional(node, "_BBN", &bbn, &has_bbn)) {
        kfree(hb.windows);
        return;
    }
    // The low bits only, ACPI 6.6, 6.5.5 and 6.5.6.
    hb.segment = (uint16_t)segment;
    hb.bus = scan.bus_found ? (uint8_t)scan.bus : (uint8_t)bbn;
    for (size_t i = 0; i < hb.window_count; i++) {
        if (hb.windows[i].base < FOUR_GIB && hb.windows[i].end > ONE_MIB) {
            hb.low_window_count++;
        }
    }

    const char *name = acpi_path(node);
    if (scan.bus_found && has_bbn && (uint8_t)bbn != hb.bus) {
        printf(
            "PCI host bridge %s: _CRS starts at bus %02x, _BBN says %02x\n", name, hb.bus,
            (uint8_t)bbn
        );
    }
    host_bridge_log(&hb, name);
    bool known = host_bridge_known(r, &hb, name);
    acpi_path_free(name);
    if (known) {
        kfree(hb.windows);
        return;
    }

    r->bridges = heap_grow(
        r->bridges, r->bridge_count, &r->bridge_capacity, sizeof(struct host_bridge), 8
    );
    r->bridges[r->bridge_count++] = hb;
}

static uacpi_iteration_decision device_found(
    void *user, uacpi_namespace_node *node, uacpi_u32 depth
) {
    (void)depth;
    struct scan *r = user;
    bool host_bridge = uacpi_device_matches_pnp_id(node, acpi_host_bridge_ids);
    uacpi_u32 sta;
    uacpi_status status = uacpi_eval_sta(node, &sta);
    if (status != UACPI_STATUS_OK) {
        acpi_log_failure(node, "_STA", status);
        // A host bridge that may be absent gives no windows, and another
        // device's memory is kept clear all the same.
        if (host_bridge) {
            return UACPI_ITERATION_DECISION_CONTINUE;
        }
        sta = ACPI_STA_RESULT_DEVICE_PRESENT;
    }
    if ((sta & ACPI_STA_RESULT_DEVICE_PRESENT) == 0) {
        return UACPI_ITERATION_DECISION_CONTINUE;
    }

    if (host_bridge) {
        host_bridge_found(r, node);
    } else {
        struct crs_scan scan = { .r = r, .hb = NULL };
        crs_walk(&scan, node);
    }
    return UACPI_ITERATION_DECISION_CONTINUE;
}

static void ranges_out(const struct range_list *l, struct pci_host_range **out, size_t *count) {
    *out = kmalloc((l->count != 0 ? l->count : 1) * sizeof(struct pci_host_range));
    for (size_t i = 0; i < l->count; i++) {
        (*out)[i] = (struct pci_host_range){ l->r[i].base, l->r[i].end };
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

    struct scan r = { .crs_failures = 0 };
    private_mcfg(&r);
    private_iommus(&r);
    claim_madt(&r);
    claim_hpets(&r);
    claim(&r, MSI_BASE, MSI_SIZE);
    uacpi_namespace_for_each_child(
        uacpi_namespace_root(), device_found, NULL, UACPI_OBJECT_DEVICE_BIT, UACPI_MAX_DEPTH_ANY,
        &r
    );
    // Devices may list them among what they use, as motherboard resources do.
    for (size_t i = 0; i < r.private.count; i++) {
        list_remove(&r.claimed, r.private.r[i].base, r.private.r[i].end);
    }
    printf(
        "PCI: %zu ranges of memory claimed, %u devices whose _CRS fails\n", r.claimed.count,
        r.crs_failures
    );

    ranges_out(&r.claimed, &host.claimed, &host.claimed_count);
    ranges_out(&r.private, &host.private, &host.private_count);
    host.bridges = kmalloc(
        (r.bridge_count != 0 ? r.bridge_count : 1) * sizeof(struct pci_host_bridge)
    );
    for (size_t i = 0; i < r.bridge_count; i++) {
        const struct host_bridge *hb = &r.bridges[i];
        host.bridges[i] = (struct pci_host_bridge){
            .node = hb->node,
            .segment = hb->segment,
            .bus = hb->bus,
            .windows = hb->windows,
            .window_count = hb->window_count
        };
    }
    host.bridge_count = r.bridge_count;
    kfree(r.bridges);
    list_free(&r.claimed);
    list_free(&r.private);
    return &host;
}
