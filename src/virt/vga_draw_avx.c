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

// The drawing kernels that AVX does better with its 32-byte moves and test,
// though its integer arithmetic stays 16 bytes wide. Built with AVX enabled.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <memory.h>
#include <virt/vga_draw.h>

typedef char c8x16 __attribute__((vector_size(16)));
typedef uint8_t u8x16 __attribute__((vector_size(16)));
typedef uint8_t u8x32 __attribute__((vector_size(32)));
typedef uint32_t u32x8 __attribute__((vector_size(32)));
typedef long long i64x4 __attribute__((vector_size(32)));

static inline u8x16 load16(const void *p) {
    u8x16 v;
    memcpy(&v, p, sizeof(u8x16));
    return v;
}

static inline u8x32 load(const void *p) {
    u8x32 v;
    memcpy(&v, p, sizeof(u8x32));
    return v;
}

static inline void store(void *p, u8x32 v) {
    memcpy(p, &v, sizeof(u8x32));
}

// A store past the caches, to a 32-byte aligned address.
static inline void stream(void *p, u8x32 v) {
    asm volatile ("vmovntdq %1, %0" : "=m"(*(u8x32 *)p) : "x"(v));
}

static inline bool match128(const uint8_t *a, const uint8_t *b) {
    u8x32 d = (load(a) ^ load(b)) | (load(a + 32) ^ load(b + 32)) | (load(a + 64) ^ load(b + 64))
            | (load(a + 96) ^ load(b + 96));
    return __builtin_ia32_ptestz256((i64x4)d, (i64x4)d) != 0;
}

static inline uint32_t differ_mask(const uint8_t *a, const uint8_t *b) {
    return ~(uint32_t)__builtin_ia32_pmovmskb128((c8x16)(load16(a) == load16(b))) & 0xffff;
}

static void differ(const void *a, const void *b, size_t size, size_t *first, size_t *end) {
    const uint8_t *p = a;
    const uint8_t *q = b;
    size_t i = 0;
    while (i + 128 <= size && match128(p + i, q + i)) {
        i += 128;
    }
    uint32_t differs = 0;
    for (; i + 16 <= size; i += 16) {
        differs = differ_mask(p + i, q + i);
        if (differs != 0) {
            i += (size_t)__builtin_ctz(differs);
            break;
        }
    }
    if (differs == 0) {
        while (i < size && p[i] == q[i]) {
            i++;
        }
        if (i == size) {
            *first = size;
            *end = size;
            return;
        }
    }
    *first = i;
    size_t j = size;
    while (j >= i + 128 && match128(p + j - 128, q + j - 128)) {
        j -= 128;
    }
    for (; j >= i + 16; j -= 16) {
        differs = differ_mask(p + j - 16, q + j - 16);
        if (differs != 0) {
            *end = j - 16 + (size_t)(31 - __builtin_clz(differs)) + 1;
            return;
        }
    }
    while (j > i + 1 && p[j - 1] == q[j - 1]) {
        j--;
    }
    *end = j;
}

static inline void copy_small(uint8_t *d, const uint8_t *s, size_t size) {
    size_t i = 0;
    for (; i + 4 <= size; i += 4) {
        uint32_t v;
        memcpy(&v, s + i, sizeof(uint32_t));
        memcpy(d + i, &v, sizeof(uint32_t));
    }
    for (; i < size; i++) {
        d[i] = s[i];
    }
}

// The first and the last 32 bytes unaligned, those between aligned on the
// destination, past the caches where streamed.
static inline void copy_vectors(uint8_t *d, const uint8_t *s, size_t size, bool streamed) {
    u8x32 head = load(s);
    u8x32 tail = load(s + size - 32);
    size_t i = 32 - ((uintptr_t)d & 31);
    for (; i + 128 <= size; i += 128) {
        u8x32 v0 = load(s + i);
        u8x32 v1 = load(s + i + 32);
        u8x32 v2 = load(s + i + 64);
        u8x32 v3 = load(s + i + 96);
        if (streamed) {
            stream(d + i, v0);
            stream(d + i + 32, v1);
            stream(d + i + 64, v2);
            stream(d + i + 96, v3);
        } else {
            store(d + i, v0);
            store(d + i + 32, v1);
            store(d + i + 64, v2);
            store(d + i + 96, v3);
        }
    }
    for (; i + 32 <= size; i += 32) {
        if (streamed) {
            stream(d + i, load(s + i));
        } else {
            store(d + i, load(s + i));
        }
    }
    store(d, head);
    store(d + size - 32, tail);
}

static void copy(void *dst, const void *src, size_t size) {
    if (size < 32) {
        copy_small(dst, src, size);
        return;
    }
    copy_vectors(dst, src, size, false);
}

static void copy_fb(void *dst, const void *src, size_t size) {
    if (size < 32) {
        copy_small(dst, src, size);
        return;
    }
    copy_vectors(dst, src, size, true);
    asm volatile ("sfence" ::: "memory");
}

static void fill(uint32_t *dst, uint32_t value, size_t count) {
    size_t x = 0;
    u32x8 v = { value, value, value, value, value, value, value, value };
    for (; x + 8 <= count; x += 8) {
        store(dst + x, (u8x32)v);
    }
    for (; x < count; x++) {
        dst[x] = value;
    }
}

const struct vga_draw_ops vga_draw_avx = {
    .differ = differ,
    .copy = copy,
    .copy_fb = copy_fb,
    .fill = fill
};
