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
#include <sys/mm.h>
#include <lib/log.h>
#include <lib/spinlock.h>
#include <memory.h>
#include <sys/x86.h>

// A bitmap of the heap's pages, set when used, kept at the start of the heap.
static uintptr_t heap_base;
static size_t heap_pages;
static uint64_t *heap_bitmap;
static uint64_t next_search;
static struct spinlock mm_lock;

static bool page_used(size_t i) {
    return (heap_bitmap[i / 64] >> (i % 64)) & 1;
}

static void page_mark(size_t i, bool used) {
    if (used) {
        heap_bitmap[i / 64] |= (uint64_t)1 << (i % 64);
    } else {
        heap_bitmap[i / 64] &= ~((uint64_t)1 << (i % 64));
    }
}

void mm_init(uintptr_t base, size_t size) {
    heap_base = base;
    heap_pages = size / PAGE_SIZE;
    heap_bitmap = (uint64_t *)(uintptr_t)base;

    size_t bitmap_bytes = (heap_pages + 63) / 64 * 8;
    size_t bitmap_pages = (bitmap_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    memset(heap_bitmap, 0, bitmap_pages * PAGE_SIZE);
    for (size_t i = 0; i < bitmap_pages; i++) {
        page_mark(i, true);
    }
    next_search = bitmap_pages;
}

void *page_alloc_aligned(size_t count, size_t align) {
    spinlock_acquire(&mm_lock);

    for (size_t pass = 0; pass < 2; pass++) {
        size_t first = pass == 0 ? next_search : 0;
        while (first + count <= heap_pages) {
            uintptr_t addr = heap_base + first * PAGE_SIZE;
            if (addr % align != 0) {
                first += (align - addr % align) / PAGE_SIZE;
                continue;
            }
            size_t i;
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
    size_t first = ((uintptr_t)pages - heap_base) / PAGE_SIZE;

    spinlock_acquire(&mm_lock);
    for (size_t i = first; i < first + count; i++) {
        page_mark(i, false);
    }
    if (first < next_search) {
        next_search = first;
    }
    spinlock_release(&mm_lock);
}

#define PTE_P ((uint64_t)1 << 0)
#define PTE_W ((uint64_t)1 << 1)
#define PTE_PWT ((uint64_t)1 << 3)
#define PTE_PCD ((uint64_t)1 << 4)
#define PTE_PS ((uint64_t)1 << 7)
#define PTE_PAT_4K ((uint64_t)1 << 7)
#define PTE_PAT_LARGE ((uint64_t)1 << 12)
#define PTE_ADDRESS ((uint64_t)0x000ffffffffff000)
#define PTE_LARGE_FLAGS ((uint64_t)0x0fff)
#define SIZE_2M ((uint64_t)0x200000)
#define SIZE_1G ((uint64_t)0x40000000)

static uint64_t *table_of(uint64_t entry) {
    return (uint64_t *)(uintptr_t)(entry & PTE_ADDRESS);
}

// The PAT entry the host's IA32_PAT makes write-combining is the one PWT alone
// selects.
static uint64_t write_combining(uint64_t entry, uint64_t pat_bit) {
    return (entry & ~(PTE_PCD | pat_bit)) | PTE_PWT;
}

// Replaces a large page with a table of 512 pages of the next size down, alike
// but for their addresses.
static void split(uint64_t *entry, size_t size, bool to_4k) {
    uint64_t *table = page_alloc(1);
    uintptr_t base = *entry & PTE_ADDRESS & ~(size - 1);
    uint64_t flags = *entry & PTE_LARGE_FLAGS;
    if (to_4k) {
        bool pat = *entry & PTE_PAT_LARGE;
        flags = (flags & ~PTE_PS) | (pat ? PTE_PAT_4K : 0);
    }
    for (size_t i = 0; i < 512; i++) {
        table[i] = (base + i * (size / 512)) | flags;
    }
    *entry = (uint64_t)(uintptr_t)table | (*entry & 0x7) | PTE_P;
}

static struct spinlock map_lock;

static uint64_t *next_table(uint64_t *entry) {
    if ((*entry & PTE_P) == 0) {
        *entry = (uint64_t)(uintptr_t)page_alloc(1) | PTE_P | PTE_W;
    }
    return table_of(*entry);
}

// 2 MiB pages, as the loader maps a framebuffer, in the first 256 TiB.
void mm_map_device(uintptr_t base, size_t size) {
    uint64_t *top = table_of(read_cr3());
    if ((read_cr4() & CR4_LA57) != 0) {
        top = table_of(top[0]);
    }
    uintptr_t end = (base + size + SIZE_2M - 1) & ~(SIZE_2M - 1);
    if (end > (uint64_t)1 << 48) {
        printf(
            "Cannot map device registers at %#llx\n", (unsigned long long)base
        );
        return;
    }
    spinlock_acquire(&map_lock);
    for (uintptr_t a = base & ~(SIZE_2M - 1); a < end; a += SIZE_2M) {
        uint64_t *pdpte = &next_table(&top[(a >> 39) & 511])[(a >> 30) & 511];
        if ((*pdpte & PTE_PS) != 0) {
            continue;
        }
        uint64_t *pde = &next_table(pdpte)[(a >> 21) & 511];
        if ((*pde & PTE_P) == 0) {
            *pde = a | PTE_P | PTE_W | PTE_PS | PTE_PCD | PTE_PWT;
        }
    }
    spinlock_release(&map_lock);
}

void mm_write_combining(uintptr_t base, size_t size) {
    uintptr_t start = base & ~(uint64_t)(PAGE_SIZE - 1);
    uintptr_t end = (base + size + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t *top = table_of(read_cr3());
    if ((read_cr4() & CR4_LA57) != 0) {
        top = table_of(top[0]);
    }
    for (uintptr_t a = start; a < end; ) {
        uint64_t *pml4e = &top[(a >> 39) & 511];
        if ((*pml4e & PTE_P) == 0) {
            return;
        }
        uint64_t *pdpte = &table_of(*pml4e)[(a >> 30) & 511];
        if ((*pdpte & PTE_P) == 0) {
            return;
        }
        if ((*pdpte & PTE_PS) != 0) {
            if (a % SIZE_1G == 0 && end - a >= SIZE_1G) {
                *pdpte = write_combining(*pdpte, PTE_PAT_LARGE);
                a += SIZE_1G;
                continue;
            }
            split(pdpte, SIZE_1G, false);
        }
        uint64_t *pde = &table_of(*pdpte)[(a >> 21) & 511];
        if ((*pde & PTE_PS) != 0) {
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
