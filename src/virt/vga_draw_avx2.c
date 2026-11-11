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

// The drawing kernels with AVX2's 32-byte integer arithmetic, whose byte
// shuffles and packs work on each 16-byte half alone. Built with AVX2 enabled.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <memory.h>
#include <virt/vga_draw.h>

typedef char c8x32 __attribute__((vector_size(32)));
typedef uint8_t u8x8 __attribute__((vector_size(8)));
typedef uint8_t u8x16 __attribute__((vector_size(16)));
typedef uint8_t u8x32 __attribute__((vector_size(32)));
typedef uint16_t u16x8 __attribute__((vector_size(16)));
typedef uint16_t u16x16 __attribute__((vector_size(32)));
typedef int16_t i16x16 __attribute__((vector_size(32)));
typedef uint32_t u32x8 __attribute__((vector_size(32)));
typedef uint64_t u64x4 __attribute__((vector_size(32)));
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

static inline u8x32 join(u8x16 low, u8x16 high) {
    return __builtin_shufflevector(
        low, high, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21,
        22, 23, 24, 25, 26, 27, 28, 29, 30, 31
    );
}

// Bytes of table at the positions control gives, within each 16-byte half, 0
// where its bit 7 is set.
static inline u8x32 shuffle(u8x32 table, u8x32 control) {
    return (u8x32)__builtin_ia32_pshufb256((c8x32)table, (c8x32)control);
}

static inline bool match128(const uint8_t *a, const uint8_t *b) {
    u8x32 d = (load(a) ^ load(b)) | (load(a + 32) ^ load(b + 32)) | (load(a + 64) ^ load(b + 64))
            | (load(a + 96) ^ load(b + 96));
    return __builtin_ia32_ptestz256((i64x4)d, (i64x4)d) != 0;
}

static inline uint32_t differ_mask(const uint8_t *a, const uint8_t *b) {
    return ~(uint32_t)__builtin_ia32_pmovmskb256((c8x32)(load(a) == load(b)));
}

