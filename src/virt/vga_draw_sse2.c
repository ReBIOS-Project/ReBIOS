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

// The drawing kernels with SSE2, which every x86-64 processor has, and which
// the other levels' fall back on. Built with SSE2 enabled.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <memory.h>
#include <virt/vga_draw.h>

typedef char c8x16 __attribute__((vector_size(16)));
typedef uint8_t u8x16 __attribute__((vector_size(16)));
typedef uint16_t u16x8 __attribute__((vector_size(16)));
typedef int16_t i16x8 __attribute__((vector_size(16)));
typedef uint32_t u32x4 __attribute__((vector_size(16)));
typedef int32_t i32x4 __attribute__((vector_size(16)));

static inline u8x16 load(const void *p) {
    u8x16 v;
    memcpy(&v, p, sizeof(u8x16));
    return v;
}

static inline void store(void *p, u8x16 v) {
    memcpy(p, &v, sizeof(u8x16));
}

// A store past the caches, to a 16-byte aligned address.
static inline void stream(void *p, u8x16 v) {
    asm volatile ("movntdq %1, %0" : "=m"(*(u8x16 *)p) : "x"(v));
}

// A bit for each byte of a and b, set where they match.
static inline uint32_t match_mask(u8x16 a, u8x16 b) {
    return (uint32_t)__builtin_ia32_pmovmskb128((c8x16)(a == b));
}

static inline bool match64(const uint8_t *a, const uint8_t *b) {
    u8x16 d = (load(a) ^ load(b)) | (load(a + 16) ^ load(b + 16)) | (load(a + 32) ^ load(b + 32))
            | (load(a + 48) ^ load(b + 48));
    return match_mask(d, (u8x16){ 0 }) == 0xffff;
}

