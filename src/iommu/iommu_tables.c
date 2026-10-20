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
#include <uacpi/tables.h>
#include <memory.h>
#include <hw/acpi.h>
#include <hw/pci_cfg.h>
#include <iommu/iommu_tables.h>
#include <lib/heap.h>
#include <lib/log.h>
#include <lib/printf.h>
#include <sys/x86.h>

#define IVRS_SIGNATURE "IVRS"

#define RID_FMT "%02x:%02x.%x"
#define RID_ARGS(rid) (unsigned)((rid) >> 8), (unsigned)(((rid) >> 3) & 0x1f), (unsigned)((rid) & 7)

#define PCI_VENDOR_ID 0x00
#define PCI_HEADER_TYPE 0x0e
#define PCI_HEADER_TYPE_MASK 0x7f
#define PCI_HEADER_TYPE_BRIDGE 1
#define PCI_HEADER_TYPE_CARDBUS 2
#define PCI_SECONDARY_BUS 0x19
#define PCI_SUBORDINATE_BUS 0x1a

// A device scope's type, length, flags, reserved, enumeration ID, and start
// bus bytes, ahead of its path of device and function pairs (VT-d D51397-019
// rev. 5.20, 8.3.1).
#define SCOPE_HEADER_SIZE 6
#define SCOPE_PATH_ENTRY_SIZE 2

#define DRHD_SIZE_MASK 0xf

#define IVHD_TYPE_10H 0x10
#define IVHD_TYPE_11H 0x11
#define IVHD_TYPE_40H 0x40
#define IVMD_TYPE_ALL 0x20
#define IVMD_TYPE_SELECT 0x21
#define IVMD_TYPE_RANGE 0x22

// IVHD device entry types (AMD IOMMU 48882 rev. 3.11, Tables 105, 107, and 110).
#define IVHD_ENTRY_PAD 0x00
#define IVHD_ENTRY_ALL 0x01
#define IVHD_ENTRY_SELECT 0x02
#define IVHD_ENTRY_START 0x03
#define IVHD_ENTRY_END 0x04
#define IVHD_ENTRY_ALIAS_SELECT 0x42
#define IVHD_ENTRY_ALIAS_START 0x43
#define IVHD_ENTRY_EXT_SELECT 0x46
#define IVHD_ENTRY_EXT_START 0x47
#define IVHD_ENTRY_SPECIAL 0x48
#define IVHD_ENTRY_VARIABLE 0xf0

// A variable-length entry is this long, plus the length of its UID, which its
// byte at IVHD_ENTRY_UID_LENGTH gives.
#define IVHD_ENTRY_VARIABLE_SIZE 22
#define IVHD_ENTRY_UID_LENGTH 21

#define IVHD_SPECIAL_IOAPIC 1
#define IVHD_SPECIAL_HPET 2

#define IVMD_UNITY (1u << 0)
#define IVMD_READ (1u << 1)
#define IVMD_WRITE (1u << 2)
#define IVMD_EXCLUSION (1u << 3)

struct ivrs {
    struct acpi_sdt_hdr hdr;
    uint32_t ivinfo;
    uint64_t reserved;
} __attribute__((packed));

// The type and length that start every IVHD and IVMD.
struct ivdb {
    uint8_t type;
    uint8_t flags;
    uint16_t length;
} __attribute__((packed));

// The header of an IVHD, of which type 10h has the part ahead of efr only.
struct ivhd {
    uint8_t type;
    uint8_t flags;
    uint16_t length;
    uint16_t iommu_rid;
    uint16_t cap_offset;
    uint64_t base;
    uint16_t segment;
    uint16_t iommu_info;
    // Feature reporting (10h) or IOMMU attributes (11h, 40h).
    uint32_t features;
    uint64_t efr;
    uint64_t efr2;
} __attribute__((packed));

#define IVHD_10H_SIZE offsetof(struct ivhd, efr)

struct ivmd {
    uint8_t type;
    uint8_t flags;
    uint16_t length;
    uint16_t rid;
    // Type 22h's last requester ID.
    uint16_t aux;
    uint16_t segment;
    uint8_t reserved[6];
    uint64_t start;
    uint64_t size;
} __attribute__((packed));

_Static_assert(sizeof(struct ivrs) == 48, "IVRS layout");
_Static_assert(IVHD_10H_SIZE == 24, "IVHD layout");
_Static_assert(sizeof(struct ivhd) == 40, "IVHD layout");
_Static_assert(sizeof(struct ivmd) == 32, "IVMD layout");

struct builder {
    struct iommu_tables *t;
    uint32_t unit_capacity;
    uint32_t range_capacity;
    uint32_t special_capacity;
    uint32_t reserved_capacity;
    // Ranges of every requester ID a unit translates, which only apply to
    // what no other range lists, so they go last.
    struct iommu_device_range *fallbacks;
    uint32_t fallback_count;
    uint32_t fallback_capacity;
};

// The requester IDs a DMAR device scope resolves to.
struct scope_target {
    uint16_t rid;
    // PCI sub-hierarchies: those behind the bridge.
    uint16_t first;
    uint16_t last;
};

// An IVHD start of range entry waiting for its end of range.
struct range_start {
    bool pending;
    uint32_t offset;
    uint16_t first;
    uint8_t settings;
    bool has_alias;
    uint16_t alias;
};

