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
#include <stdbool.h>
#include <memory.h>
#include <loader/efi_glue.h>
#include <loader/paging.h>
#include <sys/x86.h>

#define PTE_P (1ull << 0)
#define PTE_W (1ull << 1)
#define PTE_PS (1ull << 7)

#define GIB (1ull << 30)

uint64_t paging_build_identity_map(uint64_t *map_top, uint64_t *pages) {
    bool huge_pages = cpuid(0x80000000, 0).eax >= 0x80000001
                      && (cpuid(0x80000001, 0).edx & (1u << 26));

    uint64_t top = (*map_top + GIB - 1) & ~(GIB - 1);
    uint64_t gib_count = top / GIB;
    uint64_t pdpt_count = (gib_count + 511) / 512;
    uint64_t pd_count = huge_pages ? 0 : gib_count;
    uint64_t table_pages = 1 + pdpt_count + pd_count;

    // The PML4 has 512 entries of 512GiB each.
    if (pdpt_count > 512) {
        return 0;
    }

    uint64_t tables = efi_alloc_pages(table_pages, UINT32_MAX);
    if (tables == 0) {
        return 0;
    }
    memset((void *)(uintptr_t)tables, 0, table_pages * 4096);

    uint64_t *pml4 = (uint64_t *)(uintptr_t)tables;
    uint64_t pdpt_base = tables + 4096;
    uint64_t pd_base = pdpt_base + pdpt_count * 4096;

    for (uint64_t i = 0; i < pdpt_count; i++) {
        pml4[i] = (pdpt_base + i * 4096) | PTE_P | PTE_W;
    }

    for (uint64_t gib = 0; gib < gib_count; gib++) {
        uint64_t *pdpte = (uint64_t *)(uintptr_t)(pdpt_base) + gib;
        if (huge_pages) {
            *pdpte = gib * GIB | PTE_P | PTE_W | PTE_PS;
            continue;
        }
        uint64_t pd_addr = pd_base + gib * 4096;
        uint64_t *pd = (uint64_t *)(uintptr_t)pd_addr;
        *pdpte = pd_addr | PTE_P | PTE_W;
        for (uint64_t j = 0; j < 512; j++) {
            pd[j] = (gib * GIB + j * 0x200000) | PTE_P | PTE_W | PTE_PS;
        }
    }

    *map_top = top;
    *pages = table_pages;
    return tables;
}
