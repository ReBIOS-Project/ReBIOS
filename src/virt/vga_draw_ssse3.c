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

// The drawing kernels that SSSE3's byte shuffle does better: picking pixels
// out of a window of source pixels, and the bytes of pixels out of others'.
// Built with SSSE3 enabled.

#include <stddef.h>
#include <stdint.h>
#include <memory.h>
#include <virt/vga_draw.h>

typedef char c8x16 __attribute__((vector_size(16)));
typedef uint8_t u8x16 __attribute__((vector_size(16)));
typedef uint16_t u16x8 __attribute__((vector_size(16)));
typedef int16_t i16x8 __attribute__((vector_size(16)));
typedef uint32_t u32x4 __attribute__((vector_size(16)));

static inline u8x16 load(const void *p) {
    u8x16 v;
    memcpy(&v, p, sizeof(u8x16));
    return v;
}

static inline void store(void *p, u8x16 v) {
    memcpy(p, &v, sizeof(u8x16));
}

// Bytes of table at the positions control gives, 0 where its bit 7 is set.
static inline u8x16 shuffle(u8x16 table, u8x16 control) {
    return (u8x16)__builtin_ia32_pshufb128((c8x16)table, (c8x16)control);
}

static inline u8x16 mix(u8x16 a, u8x16 b, u16x8 weights_low, u16x8 weights_high) {
    u8x16 z = { 0 };
    u16x8 a_low = (u16x8)__builtin_shufflevector(
        a, z, 0, 16, 1, 16, 2, 16, 3, 16, 4, 16, 5, 16, 6, 16, 7, 16
    );
    u16x8 a_high = (u16x8)__builtin_shufflevector(
        a, z, 8, 16, 9, 16, 10, 16, 11, 16, 12, 16, 13, 16, 14, 16, 15, 16
    );
    u16x8 b_low = (u16x8)__builtin_shufflevector(
        b, z, 0, 16, 1, 16, 2, 16, 3, 16, 4, 16, 5, 16, 6, 16, 7, 16
    );
    u16x8 b_high = (u16x8)__builtin_shufflevector(
        b, z, 8, 16, 9, 16, 10, 16, 11, 16, 12, 16, 13, 16, 14, 16, 15, 16
    );
    u16x8 low = (a_low * (VGA_DRAW_FILTER_ONE - weights_low) + b_low * weights_low)
             >> VGA_DRAW_FILTER_SHIFT;
    u16x8 high = (a_high * (VGA_DRAW_FILTER_ONE - weights_high) + b_high * weights_high)
              >> VGA_DRAW_FILTER_SHIFT;
    return (u8x16)__builtin_ia32_packuswb128((i16x8)low, (i16x8)high);
}

// The pixels of a group of columns out of its window, mixed with those after
// them where filtering.
static inline u8x16 scale_group(
    const uint32_t *src,
    const struct vga_draw_scale *scale,
    size_t x
) {
    size_t g = x / VGA_DRAW_GROUP;
    u8x16 picks = load(scale->picks + 16 * g);
    const uint32_t *window = src + scale->window[g];
    u8x16 a = shuffle(load(window), picks);
    if (!scale->filter) {
        return a;
    }
    u8x16 b = shuffle(load(window + 1), picks);
    u16x8 low = (u16x8)load(scale->weights + 4 * x);
    u16x8 high = (u16x8)load(scale->weights + 4 * x + 8);
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
    for (; x + VGA_DRAW_GROUP <= x1; x += VGA_DRAW_GROUP) {
        if (scale->fits[x / VGA_DRAW_GROUP]) {
            store(dst + x, scale_group(src, scale, x));
            continue;
        }
        for (size_t i = 0; i < VGA_DRAW_GROUP; i++) {
            dst[x + i] = vga_draw_scale_one(src, scale, x + i);
        }
    }
    for (; x < x1; x++) {
        dst[x] = vga_draw_scale_one(src, scale, x);
    }
}

static void convert24(uint32_t *dst, const uint8_t *src, size_t count) {
    // 4 pixels from each 12 bytes, of the 16 that a load takes, so the last 2
    // pixels are left for one at a time.
    const u8x16 spread = { 0, 1, 2, 0x80, 3, 4, 5, 0x80, 6, 7, 8, 0x80, 9, 10, 11, 0x80 };
    size_t x = 0;
    for (; x + 4 + 2 <= count; x += 4) {
        store(dst + x, shuffle(load(src + 3 * x), spread));
    }
    for (; x < count; x++) {
        const uint8_t *q = src + 3 * x;
        dst[x] = (uint32_t)q[2] << 16 | (uint32_t)q[1] << 8 | q[0];
    }
}