static const uint8_t special_types[] = {
    [ACPI_DSS_IOAPIC] = IOMMU_SPECIAL_IOAPIC,
    [ACPI_DSS_MSI_CAPABLE_HPET] = IOMMU_SPECIAL_HPET,
    [ACPI_DSS_ACPI_NAMESPACE_DEVICE] = IOMMU_SPECIAL_ACPI,
};

static const char *const special_names[] = {
    [IOMMU_SPECIAL_IOAPIC] = "I/O APIC",
    [IOMMU_SPECIAL_HPET] = "HPET",
    [IOMMU_SPECIAL_ACPI] = "ACPI device",
};

static const char *const kind_names[] = {
    [IOMMU_KIND_NONE] = "none",
    [IOMMU_KIND_VTD] = "Intel VT-d",
    [IOMMU_KIND_AMD] = "AMD-Vi",
};

static struct iommu_tables tables;
static bool parse_done;

static uint16_t get16(const uint8_t *p) {
    uint16_t value;
    memcpy(&value, p, sizeof(value));
    return value;
}

// Returns the array, with room for one more element than count.
static void *grow(void *array, uint32_t count, uint32_t *capacity, size_t size) {
    if (count < *capacity) {
        return array;
    }
    uint32_t new_capacity = *capacity == 0 ? 8 : *capacity * 2;
    void *grown = kmalloc(new_capacity * size);
    if (count != 0) {
        memcpy(grown, array, count * size);
    }
    kfree(array);
    *capacity = new_capacity;
    return grown;
}

static bool add_unit(struct builder *b, const struct iommu_unit *unit, uint16_t *index) {
    struct iommu_tables *t = b->t;
    // Ranges and special devices name their unit in 16 bits.
    if (t->unit_count > UINT16_MAX) {
        return false;
    }
    t->units = grow(t->units, t->unit_count, &b->unit_capacity, sizeof(*t->units));
    *index = (uint16_t)t->unit_count;
    t->units[t->unit_count++] = *unit;
    return true;
}

static void add_range(struct builder *b, const struct iommu_device_range *range) {
    struct iommu_tables *t = b->t;
    t->ranges = grow(t->ranges, t->range_count, &b->range_capacity, sizeof(*t->ranges));
    t->ranges[t->range_count++] = *range;
}

static void add_fallback(struct builder *b, const struct iommu_device_range *range) {
    b->fallbacks = grow(b->fallbacks, b->fallback_count, &b->fallback_capacity,
                        sizeof(*b->fallbacks));
    b->fallbacks[b->fallback_count++] = *range;
}

static void add_special(struct builder *b, const struct iommu_special_device *special) {
    struct iommu_tables *t = b->t;
    t->specials = grow(t->specials, t->special_count, &b->special_capacity, sizeof(*t->specials));
    t->specials[t->special_count++] = *special;
}

static void add_reserved(struct builder *b, const struct iommu_reserved_region *region) {
    struct iommu_tables *t = b->t;
    t->reserved = grow(t->reserved, t->reserved_count, &b->reserved_capacity,
                       sizeof(*t->reserved));
    t->reserved[t->reserved_count++] = *region;
}

static void stop(struct builder *b, const char *what, uint32_t offset, const char *why) {
    printf("%s at offset %#x %s, parsing stops there\n", what, offset, why);
    b->t->truncated = true;
}

static void format_rids(char *buf, size_t size, uint16_t segment, uint16_t first, uint16_t last) {
    if (first == last) {
        snprintf(buf, size, "%04x:" RID_FMT, segment, RID_ARGS(first));
    } else {
        snprintf(buf, size, "%04x:" RID_FMT "-" RID_FMT, segment, RID_ARGS(first),
                 RID_ARGS(last));
    }
}

static uint16_t rid_of(struct pci_addr addr) {
    return (uint16_t)(addr.bus << 8 | addr.device << 3 | addr.function);
}

static uint32_t dmar_min_length(uint32_t type) {
    switch (type) {
        case ACPI_DMAR_ENTRY_TYPE_DRHD: {
            return sizeof(struct acpi_dmar_drhd);
        }
        case ACPI_DMAR_ENTRY_TYPE_RMRR: {
            return sizeof(struct acpi_dmar_rmrr);
        }
        case ACPI_DMAR_ENTRY_TYPE_ATSR: {
            return sizeof(struct acpi_dmar_atsr);
        }
        case ACPI_DMAR_ENTRY_TYPE_RHSA: {
            return sizeof(struct acpi_dmar_rhsa);
        }
        case ACPI_DMAR_ENTRY_TYPE_ANDD: {
            return sizeof(struct acpi_dmar_andd);
        }
        case ACPI_DMAR_ENTRY_TYPE_SATC: {
            return sizeof(struct acpi_dmar_satc);
        }
        case ACPI_DMAR_ENTRY_TYPE_SIDP: {
            return sizeof(struct acpi_dmar_sidp);
        }
        default: {
            return sizeof(struct acpi_dmar_entry_hdr);
        }
    }
}

static bool scope_type_known(uint32_t type) {
    return type >= ACPI_DSS_PCI_ENDPOINT && type <= ACPI_DSS_ACPI_NAMESPACE_DEVICE;
}

