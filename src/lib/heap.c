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
#include <memory.h>
#include <lib/heap.h>
#include <lib/spinlock.h>
#include <sys/mm.h>

// Blocks of up to 2KiB come from per-size free lists, refilled a page at a
// time; larger ones are whole pages. A 16-byte header ahead of each block,
// kept for alignment, says which.
#define HEADER_SIZE 16
#define MIN_CLASS_SHIFT 5
#define MAX_CLASS_SHIFT 11
#define CLASS_COUNT (MAX_CLASS_SHIFT - MIN_CLASS_SHIFT + 1)
#define LARGE_MARK 0xffffffffffffffffull

struct free_block {
    struct free_block *next;
};

struct header {
    uint64_t class_or_mark;
    uint64_t pages;
};

static struct free_block *free_lists[CLASS_COUNT];
static struct spinlock heap_lock;

static int size_class(size_t total) {
    for (int c = 0; c < CLASS_COUNT; c++) {
        if (total <= (1ull << (MIN_CLASS_SHIFT + c))) {
            return c;
        }
    }
    return -1;
}

void *kmalloc(size_t size) {
    size_t total = size + HEADER_SIZE;
    int c = size_class(total);

    if (c < 0) {
        size_t pages = (total + PAGE_SIZE - 1) / PAGE_SIZE;
        struct header *h = page_alloc(pages);
        h->class_or_mark = LARGE_MARK;
        h->pages = pages;
        return (uint8_t *)h + HEADER_SIZE;
    }

    size_t block = 1ull << (MIN_CLASS_SHIFT + c);
    spinlock_acquire(&heap_lock);
    if (free_lists[c] == NULL) {
        uint8_t *page = page_alloc(1);
        for (size_t off = 0; off + block <= PAGE_SIZE; off += block) {
            struct free_block *b = (struct free_block *)(page + off);
            b->next = free_lists[c];
            free_lists[c] = b;
        }
    }
    struct free_block *b = free_lists[c];
    free_lists[c] = b->next;
    spinlock_release(&heap_lock);

    memset(b, 0, block);
    struct header *h = (struct header *)b;
    h->class_or_mark = (uint64_t)c;
    return (uint8_t *)h + HEADER_SIZE;
}

void kfree(void *ptr) {
    if (ptr == NULL) {
        return;
    }

    struct header *h = (struct header *)((uint8_t *)ptr - HEADER_SIZE);
    if (h->class_or_mark == LARGE_MARK) {
        page_free(h, h->pages);
        return;
    }

    int c = (int)h->class_or_mark;
    struct free_block *b = (struct free_block *)h;
    spinlock_acquire(&heap_lock);
    b->next = free_lists[c];
    free_lists[c] = b;
    spinlock_release(&heap_lock);
}