static void differ(const void *a, const void *b, size_t size, size_t *first, size_t *end) {
    const uint8_t *p = a;
    const uint8_t *q = b;
    size_t i = 0;
    while (i + 128 <= size && match128(p + i, q + i)) {
        i += 128;
    }
    uint32_t differs = 0;
    for (; i + 32 <= size; i += 32) {
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
    for (; j >= i + 32; j -= 32) {
        differs = differ_mask(p + j - 32, q + j - 32);
        if (differs != 0) {
            *end = j - 32 + (size_t)(31 - __builtin_clz(differs)) + 1;
            return;
        }
    }
    while (j > i + 1 && p[j - 1] == q[j - 1]) {
        j--;
    }
    *end = j;
}

// 8 entries of a table at the indices given, by a gather, of which GCC and
// Clang name the builtin differently.
static inline u32x8 gather(const uint32_t *table, u32x8 index) {
    u32x8 result = { 0 };
    u32x8 mask = { 0 };
    mask = ~mask;
    asm (
        "vpgatherdd %1, (%3, %2, 4), %0"
        : "+x"(result), "+x"(mask)
        : "x"(index), "r"(table), "m"(*(const uint32_t (*)[256])table)
    );
    return result;
}

static void convert8(uint32_t *dst, const uint8_t *src, size_t count, const uint32_t *palette) {
    size_t x = 0;
    for (; x + 8 <= count; x += 8) {
        u8x8 bytes;
        memcpy(&bytes, src + x, sizeof(u8x8));
        store(dst + x, (u8x32)gather(palette, __builtin_convertvector(bytes, u32x8)));
    }
    for (; x < count; x++) {
        dst[x] = palette[src[x]];
    }
}

static inline void convert16_any(uint32_t *dst, const uint8_t *src, size_t count, bool six) {
    size_t x = 0;
    for (; x + 8 <= count; x += 8) {
        u32x8 p = __builtin_convertvector((u16x8)load16(src + 2 * x), u32x8);
        u32x8 r = (p >> (six ? 11 : 10)) & 0x1f;
        u32x8 g = (p >> 5) & (six ? 0x3f : 0x1f);
        u32x8 b = p & 0x1f;
        r = (r << 3) | (r >> 2);
        g = six ? (g << 2) | (g >> 4) : (g << 3) | (g >> 2);
        b = (b << 3) | (b >> 2);
        store(dst + x, (u8x32)(r << 16 | g << 8 | b));
    }
    for (; x < count; x++) {
        dst[x] = vga_draw_expand16(src[2 * x] | (uint32_t)src[2 * x + 1] << 8, six);
    }
}

static void convert15(uint32_t *dst, const uint8_t *src, size_t count) {
    convert16_any(dst, src, count, false);
}

static void convert16(uint32_t *dst, const uint8_t *src, size_t count) {
    convert16_any(dst, src, count, true);
}

static void convert24(uint32_t *dst, const uint8_t *src, size_t count) {
    // 4 pixels from each 12 bytes in each half, of the 16 that its load takes,
    // so the last 2 pixels are left for one at a time.
    const u8x32 spread = {
        0, 1, 2, 0x80, 3, 4, 5, 0x80, 6, 7, 8, 0x80, 9, 10, 11, 0x80, 0, 1, 2, 0x80, 3, 4, 5, 0x80,
        6, 7, 8, 0x80, 9, 10, 11, 0x80
    };
    size_t x = 0;
    for (; x + 8 + 2 <= count; x += 8) {
        const uint8_t *p = src + 3 * x;
        store(dst + x, shuffle(join(load16(p), load16(p + 12)), spread));
    }
    for (; x < count; x++) {
        const uint8_t *q = src + 3 * x;
        dst[x] = (uint32_t)q[2] << 16 | (uint32_t)q[1] << 8 | q[0];
    }
}

static void convert32(uint32_t *dst, const uint8_t *src, size_t count) {
    size_t x = 0;
    for (; x + 8 <= count; x += 8) {
        store(dst + x, (u8x32)((u32x8)load(src + 4 * x) & 0x00ffffff));
    }
    for (; x < count; x++) {
        const uint8_t *q = src + 4 * x;
        dst[x] = (uint32_t)q[2] << 16 | (uint32_t)q[1] << 8 | q[0];
    }
}

// The 8 pixels of a and b mixed, as the 16-byte halves unpack and pack them:
// weights_low for the bytes of pixels 0, 1, 4, and 5, weights_high for the
// others. Each byte's a * (ONE - weight) + b * weight fits 16 bits.
static inline u8x32 mix(u8x32 a, u8x32 b, u16x16 weights_low, u16x16 weights_high) {
    u8x32 z = { 0 };
    u16x16 a_low = (u16x16)__builtin_shufflevector(
        a, z, 0, 32, 1, 32, 2, 32, 3, 32, 4, 32, 5, 32, 6, 32, 7, 32, 16, 32, 17, 32, 18, 32, 19,
        32, 20, 32, 21, 32, 22, 32, 23, 32
    );
    u16x16 a_high = (u16x16)__builtin_shufflevector(
        a, z, 8, 32, 9, 32, 10, 32, 11, 32, 12, 32, 13, 32, 14, 32, 15, 32, 24, 32, 25, 32, 26, 32,
        27, 32, 28, 32, 29, 32, 30, 32, 31, 32
    );
    u16x16 b_low = (u16x16)__builtin_shufflevector(
        b, z, 0, 32, 1, 32, 2, 32, 3, 32, 4, 32, 5, 32, 6, 32, 7, 32, 16, 32, 17, 32, 18, 32, 19,
        32, 20, 32, 21, 32, 22, 32, 23, 32
    );
    u16x16 b_high = (u16x16)__builtin_shufflevector(
        b, z, 8, 32, 9, 32, 10, 32, 11, 32, 12, 32, 13, 32, 14, 32, 15, 32, 24, 32, 25, 32, 26, 32,
        27, 32, 28, 32, 29, 32, 30, 32, 31, 32
    );
    u16x16 low = (a_low * (VGA_DRAW_FILTER_ONE - weights_low) + b_low * weights_low)
              >> VGA_DRAW_FILTER_SHIFT;
    u16x16 high = (a_high * (VGA_DRAW_FILTER_ONE - weights_high) + b_high * weights_high)
               >> VGA_DRAW_FILTER_SHIFT;
    return (u8x32)__builtin_ia32_packuswb256((i16x16)low, (i16x16)high);
}

static inline u8x32 scale_groups(
    const uint32_t *src,
    const struct vga_draw_scale *scale,
    size_t x
) {
    size_t g = x / VGA_DRAW_GROUP;
    u8x32 picks = load(scale->picks + 16 * g);
    u8x32 windows = join(load16(src + scale->window[g]), load16(src + scale->window[g + 1]));
    u8x32 a = shuffle(windows, picks);
    if (!scale->filter) {
        return a;
    }
    const uint32_t *next = src + 1;
    windows = join(load16(next + scale->window[g]), load16(next + scale->window[g + 1]));
    u8x32 b = shuffle(windows, picks);
    u64x4 w0 = (u64x4)load(scale->weights + 4 * x);
    u64x4 w1 = (u64x4)load(scale->weights + 4 * x + 16);
    u16x16 low = (u16x16)__builtin_shufflevector(w0, w1, 0, 1, 4, 5);
    u16x16 high = (u16x16)__builtin_shufflevector(w0, w1, 2, 3, 6, 7);
    return mix(a, b, low, high);
}

static void scale(
    uint32_t *dst,
    const uint32_t *src,
    const struct vga_draw_scale *scale,
    size_t x0,
    size_t x1
) {
    size_t x = x0;
    for (; x < x1 && x % VGA_DRAW_GROUP != 0; x++) {
        dst[x] = vga_draw_scale_one(src, scale, x);
    }
    for (; x + 2 * VGA_DRAW_GROUP <= x1; x += 2 * VGA_DRAW_GROUP) {
        size_t g = x / VGA_DRAW_GROUP;
        if (scale->fits[g] && scale->fits[g + 1]) {
            store(dst + x, scale_groups(src, scale, x));
            continue;
        }
        for (size_t i = 0; i < 2 * VGA_DRAW_GROUP; i++) {
            dst[x + i] = vga_draw_scale_one(src, scale, x + i);
        }
    }
    for (; x < x1; x++) {
        dst[x] = vga_draw_scale_one(src, scale, x);
    }
}

static void blend(
    uint32_t *dst,
    const uint32_t *a,
    const uint32_t *b,
    uint32_t weight,
    size_t count
) {
    size_t x = 0;
    u16x16 w = { 0 };
    w += (uint16_t)weight;
    for (; x + 8 <= count; x += 8) {
        store(dst + x, mix(load(a + x), load(b + x), w, w));
    }
    for (; x < count; x++) {
        dst[x] = vga_draw_blend_one(a[x], b[x], weight);
    }
}

static void to_xbgr(uint32_t *dst, const uint32_t *src, size_t count) {
    const u8x32 swap = {
        2, 1, 0, 3, 6, 5, 4, 7, 10, 9, 8, 11, 14, 13, 12, 15, 2, 1, 0, 3, 6, 5, 4, 7, 10, 9, 8, 11,
        14, 13, 12, 15
    };
    size_t x = 0;
    for (; x + 8 <= count; x += 8) {
        store(dst + x, shuffle(load(src + x), swap));
    }
    for (; x < count; x++) {
        uint32_t v = src[x];
        dst[x] = (v & 0xff00ff00) | (v & 0xff) << 16 | ((v >> 16) & 0xff);
    }
}

static inline void to_rgb16(uint16_t *dst, const uint32_t *src, size_t count, bool six) {
    size_t x = 0;
    for (; x + 8 <= count; x += 8) {
        u32x8 v = (u32x8)load(src + x);
        u32x8 r = (v >> 19) & 0x1f;
        u32x8 b = (v >> 3) & 0x1f;
        u32x8 packed = six
                     ? r << 11 | ((v >> 10) & 0x3f) << 5 | b
                     : r << 10 | ((v >> 11) & 0x1f) << 5 | b;
        u16x8 narrow = __builtin_convertvector(packed, u16x8);
        memcpy(dst + x, &narrow, sizeof(u16x8));
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

static inline void transpose8(
    uint32_t *dst,
    ptrdiff_t dst_pitch,
    const uint32_t *src,
    ptrdiff_t src_pitch
) {
    u32x8 r[8];
    for (size_t i = 0; i < 8; i++) {
        r[i] = (u32x8)load(src + (ptrdiff_t)i * src_pitch);
    }
    u64x4 t[8];
    for (size_t i = 0; i < 8; i += 2) {
        t[i] = (u64x4)__builtin_shufflevector(r[i], r[i + 1], 0, 8, 1, 9, 4, 12, 5, 13);
        t[i + 1] = (u64x4)__builtin_shufflevector(r[i], r[i + 1], 2, 10, 3, 11, 6, 14, 7, 15);
    }
    u64x4 u[8];
    for (size_t i = 0; i < 8; i += 4) {
        u[i] = __builtin_shufflevector(t[i], t[i + 2], 0, 4, 2, 6);
        u[i + 1] = __builtin_shufflevector(t[i], t[i + 2], 1, 5, 3, 7);
        u[i + 2] = __builtin_shufflevector(t[i + 1], t[i + 3], 0, 4, 2, 6);
        u[i + 3] = __builtin_shufflevector(t[i + 1], t[i + 3], 1, 5, 3, 7);
    }
    for (size_t i = 0; i < 4; i++) {
        u64x4 low = __builtin_shufflevector(u[i], u[i + 4], 0, 1, 4, 5);
        u64x4 high = __builtin_shufflevector(u[i], u[i + 4], 2, 3, 6, 7);
        store(dst + (ptrdiff_t)i * dst_pitch, (u8x32)low);
        store(dst + (ptrdiff_t)(i + 4) * dst_pitch, (u8x32)high);
    }
}

static void transpose(
    uint32_t *dst,
    ptrdiff_t dst_pitch,
    const uint32_t *src,
    ptrdiff_t src_pitch,
    size_t rows,
    size_t columns
) {
    size_t r = 0;
    for (; r + 8 <= rows; r += 8) {
        size_t c = 0;
        for (; c + 8 <= columns; c += 8) {
            transpose8(
                dst + (ptrdiff_t)c * dst_pitch + (ptrdiff_t)r, dst_pitch,
                src + (ptrdiff_t)r * src_pitch + (ptrdiff_t)c, src_pitch
            );
        }
        for (; c < columns; c++) {
            for (size_t i = r; i < r + 8; i++) {
                ptrdiff_t to = (ptrdiff_t)c * dst_pitch + (ptrdiff_t)i;
                dst[to] = src[(ptrdiff_t)i * src_pitch + (ptrdiff_t)c];
            }
        }
    }
    for (; r < rows; r++) {
        for (size_t c = 0; c < columns; c++) {
            ptrdiff_t to = (ptrdiff_t)c * dst_pitch + (ptrdiff_t)r;
            dst[to] = src[(ptrdiff_t)r * src_pitch + (ptrdiff_t)c];
        }
    }
}

static void dots(uint32_t *dst, uint8_t glyph, uint32_t fg, uint32_t bg) {
    u32x8 g = { 0 };
    g += glyph;
    u32x8 bits = { 0x80, 0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x01 };
    u32x8 on = (u32x8)((g & bits) == bits);
    u32x8 fgs = { 0 };
    u32x8 bgs = { 0 };
    fgs += fg;
    bgs += bg;
    store(dst, (u8x32)((fgs & on) | (bgs & ~on)));
}

static void text_line(
    uint32_t *line,
    const struct vga_draw_frame *f,
    const struct vga_draw_scan *s
) {
    vga_draw_text_line_with(line, f, s, dots);
}

const struct vga_draw_ops vga_draw_avx2 = {
    .differ = differ,
    .convert8 = convert8,
    .convert15 = convert15,
    .convert16 = convert16,
    .convert24 = convert24,
    .convert32 = convert32,
    .scale = scale,
    .blend = blend,
    .to_xbgr = to_xbgr,
    .to_rgb565 = to_rgb565,
    .to_rgb555 = to_rgb555,
    .transpose = transpose,
    .text_line = text_line
};