// The length of the device scope at offset, which must end by end, or 0,
// having stopped the parsing, if it does not.
static uint32_t scope_length(struct builder *b, const uint8_t *dmar, uint32_t offset,
                             uint32_t end) {
    if (end - offset < 2) {
        stop(b, "DMAR device scope", offset, "is cut short by the end of its structure");
        return 0;
    }
    uint32_t type = dmar[offset];
    uint32_t length = dmar[offset + 1];
    // Every known type needs a path of at least one entry.
    uint32_t min = SCOPE_HEADER_SIZE + (scope_type_known(type) ? SCOPE_PATH_ENTRY_SIZE : 0);
    if (length < min) {
        stop(b, "DMAR device scope", offset, "is too short for its type");
        return 0;
    }
    if (length > end - offset) {
        stop(b, "DMAR device scope", offset, "overruns its structure");
        return 0;
    }
    return length;
}

static void scope_skip(const char *owner, uint32_t offset, const uint8_t *scope,
                       const char *why) {
    char path[128] = "none";
    int len = 0;
    uint32_t entries = ((uint32_t)scope[1] - SCOPE_HEADER_SIZE) / SCOPE_PATH_ENTRY_SIZE;
    for (uint32_t i = 0; i < entries && len >= 0 && (size_t)len < sizeof(path); i++) {
        const uint8_t *entry = scope + SCOPE_HEADER_SIZE + i * SCOPE_PATH_ENTRY_SIZE;
        len += snprintf(path + len, sizeof(path) - (size_t)len, "%s%02x.%x", i == 0 ? "" : "/",
                        entry[0], entry[1]);
    }
    printf("DMAR %s device scope at offset %#x, type %u, start bus %02x, path %s: %s, skipped\n",
           owner, offset, scope[0], scope[5], path, why);
}

// Why the function at addr cannot be a bridge on a scope's path, or NULL with
// the buses behind it.
static const char *bridge_buses(struct pci_addr addr, uint8_t *secondary, uint8_t *subordinate) {
    if (pci_cfg_read(addr, PCI_VENDOR_ID, 2) == 0xffff) {
        return "reads all ones";
    }
    uint32_t header_type = pci_cfg_read(addr, PCI_HEADER_TYPE, 1) & PCI_HEADER_TYPE_MASK;
    if (header_type != PCI_HEADER_TYPE_BRIDGE && header_type != PCI_HEADER_TYPE_CARDBUS) {
        return "is not a bridge";
    }
    *secondary = (uint8_t)pci_cfg_read(addr, PCI_SECONDARY_BUS, 1);
    *subordinate = (uint8_t)pci_cfg_read(addr, PCI_SUBORDINATE_BUS, 1);
    if (*secondary <= addr.bus) {
        return "has a secondary bus not above its own";
    }
    if (*subordinate < *secondary) {
        return "has a subordinate bus below its secondary bus";
    }
    return NULL;
}

// Walks the path of a device scope of a known type, whose length scope_length()
// checked, through the bridges on it (VT-d D51397-019 rev. 5.20, 8.3.1).
static bool scope_resolve(const char *owner, uint32_t offset, const uint8_t *scope,
                          uint16_t segment, struct scope_target *target) {
    uint32_t type = scope[0];
    uint32_t path_size = (uint32_t)scope[1] - SCOPE_HEADER_SIZE;
    if (path_size % SCOPE_PATH_ENTRY_SIZE != 0) {
        scope_skip(owner, offset, scope, "path of an odd length");
        return false;
    }
    uint32_t entries = path_size / SCOPE_PATH_ENTRY_SIZE;
    const uint8_t *path = scope + SCOPE_HEADER_SIZE;

    char why[96];
    struct pci_addr addr = {.segment = segment, .bus = scope[5]};
    uint8_t secondary = 0;
    uint8_t subordinate = 0;
    for (uint32_t i = 0; i < entries; i++) {
        uint8_t device = path[i * SCOPE_PATH_ENTRY_SIZE];
        uint8_t function = path[i * SCOPE_PATH_ENTRY_SIZE + 1];
        if (device >= 32 || function >= 8) {
            snprintf(why, sizeof(why), "no device %#x function %#x", device, function);
            scope_skip(owner, offset, scope, why);
            return false;
        }
        addr.device = device;
        addr.function = function;

        bool last = i + 1 == entries;
        if (last && type != ACPI_DSS_PCI_BRIDGE) {
            break;
        }
        const char *problem = bridge_buses(addr, &secondary, &subordinate);
        if (problem != NULL) {
            snprintf(why, sizeof(why), "bridge %04x:" RID_FMT " %s", segment,
                     RID_ARGS(rid_of(addr)), problem);
            scope_skip(owner, offset, scope, why);
            return false;
        }
        if (!last) {
            addr.bus = secondary;
        }
    }

    target->rid = rid_of(addr);
    target->first = 0;
    target->last = 0;
    if (type == ACPI_DSS_PCI_BRIDGE) {
        target->first = (uint16_t)(secondary << 8);
        target->last = (uint16_t)(subordinate << 8 | 0xff);
    }
    return true;
}

