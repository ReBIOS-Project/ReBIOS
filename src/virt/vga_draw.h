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

#ifndef VIRT__VGA_DRAW_H
#define VIRT__VGA_DRAW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct cpu;

// Bilinear filtering's weights, in steps of 1 / VGA_DRAW_FILTER_ONE.
#define VGA_DRAW_FILTER_SHIFT 8
#define VGA_DRAW_FILTER_ONE ((uint32_t)1 << VGA_DRAW_FILTER_SHIFT)

// Display columns come in groups of VGA_DRAW_GROUP, whose source pixels a byte
// shuffle picks out of a window of as many.
#define VGA_DRAW_GROUP 4

// A line of the display scaled across from a source line, of up to columns
// display columns.
struct vga_draw_scale {
    size_t columns;
    // Filtering across: xmap[x] is the source pixel at or before column x, and
    // xweight[x] the weight of the one after; else xmap[x] is the source pixel
    // nearest it.
    bool filter;
    uint32_t *xmap;
    uint8_t *xweight;
    // xweight as 4 16-bit lanes for each column, one for each byte of a pixel.
    uint16_t *weights;
    // For each group of columns: the window's first source pixel, the
    // shuffle's control bytes, which pick each column's pixel out of the
    // window, and whether the window holds them all, those after them included
    // where filtering, within the source line.
    uint32_t *window;
    uint8_t *picks;
    bool *fits;
};

// What a VGA scan line is made of: the planes, and the registers and colours
// of the frame being made.
struct vga_draw_frame {
    const uint8_t *planes[4];
    // The CRT controller's addressing: doubleword, byte, or word mode, which
    // wraps at bit 15 or 13, and whether bits 13 and 14 come from the row scan
    // counter instead.
    bool dword;
    bool byte;
    bool wrap15;
    bool cms;
    bool srs;
    // Text modes: the cursor's address and scan lines, the underline's scan
    // line, whether attribute bit 7 blinks, whether line graphics characters
    // continue into the ninth dot, and the character map select register.
    uint32_t cursor;
    unsigned int cursor_start;
    unsigned int cursor_end;
    bool cursor_shown;
    unsigned int underline;
    bool blink;
    bool line_graphics;
    uint8_t char_map;
    // Graphics modes: the graphics controller's mode register and the planes
    // the attribute controller enables.
    uint8_t gc_mode;
    uint8_t plane_enable;
    // Each byte of a plane as its 8 dots' bits, the leftmost's in the lowest
    // byte.
    const uint64_t *spread;
    // The DAC's colours through the PEL mask, and the attribute controller's
    // palette's through them, as 0RGB.
    uint32_t palette[256];
    uint32_t ac_palette[16];
};

// One scan line of a VGA frame: width pixels from columns character clocks
// fetched from ma on, in row_scan, of dots dots each, every dot repeat times.
struct vga_draw_scan {
    uint32_t width;
    uint32_t columns;
    uint32_t ma;
    unsigned int row_scan;
    unsigned int dots;
    unsigned int repeat;
    bool cursor_on;
    bool blink_on;
};

