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

#ifndef LIB__UTIL_H
#define LIB__UTIL_H

#include <stddef.h>
#include <stdint.h>
#include <sys/mm.h>

#define ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))

#define ONE_MIB ((uint64_t)0x100000)
#define TWO_MIB ((uint64_t)0x200000)
#define ONE_GIB ((uint64_t)0x40000000)
#define FOUR_GIB ((uint64_t)0x100000000)

// Rounds value up or down to a multiple of alignment, a power of two.
static inline uint64_t align_up(uint64_t value, uint64_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

static inline uint64_t align_down(uint64_t value, uint64_t alignment) {
    return value & ~(alignment - 1);
}

static inline uint64_t page_up(uint64_t value) {
    return align_up(value, PAGE_SIZE);
}

static inline uint64_t page_down(uint64_t value) {
    return align_down(value, PAGE_SIZE);
}

// The pages that size bytes take up, the last of them maybe in part.
static inline size_t page_count(size_t size) {
    return (size + PAGE_SIZE - 1) / PAGE_SIZE;
}

#endif