static bool dmar_drhd(struct builder *b, const uint8_t *dmar, uint32_t offset, uint32_t length) {
    struct acpi_dmar_drhd drhd;
    memcpy(&drhd, dmar + offset, sizeof(drhd));
    struct iommu_unit unit = {
        .base = drhd.address,
        .vtd_size = 4096ull << (drhd.size & DRHD_SIZE_MASK),
        .segment = drhd.segment,
        .include_all = (drhd.flags & ACPI_INCLUDE_PCI_ALL) != 0,
    };
    uint16_t index;
    if (!add_unit(b, &unit, &index)) {
        stop(b, "DMAR DRHD", offset, "makes too many units");
        return false;
    }
    if (unit.include_all) {
        struct iommu_device_range all = {
            .segment = unit.segment,
            .first = 0,
            .last = 0xffff,
            .unit = index,
        };
        add_fallback(b, &all);
    }

    uint32_t end = offset + length;
    for (uint32_t at = offset + sizeof(drhd); at < end;) {
        uint32_t scope_len = scope_length(b, dmar, at, end);
        if (scope_len == 0) {
            return false;
        }
        const uint8_t *scope = dmar + at;
        struct scope_target target;
        if (!scope_type_known(scope[0])) {
            scope_skip("DRHD", at, scope, "unknown type");
        } else if (scope_resolve("DRHD", at, scope, unit.segment, &target)) {
            switch (scope[0]) {
                case ACPI_DSS_PCI_ENDPOINT:
                case ACPI_DSS_PCI_BRIDGE: {
                    struct iommu_device_range range = {
                        .segment = unit.segment,
                        .first = target.rid,
                        .last = target.rid,
                        .unit = index,
                    };
                    add_range(b, &range);
                    if (scope[0] == ACPI_DSS_PCI_BRIDGE) {
                        range.first = target.first;
                        range.last = target.last;
                        add_range(b, &range);
                    }
                    break;
                }
                default: {
                    struct iommu_special_device special = {
                        .type = special_types[scope[0]],
                        .id = scope[4],
                        .segment = unit.segment,
                        .rid = target.rid,
                        .unit = index,
                    };
                    add_special(b, &special);
                    break;
                }
            }
        }
        at += scope_len;
    }
    return true;
}

static bool dmar_rmrr(struct builder *b, const uint8_t *dmar, uint32_t offset, uint32_t length) {
    struct acpi_dmar_rmrr rmrr;
    memcpy(&rmrr, dmar + offset, sizeof(rmrr));
    if (length == sizeof(rmrr)) {
        printf("DMAR RMRR at offset %#x for %#llx-%#llx has no device scope, skipped\n", offset,
               (unsigned long long)rmrr.base, (unsigned long long)rmrr.limit);
        return true;
    }
    if (rmrr.limit < rmrr.base) {
        printf("DMAR RMRR at offset %#x has its limit %#llx below its base %#llx, skipped\n",
               offset, (unsigned long long)rmrr.limit, (unsigned long long)rmrr.base);
        return true;
    }

    uint32_t end = offset + length;
    for (uint32_t at = offset + sizeof(rmrr); at < end;) {
        uint32_t scope_len = scope_length(b, dmar, at, end);
        if (scope_len == 0) {
            return false;
        }
        const uint8_t *scope = dmar + at;
        struct scope_target target;
        if (!scope_type_known(scope[0])) {
            scope_skip("RMRR", at, scope, "unknown type");
        } else if (scope_resolve("RMRR", at, scope, rmrr.segment, &target)) {
            struct iommu_reserved_region region = {
                .base = rmrr.base,
                .last = rmrr.limit,
                .readable = true,
                .writable = true,
                .segment = rmrr.segment,
                .first = target.rid,
                .last_rid = target.rid,
            };
            add_reserved(b, &region);
            if (scope[0] == ACPI_DSS_PCI_BRIDGE) {
                region.first = target.first;
                region.last_rid = target.last;
                add_reserved(b, &region);
            }
        }
        at += scope_len;
    }
    return true;
}

static void dmar_andd(const uint8_t *dmar, uint32_t offset, uint32_t length) {
    struct acpi_dmar_andd andd;
    memcpy(&andd, dmar + offset, sizeof(andd));
    const uint8_t *name = dmar + offset + sizeof(andd);
    uint32_t max = length - (uint32_t)sizeof(andd);
    uint32_t name_length = 0;
    while (name_length < max && name[name_length] >= 0x20 && name[name_length] < 0x7f) {
        name_length++;
    }
    printf("DMAR ANDD: ACPI device %u is %.*s\n", andd.device_id, (int)name_length,
           (const char *)name);
}

