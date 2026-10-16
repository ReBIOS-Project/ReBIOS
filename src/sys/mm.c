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
#include <memory.h>
#include <lib/log.h>
#include <lib/spinlock.h>
#include <sys/mm.h>

// A bitmap of the heap's pages, set when used, kept at the start of the heap.
static uint64_t heap_base;
static uint64_t heap_pages;
static uint64_t *heap_bitmap;
static uint64_t next_search;
static struct spinlock mm_lock;

static bool page_used(uint64_t i) {
    return (heap_bitmap[i / 64] >> (i % 64)) & 1;
}

static void page_mark(uint64_t i, bool used) {
    if (used) {
        heap_bitmap[i / 64] |= 1ull << (i % 64);
    } else {
        heap_bitmap[i / 64] &= ~(1ull << (i % 64));
    }
}

void mm_init(uint64_t base, uint64_t size) {
    heap_base = base;
    heap_pages = size / PAGE_SIZE;
    heap_bitmap = (uint64_t *)(uintptr_t)base;

    uint64_t bitmap_bytes = (heap_pages + 63) / 64 * 8;
    uint64_t bitmap_pages = (bitmap_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    memset(heap_bitmap, 0, bitmap_pages * PAGE_SIZE);
    for (uint64_t i = 0; i < bitmap_pages; i++) {
        page_mark(i, true);
    }
    next_search = bitmap_pages;
}

void *page_alloc_aligned(size_t count, size_t align) {
    spinlock_acquire(&mm_lock);

    for (int pass = 0; pass < 2; pass++) {
        uint64_t first = pass == 0 ? next_search : 0;
        while (first + count <= heap_pages) {
            uint64_t addr = heap_base + first * PAGE_SIZE;
            if (addr % align != 0) {
                first += (align - addr % align) / PAGE_SIZE;
                continue;
            }
            uint64_t i;
            for (i = first; i < first + count; i++) {
                if (page_used(i)) {
                    break;
                }
            }
            if (i < first + count) {
                first = i + 1;
                continue;
            }
            for (i = first; i < first + count; i++) {
                page_mark(i, true);
            }
            if (align == PAGE_SIZE) {
                next_search = first + count;
            }
            spinlock_release(&mm_lock);

            void *pages = (void *)(uintptr_t)addr;
            memset(pages, 0, count * PAGE_SIZE);
            return pages;
        }
    }

    spinlock_release(&mm_lock);
    panic("Out of memory allocating %zu pages\n", count);
}

void *page_alloc(size_t count) {
    return page_alloc_aligned(count, PAGE_SIZE);
}

void page_free(void *pages, size_t count) {
    uint64_t first = ((uint64_t)(uintptr_t)pages - heap_base) / PAGE_SIZE;

    spinlock_acquire(&mm_lock);
    for (uint64_t i = first; i < first + count; i++) {
        page_mark(i, false);
    }
    if (first < next_search) {
        next_search = first;
    }
    spinlock_release(&mm_lock);
}

#define PTE_P (1ull << 0)
#define PTE_PWT (1ull << 3)
#define PTE_PCD (1ull << 4)
#define PTE_PS (1ull << 7)
#define PTE_PAT_4K (1ull << 7)
#define PTE_PAT_LARGE (1ull << 12)
#define PTE_ADDRESS 0x000ffffffffff000ull
#define PTE_LARGE_FLAGS 0x0fffull
#define SIZE_2M 0x200000ull
#define SIZE_1G 0x40000000ull

static uint64_t *table_of(uint64_t entry) {
    return (uint64_t *)(uintptr_t)(entry & PTE_ADDRESS);
}

// The PAT entry the host's IA32_PAT makes write-combining is the one PWT
// alone selects.
static uint64_t write_combining(uint64_t entry, uint64_t pat_bit) {
    return (entry & ~(PTE_PCD | pat_bit)) | PTE_PWT;
}

// Replaces a large page with a table of 512 pages of the next size down,
// alike but for their addresses.
static void split(uint64_t *entry, uint64_t size, bool to_4k) {
    uint64_t *table = page_alloc(1);
    uint64_t base = *entry & PTE_ADDRESS & ~(size - 1);
    uint64_t flags = *entry & PTE_LARGE_FLAGS;
    if (to_4k) {
        bool pat = *entry & PTE_PAT_LARGE;
        flags = (flags & ~PTE_PS) | (pat ? PTE_PAT_4K : 0);
    }
    for (uint64_t i = 0; i < 512; i++) {
        table[i] = (base + i * (size / 512)) | flags;
    }
    *entry = (uint64_t)(uintptr_t)table | (*entry & 0x7) | PTE_P;
}

void mm_write_combining(uint64_t base, uint64_t size) {
    uint64_t start = base & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t end = (base + size + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t *top = table_of(read_cr3());
    if (read_cr4() & CR4_LA57) {
        top = table_of(top[0]);
    }
    for (uint64_t a = start; a < end;) {
        uint64_t *pml4e = &top[(a >> 39) & 511];
        if (!(*pml4e & PTE_P)) {
            return;
        }
        uint64_t *pdpte = &table_of(*pml4e)[(a >> 30) & 511];
        if (!(*pdpte & PTE_P)) {
            return;
        }
        if (*pdpte & PTE_PS) {
            if (a % SIZE_1G == 0 && end - a >= SIZE_1G) {
                *pdpte = write_combining(*pdpte, PTE_PAT_LARGE);
                a += SIZE_1G;
                continue;
            }
            split(pdpte, SIZE_1G, false);
        }
        uint64_t *pde = &table_of(*pdpte)[(a >> 21) & 511];
        if (*pde & PTE_PS) {
            if (a % SIZE_2M == 0 && end - a >= SIZE_2M) {
                *pde = write_combining(*pde, PTE_PAT_LARGE);
                a += SIZE_2M;
                continue;
            }
            split(pde, SIZE_2M, true);
        }
        uint64_t *pte = &table_of(*pde)[(a >> 12) & 511];
        *pte = write_combining(*pte, PTE_PAT_4K);
        a += PAGE_SIZE;
    }
    write_cr3(read_cr3());
}
