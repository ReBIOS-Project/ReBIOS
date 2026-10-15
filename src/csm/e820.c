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
#include <memory.h>
#include <csm/e820.h>
#include <lib/log.h>
#include <lib/printf.h>

#define MIB (1ull << 20)
#define GIB (1ull << 30)

// A range by its last byte, which, unlike its end, always fits in 64 bits.
struct span {
    uint64_t base;
    uint64_t last;
    uint32_t type;
};

static uint64_t entry_last(const struct e820_entry *e) {
    return e->base + (e->size - 1);
}

void e820_init(struct e820_map *map) {
    map->count = 0;
}

bool e820_set(struct e820_map *map, uint64_t base, uint64_t size, uint32_t type) {
    if (size == 0) {
        return true;
    }
    uint64_t last = size - 1 > UINT64_MAX - base ? UINT64_MAX : base + (size - 1);

    // The entries that overlap or touch the range are rewritten, and so is one
    // entry either side of them: an address space all of one type stays two
    // entries, which must merge again once the range trims one of them.
    uint32_t first = 0;
    while (first < map->count && base != 0 && entry_last(&map->entries[first]) < base - 1) {
        first++;
    }
    uint32_t end = first;
    while (end < map->count && (last == UINT64_MAX || map->entries[end].base <= last + 1)) {
        end++;
    }
    if (first > 0) {
        first--;
    }
    if (end < map->count) {
        end++;
    }

    // What these keep below the range, the range, and what they keep above it,
    // at most two spans either side.
    struct span spans[5];
    uint32_t span_count = 0;
    for (uint32_t i = first; i < end; i++) {
        const struct e820_entry *e = &map->entries[i];
        if (e->base < base) {
            uint64_t kept_last = entry_last(e) < base ? entry_last(e) : base - 1;
            spans[span_count++] = (struct span){e->base, kept_last, e->type};
        }
    }
    if (type != 0) {
        spans[span_count++] = (struct span){base, last, type};
    }
    for (uint32_t i = first; i < end; i++) {
        const struct e820_entry *e = &map->entries[i];
        if (entry_last(e) > last) {
            uint64_t kept_base = e->base > last ? e->base : last + 1;
            spans[span_count++] = (struct span){kept_base, entry_last(e), e->type};
        }
    }

    // Spans that touch and share a type merge, except into the whole address
    // space, which has no 64-bit size.
    uint32_t kept = 0;
    for (uint32_t i = 0; i < span_count; i++) {
        if (kept != 0 && spans[kept - 1].type == spans[i].type
            && spans[kept - 1].last + 1 == spans[i].base
            && !(spans[kept - 1].base == 0 && spans[i].last == UINT64_MAX)) {
            spans[kept - 1].last = spans[i].last;
        } else {
            spans[kept++] = spans[i];
        }
    }

    uint32_t count = map->count - (end - first) + kept;
    if (count > E820_MAX_ENTRIES) {
        return false;
    }
    memmove(&map->entries[first + kept], &map->entries[end],
            (map->count - end) * sizeof(struct e820_entry));
    for (uint32_t i = 0; i < kept; i++) {
        struct e820_entry *e = &map->entries[first + i];
        e->base = spans[i].base;
        e->size = spans[i].last - spans[i].base + 1;
        e->type = spans[i].type;
    }
    map->count = count;
    return true;
}

static uint32_t efi_type_to_e820(uint32_t type) {
    switch (type) {
        case EfiLoaderCode:
        case EfiLoaderData:
        case EfiBootServicesCode:
        case EfiBootServicesData:
        case EfiConventionalMemory: {
            return E820_RAM;
        }
        case EfiACPIReclaimMemory: {
            return E820_ACPI;
        }
        case EfiACPIMemoryNVS: {
            return E820_NVS;
        }
        case EfiUnusableMemory: {
            // Memory with errors, which ACPI's table of UEFI types calls
            // reserved, though E820 has a type for it.
            return E820_UNUSABLE;
        }
        case EfiPersistentMemory: {
            return E820_PMEM;
        }
        default: {
            return E820_RESERVED;
        }
    }
}