static void to_xbgr(uint32_t *dst, const uint32_t *src, size_t count) {
    const u8x16 swap = { 2, 1, 0, 3, 6, 5, 4, 7, 10, 9, 8, 11, 14, 13, 12, 15 };
    size_t x = 0;
    for (; x + 4 <= count; x += 4) {
        store(dst + x, shuffle(load(src + x), swap));
    }
    for (; x < count; x++) {
        uint32_t v = src[x];
        dst[x] = (v & 0xff00ff00) | (v & 0xff) << 16 | ((v >> 16) & 0xff);
    }
}

static void to_bgr24(uint8_t *dst, const uint32_t *src, size_t count) {
    // 16 bytes stored for each 12, the next store covering the rest, so the
    // last 2 pixels are left for one at a time.
    const u8x16 pack = { 0, 1, 2, 4, 5, 6, 8, 9, 10, 12, 13, 14, 0x80, 0x80, 0x80, 0x80 };
    size_t x = 0;
    for (; x + 4 + 2 <= count; x += 4) {
        store(dst + 3 * x, shuffle(load(src + x), pack));
    }
    for (; x < count; x++) {
        uint32_t v = src[x];
        dst[3 * x] = (uint8_t)v;
        dst[3 * x + 1] = (uint8_t)(v >> 8);
        dst[3 * x + 2] = (uint8_t)(v >> 16);
    }
}

// 16 dots from their colours, through 16 colours whose blue, green, and red
// bytes three shuffles look up.
static void palette16(uint32_t *dst, uint64_t first, uint64_t second, const uint32_t *palette) {
    u8x16 tables[3];
    for (size_t i = 0; i < 16; i++) {
        tables[0][i] = (uint8_t)palette[i];
        tables[1][i] = (uint8_t)(palette[i] >> 8);
        tables[2][i] = (uint8_t)(palette[i] >> 16);
    }
    u8x16 colours;
    memcpy(&colours, &first, sizeof(uint64_t));
    memcpy((uint8_t *)&colours + sizeof(uint64_t), &second, sizeof(uint64_t));
    u8x16 blue = shuffle(tables[0], colours);
    u8x16 green = shuffle(tables[1], colours);
    u8x16 red = shuffle(tables[2], colours);
    u8x16 z = { 0 };
    u16x8 bg_low = (u16x8)__builtin_shufflevector(
        blue, green, 0, 16, 1, 17, 2, 18, 3, 19, 4, 20, 5, 21, 6, 22, 7, 23
    );
    u16x8 bg_high = (u16x8)__builtin_shufflevector(
        blue, green, 8, 24, 9, 25, 10, 26, 11, 27, 12, 28, 13, 29, 14, 30, 15, 31
    );
    u16x8 r_low = (u16x8)__builtin_shufflevector(
        red, z, 0, 16, 1, 16, 2, 16, 3, 16, 4, 16, 5, 16, 6, 16, 7, 16
    );
    u16x8 r_high = (u16x8)__builtin_shufflevector(
        red, z, 8, 16, 9, 16, 10, 16, 11, 16, 12, 16, 13, 16, 14, 16, 15, 16
    );
    store(dst, (u8x16)__builtin_shufflevector(bg_low, r_low, 0, 8, 1, 9, 2, 10, 3, 11));
    store(dst + 4, (u8x16)__builtin_shufflevector(bg_low, r_low, 4, 12, 5, 13, 6, 14, 7, 15));
    store(dst + 8, (u8x16)__builtin_shufflevector(bg_high, r_high, 0, 8, 1, 9, 2, 10, 3, 11));
    store(dst + 12, (u8x16)__builtin_shufflevector(bg_high, r_high, 4, 12, 5, 13, 6, 14, 7, 15));
}

static void graphics_line(
    uint32_t *line,
    const struct vga_draw_frame *f,
    const struct vga_draw_scan *s
) {
    vga_draw_graphics_line_with(line, f, s, palette16);
}

const struct vga_draw_ops vga_draw_ssse3 = {
    .convert24 = convert24,
    .scale = scale,
    .to_xbgr = to_xbgr,
    .to_bgr24 = to_bgr24,
    .graphics_line = graphics_line
};
