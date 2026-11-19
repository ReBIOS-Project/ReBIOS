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
#include <loader/loader.h>
#include <handoff.h>
#include <lib/log.h>
#include <loader/efi_glue.h>
#include <memory.h>

// The ranges that the list first has room for, a page's worth.
#define RESERVED_FIRST (EFI_PAGE_SIZE / sizeof(struct handoff_range))

struct handoff *loader_handoff;
// How many ranges the list's pages have room for.
static size_t reserved_capacity;

static size_t list_pages(size_t capacity) {
    return EFI_SIZE_TO_PAGES(capacity * sizeof(struct handoff_range));
}

bool loader_reserve_room(size_t more) {
    struct handoff *h = loader_handoff;
    if (h->reserved_count + more <= reserved_capacity) {
        return true;
    }
    size_t capacity = reserved_capacity != 0 ? reserved_capacity : RESERVED_FIRST;
    while (capacity < h->reserved_count + more) {
        capacity *= 2;
    }
    uintptr_t grown = efi_glue_alloc_pages(list_pages(capacity), 0);
    if (grown == 0) {
        return false;
    }
    if (h->reserved != NULL) {
        memcpy((void *)grown, h->reserved, h->reserved_count * sizeof(struct handoff_range));
        efi_bs->free_pages((uintptr_t)h->reserved, list_pages(reserved_capacity));
    }
    h->reserved = (struct handoff_range *)grown;
    reserved_capacity = capacity;
    return true;
}

void loader_reserve_list(void) {
    loader_reserve((uintptr_t)loader_handoff->reserved, list_pages(reserved_capacity));
}

void loader_reserve(uintptr_t base, size_t pages) {
    size_t size = pages * EFI_PAGE_SIZE;
    // Pages that touch a range already kept join it, as those allocated one at
    // a time, page tables among them, mostly come out next to each other.
    for (size_t i = 0; i < loader_handoff->reserved_count; i++) {
        struct handoff_range *r = &loader_handoff->reserved[i];
        if (base == r->base + r->size) {
            r->size += size;
            return;
        }
        if (base + size == r->base) {
            r->base = base;
            r->size += size;
            return;
        }
    }
    if (!loader_reserve_room(1)) {
        panic("No memory for the list of the hypervisor's memory ranges\n");
    }
    loader_handoff->reserved[loader_handoff->reserved_count].base = base;
    loader_handoff->reserved[loader_handoff->reserved_count].size = size;
    loader_handoff->reserved_count++;
}

uint64_t loader_alloc_reserved(size_t pages, uintptr_t max_addr) {
    uintptr_t base = efi_glue_alloc_pages(pages, max_addr);
    if (base != 0) {
        loader_reserve(base, pages);
    }
    return base;
}