// The kernels of the emulated VGA's drawing, which SIMD speeds up. Pixels are
// 0RGB, as 32-bit values: blue in the low byte, nothing in the high one.
struct vga_draw_ops {
    // Where size bytes of a and b differ, from *first to *end, both size where
    // nowhere.
    void (*differ)(const void *a, const void *b, size_t size, size_t *first, size_t *end);
    void (*copy)(void *dst, const void *src, size_t size);
    // The same to the framebuffer's write-combining memory.
    void (*copy_fb)(void *dst, const void *src, size_t size);
    void (*fill)(uint32_t *dst, uint32_t value, size_t count);
    // Pixels of the Bochs interface's video memory, of 8 bits through a
    // palette, of 15, 16, or 24 bits, or of 32 bits with whatever their high
    // byte holds.
    void (*convert8)(uint32_t *dst, const uint8_t *src, size_t count, const uint32_t *palette);
    void (*convert15)(uint32_t *dst, const uint8_t *src, size_t count);
    void (*convert16)(uint32_t *dst, const uint8_t *src, size_t count);
    void (*convert24)(uint32_t *dst, const uint8_t *src, size_t count);
    void (*convert32)(uint32_t *dst, const uint8_t *src, size_t count);
    // Display columns x0 to x1 of a line, from source line src.
    void (*scale)(
        uint32_t *dst,
        const uint32_t *src,
        const struct vga_draw_scale *scale,
        size_t x0,
        size_t x1
    );
    // a and b mixed, weight of VGA_DRAW_FILTER_ONE of b to the rest of a.
    void (*blend)(
        uint32_t *dst,
        const uint32_t *a,
        const uint32_t *b,
        uint32_t weight,
        size_t count
    );
    // Pixels in the framebuffer's other formats: red in the low byte, 5:6:5
    // and 5:5:5 bits, and 3 bytes with blue first.
    void (*to_xbgr)(uint32_t *dst, const uint32_t *src, size_t count);
    void (*to_rgb565)(uint16_t *dst, const uint32_t *src, size_t count);
    void (*to_rgb555)(uint16_t *dst, const uint32_t *src, size_t count);
    void (*to_bgr24)(uint8_t *dst, const uint32_t *src, size_t count);
    // dst[c * dst_pitch + r] = src[r * src_pitch + c], pitches in pixels.
    void (*transpose)(
        uint32_t *dst,
        ptrdiff_t dst_pitch,
        const uint32_t *src,
        ptrdiff_t src_pitch,
        size_t rows,
        size_t columns
    );
    // A VGA scan line of a text or a graphics mode.
    void (*text_line)(
        uint32_t *line,
        const struct vga_draw_frame *f,
        const struct vga_draw_scan *s
    );
    void (*graphics_line)(
        uint32_t *line,
        const struct vga_draw_frame *f,
        const struct vga_draw_scan *s
    );
};

// The kernels that each SIMD level (sys/simd.h) does better, in the file named
// after the level, NULL where the level below's serve as well.
extern const struct vga_draw_ops vga_draw_sse2;
extern const struct vga_draw_ops vga_draw_ssse3;
extern const struct vga_draw_ops vga_draw_sse4_1;
extern const struct vga_draw_ops vga_draw_avx;
extern const struct vga_draw_ops vga_draw_avx2;

// Where display pixel i of n falls among count source pixels, centre on
// centre: the source pixel at or before it, and the weight, of
// VGA_DRAW_FILTER_ONE, of the one after.
void vga_draw_filter_position(
    size_t i,
    size_t n,
    size_t count,
    uint32_t *index,
    uint32_t *weight
);
// The arrays of a scale for up to columns display columns.
void vga_draw_scale_alloc(struct vga_draw_scale *scale, size_t columns);
// The tables of scale for a source line of width pixels shown on columns
// display columns, which its arrays have room for.
void vga_draw_scale_set(struct vga_draw_scale *scale, size_t width, size_t columns, bool filter);

// On the boot processor, after simd_cpu_init(): picks the fastest build of
// each kernel on it, timing the copies to the framebuffer with rows rows of
// the framebuffer at fb, pitch bytes apart, which it writes back as they were.
void vga_draw_init(uint8_t *fb, size_t pitch, size_t rows);
// The same on each other processor as it starts, which no other processor's
// timing disturbs.
void vga_draw_cpu_init(void);
// This processor's picks, used between simd_begin(vga_draw_level()) and
// simd_end().
const struct vga_draw_ops *vga_draw_ops(void);
size_t vga_draw_level(void);
// The processor whose copies to the framebuffer were fastest.
struct cpu *vga_draw_copier(void);

// What the kernels share whatever their level.

// 0RGB pixels a and b mixed, weight of VGA_DRAW_FILTER_ONE of b to the rest of
// a. Red and blue share a multiplication, each in 16 bits of it.
static inline uint32_t vga_draw_blend_one(uint32_t a, uint32_t b, uint32_t weight) {
    uint32_t keep = VGA_DRAW_FILTER_ONE - weight;
    uint32_t rb = ((a & 0xff00ff) * keep + (b & 0xff00ff) * weight) >> VGA_DRAW_FILTER_SHIFT;
    uint32_t g = ((a & 0xff00) * keep + (b & 0xff00) * weight) >> VGA_DRAW_FILTER_SHIFT;
    return (rb & 0xff00ff) | (g & 0xff00);
}

static inline uint32_t vga_draw_scale_one(
    const uint32_t *src,
    const struct vga_draw_scale *scale,
    size_t x
) {
    uint32_t i = scale->xmap[x];
    if (scale->filter && scale->xweight[x] != 0) {
        return vga_draw_blend_one(src[i], src[i + 1], scale->xweight[x]);
    }
    return src[i];
}

