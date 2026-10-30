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
#include <loader/paging.h>
#include <loader/efi_glue.h>
#include <loader/loader.h>
#include <memory.h>
#include <sys/x86.h>

#define PTE_P ((uint64_t)1 << 0)
#define PTE_W ((uint64_t)1 << 1)
#define PTE_PWT ((uint64_t)1 << 3)
#define PTE_PCD ((uint64_t)1 << 4)
#define PTE_PS ((uint64_t)1 << 7)
#define PTE_ADDRESS ((uint64_t)0x000ffffffffff000)

#define MIB2 ((uint64_t)1 << 21)
#define GIB ((uint64_t)1 << 30)

uintptr_t paging_build_identity_map(uintptr_t *map_top, size_t *pages) {
    bool huge_pages = cpuid(0x80000000, 0).eax >= 0x80000001
        && (cpuid(0x80000001, 0).edx & ((uint32_t)1 << 26)) != 0;

    uintptr_t top = (*map_top + GIB - 1) & ~(GIB - 1);
    size_t gib_count = top / GIB;
    size_t pdpt_count = (gib_count + 511) / 512;
    size_t pd_count = huge_pages ? 0 : gib_count;
    size_t table_pages = 1 + pdpt_count + pd_count;

    // The PML4 has 512 entries of 512GiB each.
    if (pdpt_count > 512) {
        return 0;
    }

    uintptr_t tables = efi_alloc_pages(table_pages, UINT32_MAX);
    if (tables == 0) {
        return 0;
    }
    memset((void *)(uintptr_t)tables, 0, table_pages * 4096);

    uint64_t *pml4 = (uint64_t *)(uintptr_t)tables;
    uintptr_t pdpt_base = tables + 4096;
    uintptr_t pd_base = pdpt_base + pdpt_count * 4096;

    for (size_t i = 0; i < pdpt_count; i++) {
        pml4[i] = (pdpt_base + i * 4096) | PTE_P | PTE_W;
    }

    for (size_t gib = 0; gib < gib_count; gib++) {
        uint64_t *pdpte = (uint64_t *)(uintptr_t)(pdpt_base) + gib;
        if (huge_pages) {
            *pdpte = gib * GIB | PTE_P | PTE_W | PTE_PS;
            continue;
        }
        uintptr_t pd_addr = pd_base + gib * 4096;
        uint64_t *pd = (uint64_t *)(uintptr_t)pd_addr;
        *pdpte = pd_addr | PTE_P | PTE_W;
        for (size_t j = 0; j < 512; j++) {
            pd[j] = (gib * GIB + j * 0x200000) | PTE_P | PTE_W | PTE_PS;
        }
    }

    *map_top = top;
    *pages = table_pages;
    return tables;
}

// Below 4GiB, as the tables paging_build_identity_map() makes.
static uint64_t *next_table(uint64_t *entry) {
    if ((*entry & PTE_P) == 0) {
        uintptr_t table = loader_alloc_reserved(1, UINT32_MAX);
        if (table == 0) {
            return NULL;
        }
        memset((void *)(uintptr_t)table, 0, 4096);
        *entry = table | PTE_P | PTE_W;
    }
    return (uint64_t *)(uintptr_t)(*entry & PTE_ADDRESS);
}

bool paging_map_device(uintptr_t pml4, uintptr_t base, size_t size) {
    uintptr_t end = (base + size + MIB2 - 1) & ~(MIB2 - 1);
    for (uintptr_t a = base & ~(MIB2 - 1); a < end; a += MIB2) {
        uint64_t *pdpt = next_table(
            (uint64_t *)(uintptr_t)pml4 + (a >> 39 & 511)
        );
        if (pdpt == NULL) {
            return false;
        }
        uint64_t *pdpte = pdpt + (a >> 30 & 511);
        if ((*pdpte & PTE_PS) != 0) {
            continue;
        }
        uint64_t *pd = next_table(pdpte);
        if (pd == NULL) {
            return false;
        }
        uint64_t *pde = pd + (a >> 21 & 511);
        if ((*pde & PTE_P) == 0) {
            *pde = a | PTE_P | PTE_W | PTE_PS | PTE_PCD | PTE_PWT;
        }
    }
    return true;
}
