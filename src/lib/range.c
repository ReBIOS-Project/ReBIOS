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
#include <lib/range.h>
#include <lib/heap.h>
#include <memory.h>

static void grow(struct range_list *list) {
    list->r = heap_grow(list->r, list->count, &list->capacity, sizeof(struct range), 32);
}

void range_list_free(struct range_list *list) {
    kfree(list->r);
    list->r = NULL;
    list->count = 0;
    list->capacity = 0;
}

void range_list_add(struct range_list *list, uintptr_t base, uintptr_t end) {
    if (base >= end) {
        return;
    }
    size_t first = 0;
    while (first < list->count && list->r[first].end < base) {
        first++;
    }
    size_t last = first;
    while (last < list->count && list->r[last].base <= end) {
        if (list->r[last].base < base) {
            base = list->r[last].base;
        }
        if (list->r[last].end > end) {
            end = list->r[last].end;
        }
        last++;
    }
    if (last == first) {
        grow(list);
        memmove(
            &list->r[first + 1], &list->r[first], (list->count - first) * sizeof(struct range)
        );
        list->count++;
    } else {
        memmove(&list->r[first + 1], &list->r[last], (list->count - last) * sizeof(struct range));
        list->count -= last - first - 1;
    }
    list->r[first].base = base;
    list->r[first].end = end;
}

void range_list_remove(struct range_list *list, uintptr_t base, uintptr_t end) {
    size_t i = 0;
    while (base < end && i < list->count) {
        struct range *x = &list->r[i];
        if (x->end <= base || x->base >= end) {
            i++;
        } else if (x->base < base && x->end > end) {
            uintptr_t old_end = x->end;
            x->end = base;
            grow(list);
            memmove(
                &list->r[i + 2], &list->r[i + 1], (list->count - i - 1) * sizeof(struct range)
            );
            list->count++;
            list->r[i + 1].base = end;
            list->r[i + 1].end = old_end;
            return;
        } else if (x->base < base) {
            x->end = base;
            i++;
        } else if (x->end > end) {
            x->base = end;
            i++;
        } else {
            memmove(x, x + 1, (list->count - i - 1) * sizeof(struct range));
            list->count--;
        }
    }
}

bool range_list_overlaps(const struct range_list *list, uintptr_t base, uintptr_t end) {
    for (size_t i = 0; i < list->count; i++) {
        if (list->r[i].base < end && base < list->r[i].end) {
            return true;
        }
    }
    return false;
}