static void dmar_parse(struct builder *b, const uint8_t *dmar, uint32_t length) {
    struct acpi_dmar header;
    memcpy(&header, dmar, sizeof(header));
    b->t->vtd_haw = header.haw + 1u;
    b->t->vtd_flags = header.flags;
    printf("DMAR revision %u, host address width %u, flags %#x\n", header.hdr.revision,
           b->t->vtd_haw, b->t->vtd_flags);

    uint32_t offset = sizeof(header);
    while (offset < length) {
        struct acpi_dmar_entry_hdr entry;
        if (length - offset < sizeof(entry)) {
            stop(b, "DMAR structure", offset, "is cut short by the end of the table");
            return;
        }
        memcpy(&entry, dmar + offset, sizeof(entry));
        uint32_t entry_length = entry.length;
        if (entry_length < dmar_min_length(entry.type)) {
            stop(b, "DMAR structure", offset, "is too short for its type");
            return;
        }
        if (entry_length > length - offset) {
            stop(b, "DMAR structure", offset, "overruns the table");
            return;
        }

        bool ok = true;
        switch (entry.type) {
            case ACPI_DMAR_ENTRY_TYPE_DRHD: {
                ok = dmar_drhd(b, dmar, offset, entry_length);
                break;
            }
            case ACPI_DMAR_ENTRY_TYPE_RMRR: {
                ok = dmar_rmrr(b, dmar, offset, entry_length);
                break;
            }
            case ACPI_DMAR_ENTRY_TYPE_ANDD: {
                dmar_andd(dmar, offset, entry_length);
                break;
            }
            case ACPI_DMAR_ENTRY_TYPE_ATSR:
            case ACPI_DMAR_ENTRY_TYPE_RHSA:
            case ACPI_DMAR_ENTRY_TYPE_SATC:
            case ACPI_DMAR_ENTRY_TYPE_SIDP: {
                break;
            }
            default: {
                printf("DMAR structure at offset %#x has unknown type %u, skipped\n", offset,
                       entry.type);
                break;
            }
        }
        if (!ok) {
            return;
        }
        offset += entry_length;
    }
}

static uint32_t ivrs_min_length(uint32_t type) {
    switch (type) {
        case IVHD_TYPE_10H: {
            return IVHD_10H_SIZE;
        }
        case IVHD_TYPE_11H:
        case IVHD_TYPE_40H: {
            return sizeof(struct ivhd);
        }
        case IVMD_TYPE_ALL:
        case IVMD_TYPE_SELECT:
        case IVMD_TYPE_RANGE: {
            return sizeof(struct ivmd);
        }
        default: {
            return sizeof(struct ivdb);
        }
    }
}

static bool ivhd_type(uint32_t type) {
    return type == IVHD_TYPE_10H || type == IVHD_TYPE_11H || type == IVHD_TYPE_40H;
}

// The offset up to which the blocks fit the table and their types, and why the
// parsing stops there, or NULL at the end of the table.
static uint32_t ivrs_end(const uint8_t *ivrs, uint32_t length, const char **why) {
    uint32_t offset = sizeof(struct ivrs);
    *why = NULL;
    while (offset < length) {
        struct ivdb block;
        if (length - offset < sizeof(block)) {
            *why = "is cut short by the end of the table";
            break;
        }
        memcpy(&block, ivrs + offset, sizeof(block));
        if (block.length < ivrs_min_length(block.type)) {
            *why = "is too short for its type";
            break;
        }
        if (block.length > length - offset) {
            *why = "overruns the table";
            break;
        }
        offset += block.length;
    }
    return offset;
}

// The offset of the IVHD to use for the IOMMU that the one at offset
// describes: the first of those of the highest type.
static uint32_t ivhd_chosen(const uint8_t *ivrs, uint32_t end, uint32_t offset) {
    struct ivhd mine;
    memcpy(&mine, ivrs + offset, IVHD_10H_SIZE);
    uint32_t chosen = offset;
    uint32_t chosen_type = mine.type;
    for (uint32_t at = sizeof(struct ivrs); at < end;) {
        struct ivdb block;
        memcpy(&block, ivrs + at, sizeof(block));
        if (ivhd_type(block.type)) {
            struct ivhd other;
            memcpy(&other, ivrs + at, IVHD_10H_SIZE);
            bool same = other.iommu_rid == mine.iommu_rid && other.base == mine.base;
            if (same && (other.type > chosen_type || (other.type == chosen_type && at < chosen))) {
                chosen = at;
                chosen_type = other.type;
            }
        }
        at += block.length;
    }
    return chosen;
}

// The size of the IVHD device entry at offset, or 0 if it does not end by end.
static uint32_t ivhd_entry_size(const uint8_t *ivrs, uint32_t offset, uint32_t end) {
    uint32_t type = ivrs[offset];
    uint32_t size;
    if (type >= IVHD_ENTRY_VARIABLE) {
        if (end - offset < IVHD_ENTRY_VARIABLE_SIZE) {
            return 0;
        }
        size = IVHD_ENTRY_VARIABLE_SIZE + ivrs[offset + IVHD_ENTRY_UID_LENGTH];
    } else {
        // 4, 8, 16, or 32 bytes by the top two bits of the type.
        size = 4u << (type >> 6);
    }
    return size <= end - offset ? size : 0;
}

