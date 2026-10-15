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