static void differ(const void *a, const void *b, size_t size, size_t *first, size_t *end) {
    const uint8_t *p = a;
    const uint8_t *q = b;
    // Forwards to the first byte that differs: 64 bytes at a time while they
    // match, as most do, then 16, then bytes.
    size_t i = 0;
    while (i + 64 <= size && match64(p + i, q + i)) {
        i += 64;
    }
    uint32_t differs = 0;
    for (; i + 16 <= size; i += 16) {
        differs = ~match_mask(load(p + i), load(q + i)) & 0xffff;
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

    // Backwards to the last one the same way, down to it at most.
    size_t j = size;
    while (j >= i + 64 && match64(p + j - 64, q + j - 64)) {
        j -= 64;
    }
    for (; j >= i + 16; j -= 16) {
        differs = ~match_mask(load(p + j - 16), load(q + j - 16)) & 0xffff;
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

// Fewer bytes than a vector holds, 4 at a time where they can.
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

// The first and the last 16 bytes unaligned, those between aligned on the
// destination, past the caches where streamed.
static inline void copy_vectors(uint8_t *d, const uint8_t *s, size_t size, bool streamed) {
    u8x16 head = load(s);
    u8x16 tail = load(s + size - 16);
    size_t i = 16 - ((uintptr_t)d & 15);
    for (; i + 64 <= size; i += 64) {
        u8x16 v0 = load(s + i);
        u8x16 v1 = load(s + i + 16);
        u8x16 v2 = load(s + i + 32);
        u8x16 v3 = load(s + i + 48);
        if (streamed) {
            stream(d + i, v0);
            stream(d + i + 16, v1);
            stream(d + i + 32, v2);
            stream(d + i + 48, v3);
        } else {
            store(d + i, v0);
            store(d + i + 16, v1);
            store(d + i + 32, v2);
            store(d + i + 48, v3);
        }
    }
    for (; i + 16 <= size; i += 16) {
        if (streamed) {
            stream(d + i, load(s + i));
        } else {
            store(d + i, load(s + i));
        }
    }
    store(d, head);
    store(d + size - 16, tail);
}

static void copy(void *dst, const void *src, size_t size) {
    if (size < 16) {
        copy_small(dst, src, size);
        return;
    }
    copy_vectors(dst, src, size, false);
}

// The fence has the write-combining buffers on their way before the copy ends.
static void copy_fb(void *dst, const void *src, size_t size) {
    if (size < 16) {
        copy_small(dst, src, size);
        return;
    }
    copy_vectors(dst, src, size, true);
    asm volatile ("sfence" ::: "memory");
}

static void fill(uint32_t *dst, uint32_t value, size_t count) {
    size_t x = 0;
    u32x4 v = { value, value, value, value };
    for (; x + 4 <= count; x += 4) {
        store(dst + x, (u8x16)v);
    }
    for (; x < count; x++) {
        dst[x] = value;
    }
}

static void convert8(uint32_t *dst, const uint8_t *src, size_t count, const uint32_t *palette) {
    for (size_t x = 0; x < count; x++) {
        dst[x] = palette[src[x]];
    }
}

// 4 16-bit pixels, the low half of v, in 32-bit lanes as 0RGB.
static inline u32x4 expand4(u16x8 v, bool six) {
    u32x4 p = (u32x4)__builtin_shufflevector(v, (u16x8){ 0 }, 0, 8, 1, 9, 2, 10, 3, 11);
    u32x4 r = (p >> (six ? 11 : 10)) & 0x1f;
    u32x4 g = (p >> 5) & (six ? 0x3f : 0x1f);
    u32x4 b = p & 0x1f;
    r = (r << 3) | (r >> 2);
    g = six ? (g << 2) | (g >> 4) : (g << 3) | (g >> 2);
    b = (b << 3) | (b >> 2);
    return r << 16 | g << 8 | b;
}

static inline void convert16_any(uint32_t *dst, const uint8_t *src, size_t count, bool six) {
    size_t x = 0;
    for (; x + 8 <= count; x += 8) {
        u16x8 v = (u16x8)load(src + 2 * x);
        store(dst + x, (u8x16)expand4(v, six));
        u16x8 high = __builtin_shufflevector(v, v, 4, 5, 6, 7, 4, 5, 6, 7);
        store(dst + x + 4, (u8x16)expand4(high, six));
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
    for (size_t x = 0; x < count; x++) {
        const uint8_t *q = src + 3 * x;
        dst[x] = (uint32_t)q[2] << 16 | (uint32_t)q[1] << 8 | q[0];
    }
}

static void convert32(uint32_t *dst, const uint8_t *src, size_t count) {
    size_t x = 0;
    for (; x + 4 <= count; x += 4) {
        store(dst + x, (u8x16)((u32x4)load(src + 4 * x) & 0x00ffffff));
    }
    for (; x < count; x++) {
        const uint8_t *q = src + 4 * x;
        dst[x] = (uint32_t)q[2] << 16 | (uint32_t)q[1] << 8 | q[0];
    }
}

static void scale(
    uint32_t *dst,
    const uint32_t *src,
    const struct vga_draw_scale *scale,
    size_t x0,
    size_t x1
) {
    for (size_t x = x0; x < x1; x++) {
        dst[x] = vga_draw_scale_one(src, scale, x);
    }
}

// The 4 pixels of a and b mixed, weights 16-bit lanes of b's share for each
// byte of the low two pixels and of the high two. Each byte's a * (ONE -
// weight) + b * weight fits 16 bits.
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

static void blend(
    uint32_t *dst,
    const uint32_t *a,
    const uint32_t *b,
    uint32_t weight,
    size_t count
) {
    size_t x = 0;
    u16x8 w = { 0 };
    w += (uint16_t)weight;
    for (; x + 4 <= count; x += 4) {
        store(dst + x, mix(load(a + x), load(b + x), w, w));
    }
    for (; x < count; x++) {
        dst[x] = vga_draw_blend_one(a[x], b[x], weight);
    }
}

static void to_xbgr(uint32_t *dst, const uint32_t *src, size_t count) {
    size_t x = 0;
    for (; x + 4 <= count; x += 4) {
        u32x4 v = (u32x4)load(src + x);
        store(dst + x, (u8x16)((v & 0xff00ff00) | (v & 0xff) << 16 | ((v >> 16) & 0xff)));
    }
    for (; x < count; x++) {
        uint32_t v = src[x];
        dst[x] = (v & 0xff00ff00) | (v & 0xff) << 16 | ((v >> 16) & 0xff);
    }
}

// 4 0RGB pixels in 5:6:5 or 5:5:5 bits, in 32-bit lanes.
static inline u32x4 pack4(u32x4 v, bool six) {
    u32x4 r = (v >> 19) & 0x1f;
    u32x4 b = (v >> 3) & 0x1f;
    if (six) {
        return r << 11 | ((v >> 10) & 0x3f) << 5 | b;
    }
    return r << 10 | ((v >> 11) & 0x1f) << 5 | b;
}

// 32-bit lanes holding 16 bits each, as 16-bit lanes: by the signed saturating
// pack, which needs them biased down into its range.
static inline u8x16 narrow(u32x4 low, u32x4 high) {
    i32x4 bias = { 0x8000, 0x8000, 0x8000, 0x8000 };
    i16x8 packed = __builtin_ia32_packssdw128((i32x4)low - bias, (i32x4)high - bias);
    return (u8x16)((u16x8)packed + (uint16_t)0x8000);
}

static inline void to_rgb16(uint16_t *dst, const uint32_t *src, size_t count, bool six) {
    size_t x = 0;
    for (; x + 8 <= count; x += 8) {
        u32x4 low = pack4((u32x4)load(src + x), six);
        u32x4 high = pack4((u32x4)load(src + x + 4), six);
        store(dst + x, narrow(low, high));
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

static void to_bgr24(uint8_t *dst, const uint32_t *src, size_t count) {
    for (size_t x = 0; x < count; x++) {
        uint32_t v = src[x];
        dst[3 * x] = (uint8_t)v;
        dst[3 * x + 1] = (uint8_t)(v >> 8);
        dst[3 * x + 2] = (uint8_t)(v >> 16);
    }
}

static inline void transpose4(
    uint32_t *dst,
    ptrdiff_t dst_pitch,
    const uint32_t *src,
    ptrdiff_t src_pitch
) {
    u32x4 r0 = (u32x4)load(src);
    u32x4 r1 = (u32x4)load(src + src_pitch);
    u32x4 r2 = (u32x4)load(src + 2 * src_pitch);
    u32x4 r3 = (u32x4)load(src + 3 * src_pitch);
    u32x4 t0 = __builtin_shufflevector(r0, r1, 0, 4, 1, 5);
    u32x4 t1 = __builtin_shufflevector(r2, r3, 0, 4, 1, 5);
    u32x4 t2 = __builtin_shufflevector(r0, r1, 2, 6, 3, 7);
    u32x4 t3 = __builtin_shufflevector(r2, r3, 2, 6, 3, 7);
    store(dst, (u8x16)__builtin_shufflevector(t0, t1, 0, 1, 4, 5));
    store(dst + dst_pitch, (u8x16)__builtin_shufflevector(t0, t1, 2, 3, 6, 7));
    store(dst + 2 * dst_pitch, (u8x16)__builtin_shufflevector(t2, t3, 0, 1, 4, 5));
    store(dst + 3 * dst_pitch, (u8x16)__builtin_shufflevector(t2, t3, 2, 3, 6, 7));
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
    for (; r + 4 <= rows; r += 4) {
        size_t c = 0;
        for (; c + 4 <= columns; c += 4) {
            transpose4(
                dst + (ptrdiff_t)c * dst_pitch + (ptrdiff_t)r, dst_pitch,
                src + (ptrdiff_t)r * src_pitch + (ptrdiff_t)c, src_pitch
            );
        }
        for (; c < columns; c++) {
            for (size_t i = r; i < r + 4; i++) {
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
    u32x4 g = { glyph, glyph, glyph, glyph };
    u32x4 high_bits = { 0x80, 0x40, 0x20, 0x10 };
    u32x4 low_bits = { 0x08, 0x04, 0x02, 0x01 };
    u32x4 fgs = { fg, fg, fg, fg };
    u32x4 bgs = { bg, bg, bg, bg };
    u32x4 high = (u32x4)((g & high_bits) == high_bits);
    u32x4 low = (u32x4)((g & low_bits) == low_bits);
    store(dst, (u8x16)((fgs & high) | (bgs & ~high)));
    store(dst + 4, (u8x16)((fgs & low) | (bgs & ~low)));
}

static void text_line(
    uint32_t *line,
    const struct vga_draw_frame *f,
    const struct vga_draw_scan *s
) {
    vga_draw_text_line_with(line, f, s, dots);
}

static void graphics_line(
    uint32_t *line,
    const struct vga_draw_frame *f,
    const struct vga_draw_scan *s
) {
    vga_draw_graphics_line_with(line, f, s, NULL);
}

const struct vga_draw_ops vga_draw_sse2 = {
    .differ = differ,
    .copy = copy,
    .copy_fb = copy_fb,
    // With plain loads, the streaming ones coming with SSE4.1.
    .read_fb = copy,
    .fill = fill,
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
    .to_bgr24 = to_bgr24,
    .transpose = transpose,
    .text_line = text_line,
    .graphics_line = graphics_line
};
