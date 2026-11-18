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
#include <loader/kept.h>
#include <lib/util.h>
#include <loader/efi_glue.h>
#include <loader/loader.h>

#define AREA_FLOOR ((uint64_t)0x1000000)
#define AREA_CEILING ((uint64_t)0x100000000)

// The firmware's memory map, in a pool allocation, or NULL.
static uint8_t *memory_map(size_t *size, size_t *desc_size) {
    size_t map_key;
    uint32_t desc_version;
    *size = 0;
    *desc_size = 0;
    efi_bs->get_memory_map(size, NULL, &map_key, desc_size, &desc_version);
    // Room for the descriptors that the allocation itself may add.
    *size += 16 * *desc_size;
    uint8_t *map = efi_glue_alloc_pool(*size);
    if (map == NULL) {
        return NULL;
    }
    efi_status status = efi_bs->get_memory_map(
        size, (struct efi_memory_descriptor *)map, &map_key, desc_size, &desc_version
    );
    if (status != EFI_SUCCESS || *desc_size < sizeof(struct efi_memory_descriptor)) {
        efi_bs->free_pool(map);
        return NULL;
    }
    return map;
}

// The pages of a descriptor's free memory from AREA_FLOOR up to AREA_CEILING:
// false where it has none.
static bool free_part(const struct efi_memory_descriptor *d, uint64_t *base, uint64_t *end) {
    if (d->type != EFI_CONVENTIONAL_MEMORY) {
        return false;
    }
    uint64_t top = d->physical_start + d->number_of_pages * EFI_PAGE_SIZE;
    *base = page_up(d->physical_start > AREA_FLOOR ? d->physical_start : AREA_FLOOR);
    *end = top < AREA_CEILING ? top : AREA_CEILING;
    return *base < *end;
}

uint64_t kept_reserve(uint64_t size) {
    size_t map_size;
    size_t desc_size;
    uint8_t *map = memory_map(&map_size, &desc_size);
    if (map == NULL) {
        return 0;
    }
    uint64_t largest = 0;
    uint64_t largest_size = 0;
    for (size_t off = 0; off + desc_size <= map_size; off += desc_size) {
        uint64_t base;
        uint64_t end;
        if (free_part((const struct efi_memory_descriptor *)(map + off), &base, &end)
         && end - base > largest_size) {
            largest = base;
            largest_size = end - base;
        }
    }
    efi_bs->free_pool(map);
    if (largest_size < size) {
        return 0;
    }
    uint64_t area = largest + align_down((largest_size - size) / 2, EFI_PAGE_SIZE);
    size_t pages = page_count(size);
    efi_physical_address taken = area;
    if (efi_bs->allocate_pages(EFI_ALLOCATE_ADDRESS, EFI_LOADER_DATA, pages, &taken)
     != EFI_SUCCESS) {
        return 0;
    }
    loader_reserve(area, pages);
    return area;
}

void kept_find(bool (*found)(uintptr_t page, uint64_t end, void *ctx), void *ctx) {
    size_t size;
    size_t desc_size;
    uint8_t *map = memory_map(&size, &desc_size);
    if (map == NULL) {
        return;
    }
    bool done = false;
    for (size_t off = 0; off + desc_size <= size && !done; off += desc_size) {
        uint64_t base;
        uint64_t end;
        if (!free_part((const struct efi_memory_descriptor *)(map + off), &base, &end)) {
            continue;
        }
        for (uintptr_t page = base; page + EFI_PAGE_SIZE <= end && !done; page += EFI_PAGE_SIZE) {
            done = found(page, end, ctx);
        }
    }
    efi_bs->free_pool(map);
}
