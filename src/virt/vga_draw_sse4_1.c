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

// The drawing kernels that SSE4.1 does better: comparing with a test of all
// bits, packing to 16 bits unsigned, and blending by a mask. Built with SSE4.1
// enabled. SSE4.2 adds string comparisons and CRC32, of no use to any kernel,
// so its level draws with these.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <memory.h>
#include <virt/vga_draw.h>

typedef char c8x16 __attribute__((vector_size(16)));
typedef uint8_t u8x16 __attribute__((vector_size(16)));
typedef uint32_t u32x4 __attribute__((vector_size(16)));
typedef int32_t i32x4 __attribute__((vector_size(16)));
typedef long long i64x2 __attribute__((vector_size(16)));

static inline u8x16 load(const void *p) {
    u8x16 v;
    memcpy(&v, p, sizeof(u8x16));
    return v;
}

static inline void store(void *p, u8x16 v) {
    memcpy(p, &v, sizeof(u8x16));
}

static inline uint32_t differ_mask(u8x16 a, u8x16 b) {
    return ~(uint32_t)__builtin_ia32_pmovmskb128((c8x16)(a == b)) & 0xffff;
}

static inline bool match64(const uint8_t *a, const uint8_t *b) {
    u8x16 d = (load(a) ^ load(b)) | (load(a + 16) ^ load(b + 16)) | (load(a + 32) ^ load(b + 32))
            | (load(a + 48) ^ load(b + 48));
    return __builtin_ia32_ptestz128((i64x2)d, (i64x2)d) != 0;
}

static void differ(const void *a, const void *b, size_t size, size_t *first, size_t *end) {
    const uint8_t *p = a;
    const uint8_t *q = b;
    size_t i = 0;
    while (i + 64 <= size && match64(p + i, q + i)) {
        i += 64;
    }
    uint32_t differs = 0;
    for (; i + 16 <= size; i += 16) {
        differs = differ_mask(load(p + i), load(q + i));
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
    while (j >= i + 64 && match64(p + j - 64, q + j - 64)) {
        j -= 64;
    }
    for (; j >= i + 16; j -= 16) {
        differs = differ_mask(load(p + j - 16), load(q + j - 16));
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

static inline u32x4 pack4(u32x4 v, bool six) {
    u32x4 r = (v >> 19) & 0x1f;
    u32x4 b = (v >> 3) & 0x1f;
    if (six) {
        return r << 11 | ((v >> 10) & 0x3f) << 5 | b;
    }
    return r << 10 | ((v >> 11) & 0x1f) << 5 | b;
}

static inline void to_rgb16(uint16_t *dst, const uint32_t *src, size_t count, bool six) {
    size_t x = 0;
    for (; x + 8 <= count; x += 8) {
        u32x4 low = pack4((u32x4)load(src + x), six);
        u32x4 high = pack4((u32x4)load(src + x + 4), six);
        store(dst + x, (u8x16)__builtin_ia32_packusdw128((i32x4)low, (i32x4)high));
    }
    for (; x < count; x++) {
        dst[x] = vga_draw_pack16(src[x], six);
    }
}

static void to_rgb565(uint16_t *dst, const uint32_t *src, size_t count) {
    to_rgb16(dst, src, count, true);
}

static void to_rgb555(uint16_t *dst, const uint32_t *src, size_t count) {
    to_rgb16(dst, src, count, false);
}

static void dots(uint32_t *dst, uint8_t glyph, uint32_t fg, uint32_t bg) {
    u32x4 g = { glyph, glyph, glyph, glyph };
    u32x4 high_bits = { 0x80, 0x40, 0x20, 0x10 };
    u32x4 low_bits = { 0x08, 0x04, 0x02, 0x01 };
    u32x4 fgs = { fg, fg, fg, fg };
    u32x4 bgs = { bg, bg, bg, bg };
    c8x16 high = (c8x16)((g & high_bits) == high_bits);
    c8x16 low = (c8x16)((g & low_bits) == low_bits);
    store(dst, (u8x16)__builtin_ia32_pblendvb128((c8x16)bgs, (c8x16)fgs, high));
    store(dst + 4, (u8x16)__builtin_ia32_pblendvb128((c8x16)bgs, (c8x16)fgs, low));
}

static void text_line(
    uint32_t *line,
    const struct vga_draw_frame *f,
    const struct vga_draw_scan *s
) {
    vga_draw_text_line_with(line, f, s, dots);
}

const struct vga_draw_ops vga_draw_sse4_1 = {
    .differ = differ,
    .to_rgb565 = to_rgb565,
    .to_rgb555 = to_rgb555,
    .text_line = text_line
};
