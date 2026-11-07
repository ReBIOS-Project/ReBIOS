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

#ifndef LIB__RANGE_H
#define LIB__RANGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// [base, end).
struct range {
    uintptr_t base;
    uintptr_t end;
};

// Ranges in order, none overlapping or touching another, which take more
// memory as they need it. A zeroed list is empty.
struct range_list {
    struct range *r;
    size_t count;
    size_t capacity;
};

// Adds [base, end), merged with the ranges it overlaps or touches.
void range_list_add(struct range_list *list, uintptr_t base, uintptr_t end);
// Takes [base, end) out of the ranges.
void range_list_remove(struct range_list *list, uintptr_t base, uintptr_t end);
// Whether a range overlaps [base, end).
bool range_list_overlaps(const struct range_list *list, uintptr_t base, uintptr_t end);
// Gives the memory back, leaving the list empty.
void range_list_free(struct range_list *list);

#endif