static void ivhd_entry(struct builder *b, const struct iommu_unit *unit, uint16_t index,
                       const uint8_t *entry, uint32_t offset, struct range_start *start) {
    struct iommu_device_range range = {
        .segment = unit->segment,
        .first = get16(entry + 1),
        .last = get16(entry + 1),
        .unit = index,
        .amd_settings = entry[3],
    };
    switch (entry[0]) {
        case IVHD_ENTRY_PAD: {
            break;
        }
        case IVHD_ENTRY_ALL: {
            range.first = 0;
            range.last = 0xffff;
            add_fallback(b, &range);
            break;
        }
        case IVHD_ENTRY_SELECT:
        case IVHD_ENTRY_EXT_SELECT: {
            add_range(b, &range);
            break;
        }
        case IVHD_ENTRY_ALIAS_SELECT: {
            range.has_alias = true;
            range.alias = get16(entry + 5);
            add_range(b, &range);
            break;
        }
        case IVHD_ENTRY_START:
        case IVHD_ENTRY_EXT_START:
        case IVHD_ENTRY_ALIAS_START: {
            start->pending = true;
            start->offset = offset;
            start->first = range.first;
            start->settings = entry[3];
            start->has_alias = entry[0] == IVHD_ENTRY_ALIAS_START;
            start->alias = start->has_alias ? get16(entry + 5) : 0;
            break;
        }
        case IVHD_ENTRY_END: {
            if (!start->pending) {
                printf("IVHD end of range at offset %#x has no start of range, skipped\n",
                       offset);
                break;
            }
            start->pending = false;
            if (range.last < start->first) {
                printf("IVHD range at offset %#x ends at " RID_FMT " before its start " RID_FMT
                       ", skipped\n", start->offset, RID_ARGS(range.last),
                       RID_ARGS(start->first));
                break;
            }
            range.first = start->first;
            range.has_alias = start->has_alias;
            range.alias = start->alias;
            range.amd_settings = start->settings;
            add_range(b, &range);
            break;
        }
        case IVHD_ENTRY_SPECIAL: {
            struct iommu_special_device special = {
                .id = entry[4],
                .segment = unit->segment,
                .rid = get16(entry + 5),
                .unit = index,
                .amd_settings = entry[3],
            };
            if (entry[7] == IVHD_SPECIAL_IOAPIC) {
                special.type = IOMMU_SPECIAL_IOAPIC;
            } else if (entry[7] == IVHD_SPECIAL_HPET) {
                special.type = IOMMU_SPECIAL_HPET;
            } else {
                printf("IVHD special device entry at offset %#x has unknown variety %u, "
                       "skipped\n", offset, entry[7]);
                break;
            }
            add_special(b, &special);
            break;
        }
        default: {
            if (entry[0] >= IVHD_ENTRY_VARIABLE) {
                printf("IVHD ACPI device entry at offset %#x for %04x:" RID_FMT " skipped\n",
                       offset, unit->segment, RID_ARGS(range.first));
            } else {
                printf("IVHD device entry at offset %#x has unknown type %#x, skipped\n",
                       offset, entry[0]);
            }
            break;
        }
    }
}

static void range_start_drop(struct range_start *start) {
    printf("IVHD start of range at offset %#x has no end of range, dropped\n", start->offset);
    start->pending = false;
}

static bool ivhd_parse(struct builder *b, const uint8_t *ivrs, uint32_t offset, uint32_t length) {
    uint32_t type = ivrs[offset];
    uint32_t header_size = type == IVHD_TYPE_10H ? IVHD_10H_SIZE : sizeof(struct ivhd);
    struct ivhd ivhd = {0};
    memcpy(&ivhd, ivrs + offset, header_size);
    struct iommu_unit unit = {
        .base = ivhd.base,
        .segment = ivhd.segment,
        .amd_ivhd_type = ivhd.type,
        .amd_rid = ivhd.iommu_rid,
        .amd_cap_offset = ivhd.cap_offset,
        .amd_ivhd_flags = ivhd.flags,
        .amd_iommu_info = ivhd.iommu_info,
        .amd_features = ivhd.features,
        .amd_efr = ivhd.efr,
        .amd_efr2 = ivhd.efr2,
    };
    uint16_t index;
    if (!add_unit(b, &unit, &index)) {
        stop(b, "IVRS IVHD", offset, "makes too many units");
        return false;
    }

    struct range_start start = {0};
    uint32_t end = offset + length;
    for (uint32_t at = offset + header_size; at < end;) {
        uint32_t size = ivhd_entry_size(ivrs, at, end);
        if (size == 0) {
            stop(b, "IVHD device entry", at, "overruns its block");
            return false;
        }
        if (start.pending && ivrs[at] != IVHD_ENTRY_END) {
            range_start_drop(&start);
        }
        ivhd_entry(b, &unit, index, ivrs + at, at, &start);
        at += size;
    }
    if (start.pending) {
        range_start_drop(&start);
    }
    return true;
}

static void ivmd_parse(struct builder *b, const uint8_t *ivrs, uint32_t offset) {
    struct ivmd ivmd;
    memcpy(&ivmd, ivrs + offset, sizeof(ivmd));

    struct iommu_reserved_region region = {0};
    switch (ivmd.type) {
        case IVMD_TYPE_ALL: {
            // Every device in the system: its DeviceID and segment fields are
            // reserved.
            region.all_devices = true;
            region.first = 0;
            region.last_rid = 0xffff;
            break;
        }
        case IVMD_TYPE_SELECT: {
            region.segment = ivmd.segment;
            region.first = ivmd.rid;
            region.last_rid = ivmd.rid;
            break;
        }
        default: {
            region.segment = ivmd.segment;
            region.first = ivmd.rid;
            region.last_rid = ivmd.aux;
            break;
        }
    }
    if (region.last_rid < region.first) {
        printf("IVMD at offset %#x ends at " RID_FMT " before its start " RID_FMT ", skipped\n",
               offset, RID_ARGS(region.last_rid), RID_ARGS(region.first));
        return;
    }
    if (ivmd.size == 0 || ivmd.size - 1 > UINT64_MAX - ivmd.start) {
        printf("IVMD at offset %#x has a memory block of %#llx bytes at %#llx, skipped\n",
               offset, (unsigned long long)ivmd.size, (unsigned long long)ivmd.start);
        return;
    }
    if (ivmd.flags & IVMD_EXCLUSION) {
        region.exclusion = true;
        region.readable = true;
        region.writable = true;
    } else {
        // Without Unity, any translation will do, one to one included.
        region.readable = (ivmd.flags & IVMD_READ) != 0;
        region.writable = (ivmd.flags & IVMD_WRITE) != 0;
        if (!region.readable && !region.writable && (ivmd.flags & IVMD_UNITY) != 0) {
            printf("IVMD at offset %#x, flags %#x, sets Unity without IR or IW, taken as memory "
                   "never to map\n", offset, ivmd.flags);
        }
    }
    region.base = ivmd.start;
    region.last = ivmd.start + (ivmd.size - 1);
    add_reserved(b, &region);
}