bool e820_set_efi_map(struct e820_map *map, const void *efi_mmap, uint64_t mmap_size,
                      uint64_t desc_size) {
    if (desc_size < sizeof(EFI_MEMORY_DESCRIPTOR)) {
        return false;
    }

    bool ok = true;
    for (uint64_t off = 0; mmap_size - off >= desc_size; off += desc_size) {
        // The stride need not keep the descriptors aligned.
        EFI_MEMORY_DESCRIPTOR desc;
        memcpy(&desc, (const uint8_t *)efi_mmap + off, sizeof(desc));
        if (desc.NumberOfPages == 0) {
            continue;
        }
        uint64_t size = UINT64_MAX;
        if (desc.NumberOfPages <= UINT64_MAX >> EFI_PAGE_SHIFT) {
            size = desc.NumberOfPages << EFI_PAGE_SHIFT;
        }
        if (!e820_set(map, desc.PhysicalStart, size, efi_type_to_e820(desc.Type))) {
            ok = false;
        }
    }
    return ok;
}

bool e820_set_low_memory(struct e820_map *map) {
    // ACPI's E820 does not describe video memory or option ROMs, and what else
    // lies below the BIOS is for the operating system to find.
    static const struct e820_entry layout[] = {
        {0, 0x9fc00, E820_RAM},
        {0x9fc00, 0x400, E820_RESERVED},
        {0xf0000, 0x10000, E820_RESERVED},
    };

    // Cleared first, so that no step needs more entries than the result.
    bool ok = e820_set(map, 0, 1 * MIB, 0);
    for (size_t i = 0; i < sizeof(layout) / sizeof(layout[0]); i++) {
        if (!e820_set(map, layout[i].base, layout[i].size, layout[i].type)) {
            ok = false;
        }
    }
    return ok;
}

// The last byte of the run of RAM entries from entry i, each starting where
// the previous one ends.
static uint64_t ram_run_last(const struct e820_map *map, uint32_t i) {
    uint64_t last = entry_last(&map->entries[i]);
    while (i + 1 < map->count && last != UINT64_MAX && map->entries[i + 1].type == E820_RAM
           && map->entries[i + 1].base == last + 1) {
        i++;
        last = entry_last(&map->entries[i]);
    }
    return last;
}

static uint64_t saturate(uint64_t value, uint64_t max) {
    return value < max ? value : max;
}

void e820_cmos(const struct e820_map *map, struct e820_cmos *cmos) {
    cmos->base_kib = 640;
    cmos->extended_kib = 0;
    cmos->above_16m = 0;
    cmos->above_4g = 0;

    for (uint32_t i = 0; i < map->count; i++) {
        const struct e820_entry *e = &map->entries[i];
        if (e->type != E820_RAM) {
            continue;
        }
        if (e->base <= 1 * MIB && entry_last(e) >= 1 * MIB) {
            uint64_t end = saturate(ram_run_last(map, i), 4 * GIB - 1) + 1;
            cmos->extended_kib = saturate((end - 1 * MIB) / 1024, 65535);
            if (end > 16 * MIB) {
                cmos->above_16m = saturate((end - 16 * MIB) / (64 * 1024), 65535);
            }
        }
        if (e->base == 4 * GIB) {
            uint64_t size = ram_run_last(map, i) - 4 * GIB + 1;
            cmos->above_4g = saturate(size / (64 * 1024), 0xffffff);
        }
    }
}

static const char *type_name(uint32_t type) {
    switch (type) {
        case E820_RAM: {
            return "RAM";
        }
        case E820_RESERVED: {
            return "reserved";
        }
        case E820_ACPI: {
            return "ACPI";
        }
        case E820_NVS: {
            return "ACPI NVS";
        }
        case E820_UNUSABLE: {
            return "unusable";
        }
        case E820_PMEM: {
            return "persistent";
        }
        default: {
            return NULL;
        }
    }
}

void e820_log(const struct e820_map *map) {
    for (uint32_t i = 0; i < map->count; i++) {
        const struct e820_entry *e = &map->entries[i];
        const char *name = type_name(e->type);
        char unknown[16];
        if (name == NULL) {
            snprintf(unknown, sizeof(unknown), "type %u", (unsigned)e->type);
            name = unknown;
        }
        printf("E820 0x%016llx-0x%016llx %s\n", (unsigned long long)e->base,
               (unsigned long long)entry_last(e), name);
    }
}
