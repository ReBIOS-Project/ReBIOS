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
#include <lib/util.h>
#include <memory.h>
#include <sys/x86.h>

// A bitmap of the heap's pages, set when used, kept at the start of the heap.
static uintptr_t heap_base;
static size_t heap_pages;
static uint64_t *heap_bitmap;
static uint64_t next_search;
static struct spinlock mm_lock;

static bool page_used(size_t i) {
    return ((heap_bitmap[i / 64] >> (i % 64)) & 1) != 0;
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
    size_t bitmap_pages = page_count(bitmap_bytes);
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

#define PTE_LARGE_FLAGS ((uint64_t)0x0fff)

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
    uintptr_t base = align_down(*entry & PTE_ADDRESS, size);
    uint64_t flags = *entry & PTE_LARGE_FLAGS;
    if (to_4k) {
        bool pat = (*entry & PTE_PAT_LARGE) != 0;
        flags = (flags & ~PTE_PS) | (pat ? PTE_PAT_4K : 0);
    }
    for (size_t i = 0; i < 512; i++) {
        table[i] = (base + i * (size / 512)) | flags;
    }
    *entry = (uint64_t)(uintptr_t)table | (*entry & 0x7) | PTE_P;
}

static struct spinlock map_lock;

static uint64_t *next_table(uint64_t *entry, void *(*alloc_table)(void)) {
    if ((*entry & PTE_P) == 0) {
        void *table = alloc_table();
        if (table == NULL) {
            return NULL;
        }
        *entry = (uint64_t)(uintptr_t)table | PTE_P | PTE_W;
    }
    return table_of(*entry);
}

// 2 MiB pages, in the first 256 TiB.
bool mm_map_device_under(uint64_t *top, uintptr_t base, size_t size, void *(*alloc_table)(void)) {
    uintptr_t end = align_up(base + size, TWO_MIB);
    if (end > (uint64_t)1 << 48) {
        return false;
    }
    for (uintptr_t a = align_down(base, TWO_MIB); a < end; a += TWO_MIB) {
        uint64_t *pdpt = next_table(&top[x86_level_index(a, 3)], alloc_table);
        if (pdpt == NULL) {
            return false;
        }
        uint64_t *pdpte = &pdpt[x86_level_index(a, 2)];
        if ((*pdpte & PTE_PS) != 0) {
            continue;
        }
        uint64_t *pd = next_table(pdpte, alloc_table);
        if (pd == NULL) {
            return false;
        }
        uint64_t *pde = &pd[x86_level_index(a, 1)];
        if ((*pde & PTE_P) == 0) {
            *pde = a | PTE_P | PTE_W | PTE_PS | PTE_PCD | PTE_PWT;
        }
    }
    return true;
}

static void *heap_table(void) {
    return page_alloc(1);
}

void mm_map_device(uintptr_t base, size_t size) {
    uint64_t *top = table_of(read_cr3());
    if ((read_cr4() & CR4_LA57) != 0) {
        top = table_of(top[0]);
    }
    spinlock_acquire(&map_lock);
    bool mapped = mm_map_device_under(top, base, size, heap_table);
    spinlock_release(&map_lock);
    if (!mapped) {
        printf("Cannot map device registers at %#llx\n", (unsigned long long)base);
    }
}

void mm_write_combining(uintptr_t base, size_t size) {
    uintptr_t start = page_down(base);
    uintptr_t end = page_up(base + size);
    uint64_t *top = table_of(read_cr3());
    if ((read_cr4() & CR4_LA57) != 0) {
        top = table_of(top[0]);
    }
    for (uintptr_t a = start; a < end; ) {
        uint64_t *pml4e = &top[x86_level_index(a, 3)];
        if ((*pml4e & PTE_P) == 0) {
            break;
        }
        uint64_t *pdpte = &table_of(*pml4e)[x86_level_index(a, 2)];
        if ((*pdpte & PTE_P) == 0) {
            break;
        }
        if ((*pdpte & PTE_PS) != 0) {
            if (a % ONE_GIB == 0 && end - a >= ONE_GIB) {
                *pdpte = write_combining(*pdpte, PTE_PAT_LARGE);
                a += ONE_GIB;
                continue;
            }
            split(pdpte, ONE_GIB, false);
        }
        uint64_t *pde = &table_of(*pdpte)[x86_level_index(a, 1)];
        if ((*pde & PTE_P) == 0) {
            break;
        }
        if ((*pde & PTE_PS) != 0) {
            if (a % TWO_MIB == 0 && end - a >= TWO_MIB) {
                *pde = write_combining(*pde, PTE_PAT_LARGE);
                a += TWO_MIB;
                continue;
            }
            split(pde, TWO_MIB, true);
        }
        uint64_t *pte = &table_of(*pde)[x86_level_index(a, 0)];
        *pte = write_combining(*pte, PTE_PAT_4K);
        a += PAGE_SIZE;
    }
    write_cr3(read_cr3());
}