static void ivrs_parse(struct builder *b, const uint8_t *ivrs, uint32_t length) {
    struct ivrs header;
    memcpy(&header, ivrs, sizeof(header));
    b->t->amd_ivinfo = header.ivinfo;
    printf("IVRS revision %u, IVinfo %#x\n", header.hdr.revision, header.ivinfo);

    const char *why;
    uint32_t end = ivrs_end(ivrs, length, &why);
    for (uint32_t offset = sizeof(header); offset < end;) {
        struct ivdb block;
        memcpy(&block, ivrs + offset, sizeof(block));
        switch (block.type) {
            case IVHD_TYPE_10H:
            case IVHD_TYPE_11H:
            case IVHD_TYPE_40H: {
                uint32_t chosen = ivhd_chosen(ivrs, end, offset);
                if (chosen != offset) {
                    printf("IVRS IVHD at offset %#x, type %#x, superseded by the one at offset "
                           "%#x, type %#x\n", offset, block.type, chosen, ivrs[chosen]);
                } else if (!ivhd_parse(b, ivrs, offset, block.length)) {
                    return;
                }
                break;
            }
            case IVMD_TYPE_ALL:
            case IVMD_TYPE_SELECT:
            case IVMD_TYPE_RANGE: {
                ivmd_parse(b, ivrs, offset);
                break;
            }
            default: {
                printf("IVRS block at offset %#x has unknown type %#x, skipped\n", offset,
                       block.type);
                break;
            }
        }
        offset += block.length;
    }
    if (why != NULL) {
        stop(b, "IVRS block", end, why);
    }
}

// Makes the fallback ranges the last ones, and drops tables without units.
static void finish(struct builder *b) {
    struct iommu_tables *t = b->t;
    for (uint32_t i = 0; i < b->fallback_count; i++) {
        add_range(b, &b->fallbacks[i]);
    }
    kfree(b->fallbacks);
    b->fallbacks = NULL;
    b->fallback_count = 0;

    if (t->kind != IOMMU_KIND_NONE && t->unit_count == 0) {
        printf("No remapping hardware unit in the table\n");
        kfree(t->units);
        kfree(t->ranges);
        kfree(t->specials);
        kfree(t->reserved);
        *t = (struct iommu_tables){0};
    }
}

static void log_unit(const struct iommu_tables *t, uint32_t index) {
    const struct iommu_unit *u = &t->units[index];
    if (t->kind == IOMMU_KIND_VTD) {
        printf("IOMMU unit %u: VT-d registers at %#llx, %llu KiB, segment %u%s\n", index,
               (unsigned long long)u->base, (unsigned long long)(u->vtd_size / 1024), u->segment,
               u->include_all ? ", all other devices" : "");
        return;
    }
    printf("IOMMU unit %u: AMD IVHD type %#x, registers at %#llx, segment %u, requester ID "
           RID_FMT ", capability at %#x, flags %#x, info %#x, %s %#x, EFR %#llx, EFR2 %#llx\n",
           index, u->amd_ivhd_type, (unsigned long long)u->base, u->segment,
           RID_ARGS(u->amd_rid), u->amd_cap_offset, u->amd_ivhd_flags, u->amd_iommu_info,
           u->amd_ivhd_type == IVHD_TYPE_10H ? "features" : "attributes", u->amd_features,
           (unsigned long long)u->amd_efr, (unsigned long long)u->amd_efr2);
}

static void log_range(const struct iommu_tables *t, uint32_t index) {
    const struct iommu_device_range *r = &t->ranges[index];
    char rids[32];
    format_rids(rids, sizeof(rids), r->segment, r->first, r->last);
    char extra[48] = "";
    if (t->kind == IOMMU_KIND_AMD) {
        int len = snprintf(extra, sizeof(extra), ", settings %#x", r->amd_settings);
        if (r->has_alias && len >= 0 && (size_t)len < sizeof(extra)) {
            snprintf(extra + len, sizeof(extra) - (size_t)len, ", as " RID_FMT,
                     RID_ARGS(r->alias));
        }
    }
    printf("IOMMU devices %s: unit %u%s\n", rids, r->unit, extra);
}

static void log_special(const struct iommu_tables *t, uint32_t index) {
    const struct iommu_special_device *s = &t->specials[index];
    char extra[24] = "";
    if (t->kind == IOMMU_KIND_AMD) {
        snprintf(extra, sizeof(extra), ", settings %#x", s->amd_settings);
    }
    printf("IOMMU %s %u is %04x:" RID_FMT ": unit %u%s\n", special_names[s->type], s->id,
           s->segment, RID_ARGS(s->rid), s->unit, extra);
}