// A 16-bit pixel as 0RGB, of 5 bits each, or of 5, 6, and 5 bits, each
// channel's high bits repeated below it.
static inline uint32_t vga_draw_expand16(uint32_t v, bool six) {
    uint32_t r = (v >> (six ? 11 : 10)) & 0x1f;
    uint32_t g = (v >> 5) & (six ? 0x3f : 0x1f);
    uint32_t b = v & 0x1f;
    r = (r << 3) | (r >> 2);
    g = six ? (g << 2) | (g >> 4) : (g << 3) | (g >> 2);
    b = (b << 3) | (b >> 2);
    return r << 16 | g << 8 | b;
}

// A 0RGB pixel in 5:6:5 or 5:5:5 bits.
static inline uint16_t vga_draw_pack16(uint32_t v, bool six) {
    uint32_t r = (v >> 19) & 0x1f;
    uint32_t b = (v >> 3) & 0x1f;
    if (six) {
        return (uint16_t)(r << 11 | ((v >> 10) & 0x3f) << 5 | b);
    }
    return (uint16_t)(r << 10 | ((v >> 11) & 0x1f) << 5 | b);
}

// Where the CRT controller fetches a character clock's bytes from.
static inline uint32_t vga_draw_crtc_address(
    const struct vga_draw_frame *f,
    uint32_t ma,
    unsigned int row_scan
) {
    uint32_t a;
    if (f->dword) {
        a = (ma << 2) | ((ma >> 14) & 3);
    } else if (!f->byte) {
        uint32_t bit = f->wrap15 ? (ma >> 15) & 1 : (ma >> 13) & 1;
        a = (ma << 1) | bit;
    } else {
        a = ma;
    }
    if (f->cms) {
        a = (a & ~(uint32_t)0x2000) | ((row_scan & (uint32_t)1) << 13);
    }
    if (f->srs) {
        a = (a & ~(uint32_t)0x4000) | ((row_scan & (uint32_t)2) << 13);
    }
    return a & 0xffff;
}

// The character map an attribute selects, map A where its bit 3 is set.
static inline uint32_t vga_draw_font_base(const struct vga_draw_frame *f, uint8_t attribute) {
    uint8_t select = f->char_map;
    unsigned int map;
    if ((attribute & 0x08) != 0) {
        map = ((select >> 3) & 4) | ((select >> 2) & 3);
    } else {
        map = ((select >> 2) & 4) | (select & 3);
    }
    return (map & 3) * (uint32_t)0x4000 + (map >> 2) * (uint32_t)0x2000;
}

static inline void vga_draw_put(
    uint32_t *line,
    uint32_t *x,
    uint32_t width,
    uint32_t rgb,
    size_t count
) {
    for (size_t i = 0; i < count && *x < width; i++) {
        line[(*x)++] = rgb;
    }
}

// A text mode's scan line, with dots(), which draws a glyph's row of 8 dots,
// its leftmost the highest bit, fg where set and bg where clear.
static inline void vga_draw_text_line_with(
    uint32_t *line,
    const struct vga_draw_frame *f,
    const struct vga_draw_scan *s,
    void (*dots)(uint32_t *dst, uint8_t glyph, uint32_t fg, uint32_t bg)
) {
    bool cursor_row = f->cursor_shown && s->cursor_on && s->row_scan >= f->cursor_start
                   && s->row_scan <= f->cursor_end;
    bool underline_row = s->row_scan == f->underline;
    uint32_t x = 0;
    for (size_t c = 0; c < s->columns; c++) {
        uint32_t a = vga_draw_crtc_address(f, s->ma + (uint32_t)c, s->row_scan);
        uint8_t ch = f->planes[0][a];
        uint8_t attribute = f->planes[1][a];
        uint8_t glyph = f->planes[2][
            (vga_draw_font_base(f, attribute) + ch * (uint32_t)32 + s->row_scan) & 0xffff
        ];
        uint8_t fg = attribute & 0x0f;
        uint8_t bg = attribute >> 4;
        // The underline of a monochrome display's attributes, which colour
        // modes keep below the character cell, and the cursor span the whole
        // cell, ninth dot included.
        bool whole = false;
        if (underline_row && (attribute & 0x77) == 0x01) {
            glyph = 0xff;
            whole = true;
        }
        if (f->blink) {
            bg &= 7;
            if ((attribute & 0x80) != 0 && !s->blink_on) {
                glyph = 0;
                whole = false;
            }
        }
        if (cursor_row && s->ma + c == f->cursor) {
            glyph = 0xff;
            whole = true;
        }
        uint32_t fg_rgb = f->ac_palette[fg];
        uint32_t bg_rgb = f->ac_palette[bg];
        // Line graphics characters continue into the ninth column.
        bool ninth = whole || (f->line_graphics && ch >= 0xc0 && ch <= 0xdf && (glyph & 1) != 0);
        if (s->repeat == 1 && x + s->dots <= s->width) {
            dots(line + x, glyph, fg_rgb, bg_rgb);
            if (s->dots == 9) {
                line[x + 8] = ninth ? fg_rgb : bg_rgb;
            }
            x += s->dots;
            continue;
        }
        for (size_t k = 0; k < s->dots; k++) {
            bool on = k < 8 ? ((glyph >> (7 - k)) & 1) != 0 : ninth;
            vga_draw_put(line, &x, s->width, on ? fg_rgb : bg_rgb, s->repeat);
        }
    }
    while (x < s->width) {
        line[x++] = 0;
    }
}

