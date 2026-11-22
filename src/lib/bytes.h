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

#ifndef LIB__BYTES_H
#define LIB__BYTES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// n bytes from src to dest, which may overlap only where dest is below src:
// quadwords, then the bytes left over, through the string instructions, which
// the processor can carry out a cache line or more at a time (SDM 325462-093
// Vol. 1, 7.3.9.3), where memcpy() goes a byte at a time. DF stays clear, as
// the ABI keeps it.
static inline void bytes_copy(void *dest, const void *src, size_t n) {
    size_t quads = n / 8;
    size_t rest = n % 8;
    asm volatile (
        "rep movsq\n\t"
        "movq %[rest], %%rcx\n\t"
        "rep movsb"
        : [dest] "+D"(dest), [src] "+S"(src), [quads] "+c"(quads)
        : [rest] "r"(rest)
        : "memory"
    );
}

// n bytes from src to dest, which may overlap either way, as memmove(): those
// above src a quadword at a time from the top, each read before the write that
// could reach it.
static inline void bytes_move(void *dest, const void *src, size_t n) {
    uint8_t *to = dest;
    const uint8_t *from = src;
    if (to <= from || to >= from + n) {
        bytes_copy(dest, src, n);
        return;
    }
    size_t at = n;
    if (to - from >= 8) {
        for (; at >= 8; at -= 8) {
            uint64_t quad = *(const uint64_t *)(const void *)(from + at - 8);
            *(uint64_t *)(void *)(to + at - 8) = quad;
        }
    }
    while (at-- > 0) {
        to[at] = from[at];
    }
}

// n bytes at dest set to value, as bytes_copy() copies them.
static inline void bytes_fill(void *dest, uint8_t value, size_t n) {
    uint64_t quad = value * (uint64_t)0x0101010101010101;
    size_t quads = n / 8;
    size_t rest = n % 8;
    asm volatile (
        "rep stosq\n\t"
        "movq %[rest], %%rcx\n\t"
        "rep stosb"
        : [dest] "+D"(dest), [quads] "+c"(quads)
        : [quad] "a"(quad), [rest] "r"(rest)
        : "memory"
    );
}

// quads quadwords at dest set to quad, as bytes_fill() sets them.
static inline void bytes_fill_quads(void *dest, uint64_t quad, size_t quads) {
    asm volatile (
        "rep stosq"
        : [dest] "+D"(dest), [quads] "+c"(quads)
        : [quad] "a"(quad)
        : "memory"
    );
}

// Whether n bytes at a and at b are the same, compared a quadword at a time.
static inline bool bytes_equal(const void *a, const void *b, size_t n) {
    const uint8_t *x = a;
    const uint8_t *y = b;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t p = *(const uint64_t *)(const void *)(x + i);
        uint64_t q = *(const uint64_t *)(const void *)(y + i);
        if (p != q) {
            return false;
        }
    }
    for (; i < n; i++) {
        if (x[i] != y[i]) {
            return false;
        }
    }
    return true;
}

#endif