static void log_reserved(const struct iommu_tables *t, uint32_t index) {
    const struct iommu_reserved_region *r = &t->reserved[index];
    char devices[40] = "all devices";
    if (!r->all_devices) {
        format_rids(devices, sizeof(devices), r->segment, r->first, r->last_rid);
    }
    const char *kind = "identity mapping";
    if (r->exclusion) {
        kind = "exclusion range";
    } else if (!r->readable && !r->writable) {
        kind = "never-mapped range";
    }
    const char *access = "no access";
    if (r->readable && r->writable) {
        access = "read-write";
    } else if (r->readable) {
        access = "read-only";
    } else if (r->writable) {
        access = "write-only";
    }
    printf("IOMMU %s %#llx-%#llx, %s, for %s\n", kind, (unsigned long long)r->base,
           (unsigned long long)r->last, access, devices);
}

static void log_tables(const struct iommu_tables *t) {
    printf("IOMMU tables: %s, %u units, %u device ranges, %u special devices, %u reserved "
           "regions%s\n", kind_names[t->kind], t->unit_count, t->range_count, t->special_count,
           t->reserved_count, t->truncated ? ", table cut short" : "");
    for (uint32_t i = 0; i < t->unit_count; i++) {
        log_unit(t, i);
    }
    for (uint32_t i = 0; i < t->range_count; i++) {
        log_range(t, i);
    }
    for (uint32_t i = 0; i < t->special_count; i++) {
        log_special(t, i);
    }
    for (uint32_t i = 0; i < t->reserved_count; i++) {
        log_reserved(t, i);
    }
}

// Fills vendor with the processor's vendor string, and returns whether that
// vendor's platforms have AMD's IOMMU.
static bool vendor_is_amd(char vendor[13]) {
    struct cpuid_regs regs = cpuid(0, 0);
    memcpy(vendor, &regs.ebx, 4);
    memcpy(vendor + 4, &regs.edx, 4);
    memcpy(vendor + 8, &regs.ecx, 4);
    vendor[12] = '\0';
    return memcmp(vendor, "AuthenticAMD", 12) == 0 || memcmp(vendor, "HygonGenuine", 12) == 0;
}

static bool table_find(const char *signature, struct uacpi_table *table) {
    uacpi_status status = uacpi_table_find_by_signature(signature, table);
    if (status == UACPI_STATUS_OK) {
        return true;
    }
    if (status != UACPI_STATUS_NOT_FOUND) {
        printf("Cannot get the %s table: %s\n", signature, uacpi_status_to_string(status));
    }
    return false;
}

const struct iommu_tables *iommu_tables_parse(void) {
    if (parse_done) {
        return &tables;
    }
    if (!acpi_tables_ready()) {
        printf("No IOMMU tables without ACPI tables\n");
        return &tables;
    }
    parse_done = true;

    struct uacpi_table dmar;
    struct uacpi_table ivrs;
    bool have_dmar = table_find(ACPI_DMAR_SIGNATURE, &dmar);
    bool have_ivrs = table_find(IVRS_SIGNATURE, &ivrs);
    if (have_dmar && have_ivrs) {
        char vendor[13];
        bool amd = vendor_is_amd(vendor);
        printf("Both DMAR and IVRS tables, using the %s for the %s processor\n",
               amd ? "IVRS" : "DMAR", vendor);
        if (amd) {
            uacpi_table_unref(&dmar);
            have_dmar = false;
        } else {
            uacpi_table_unref(&ivrs);
            have_ivrs = false;
        }
    }

    struct builder b = {.t = &tables};
    if (have_dmar) {
        uint32_t length = dmar.hdr->length;
        if (length < sizeof(struct acpi_dmar)) {
            printf("DMAR table of %u bytes is too short\n", length);
        } else {
            tables.kind = IOMMU_KIND_VTD;
            dmar_parse(&b, dmar.ptr, length);
        }
        uacpi_table_unref(&dmar);
    } else if (have_ivrs) {
        uint32_t length = ivrs.hdr->length;
        if (length < sizeof(struct ivrs)) {
            printf("IVRS table of %u bytes is too short\n", length);
        } else {
            tables.kind = IOMMU_KIND_AMD;
            ivrs_parse(&b, ivrs.ptr, length);
        }
        uacpi_table_unref(&ivrs);
    } else {
        printf("No DMAR or IVRS table\n");
    }

    finish(&b);
    log_tables(&tables);
    return &tables;
}

const struct iommu_device_range *iommu_range_for(uint16_t segment, uint16_t rid) {
    for (uint32_t i = 0; i < tables.range_count; i++) {
        const struct iommu_device_range *r = &tables.ranges[i];
        if (r->segment == segment && rid >= r->first && rid <= r->last) {
            return r;
        }
    }
    return NULL;
}

const struct iommu_special_device *iommu_special_for(uint8_t type, uint8_t id) {
    for (uint32_t i = 0; i < tables.special_count; i++) {
        const struct iommu_special_device *d = &tables.specials[i];
        if (d->type == type && d->id == id) {
            return d;
        }
    }
    return NULL;
}