// The 8 dots of a 16-colour mode's character clock, 4-bit colours one a byte,
// leftmost lowest, from the planes' bytes at address a.
static inline uint64_t vga_draw_colours(const struct vga_draw_frame *f, uint32_t a) {
    uint64_t enables = (f->plane_enable & 0x0f) * (uint64_t)0x0101010101010101;
    uint64_t colours = f->spread[f->planes[0][a]] | f->spread[f->planes[1][a]] << 1
                     | f->spread[f->planes[2][a]] << 2 | f->spread[f->planes[3][a]] << 3;
    return colours & enables;
}

// A graphics mode's scan line, with palette16(), where not NULL, which draws
// the 16 dots of two character clocks of a 16-colour mode, from their colours,
// through the attribute controller's palette.
static inline void vga_draw_graphics_line_with(
    uint32_t *line,
    const struct vga_draw_frame *f,
    const struct vga_draw_scan *s,
    void (*palette16)(uint32_t *dst, uint64_t first, uint64_t second, const uint32_t *palette)
) {
    uint8_t enable = f->plane_enable & 0x0f;
    uint32_t x = 0;
    size_t c = 0;
    if (palette16 != NULL && (f->gc_mode & 0x60) == 0 && s->repeat == 1) {
        for (; c + 2 <= s->columns && x + 16 <= s->width; c += 2) {
            uint32_t first = vga_draw_crtc_address(f, s->ma + (uint32_t)c, s->row_scan);
            uint32_t second = vga_draw_crtc_address(f, s->ma + (uint32_t)c + 1, s->row_scan);
            palette16(
                line + x, vga_draw_colours(f, first), vga_draw_colours(f, second), f->ac_palette
            );
            x += 16;
        }
    }
    for (; c < s->columns; c++) {
        uint32_t a = vga_draw_crtc_address(f, s->ma + (uint32_t)c, s->row_scan);
        // Each character clock is 8 dots, of the 256-colour mode's pixels
        // twice.
        bool room = s->repeat == 1 && x + 8 <= s->width;
        if ((f->gc_mode & 0x40) != 0) {
            for (size_t k = 0; k < 4; k++) {
                uint32_t rgb = f->palette[f->planes[k][a]];
                if (room) {
                    line[x] = rgb;
                    line[x + 1] = rgb;
                    x += 2;
                } else {
                    vga_draw_put(line, &x, s->width, rgb, 2 * s->repeat);
                }
            }
        } else if ((f->gc_mode & 0x20) != 0) {
            for (size_t k = 0; k < 8; k++) {
                unsigned int shift = 6 - 2 * (k & 3);
                uint8_t low = f->planes[k < 4 ? 0 : 1][a];
                uint8_t high = f->planes[k < 4 ? 2 : 3][a];
                uint8_t colour = (uint8_t)(((low >> shift) & 3) | (((high >> shift) & 3) << 2));
                vga_draw_put(line, &x, s->width, f->ac_palette[colour & enable], s->repeat);
            }
        } else {
            uint64_t colours = vga_draw_colours(f, a);
            for (size_t k = 0; k < 8; k++) {
                uint32_t rgb = f->ac_palette[(colours >> (8 * k)) & 0x0f];
                if (room) {
                    line[x++] = rgb;
                } else {
                    vga_draw_put(line, &x, s->width, rgb, s->repeat);
                }
            }
        }
    }
    while (x < s->width) {
        line[x++] = 0;
    }
}

#endif
