// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of ReBIOS.
// Copyright (C) 2026 Mintsuki and contributors
//
// ReBIOS is free software: you can redistribute it and/or modify it under the
// terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version.
//
// ReBIOS is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
// FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
// details.
//
// You should have received a copy of the GNU General Public License along
// with ReBIOS. If not, see <https://www.gnu.org/licenses/>.

#include <stdint.h>
#include <stdbool.h>
#include <vgabios.h>

#define BDA 0x0040
#define BDA_EQUIPMENT 0x10
#define BDA_MODE 0x49
#define BDA_COLUMNS 0x4a
#define BDA_PAGE_SIZE 0x4c
#define BDA_PAGE_START 0x4e
#define BDA_CURSOR 0x50
#define BDA_CURSOR_TYPE 0x60
#define BDA_PAGE 0x62
#define BDA_CRTC 0x63
#define BDA_MODE_CONTROL 0x65
#define BDA_PALETTE 0x66
#define BDA_ROWS 0x84
#define BDA_CHAR_HEIGHT 0x85
#define BDA_VIDEO_CONTROL 0x87
#define BDA_SWITCHES 0x88
#define BDA_VGA_FLAGS 0x89
#define BDA_DCC 0x8a

#define VIDEO_CONTROL_NO_EMULATION 0x01
#define VIDEO_CONTROL_KEPT 0x80
#define VGA_FLAGS_GREY 0x02
#define VGA_FLAGS_NO_PALETTE 0x08
#define VGA_FLAGS_400 0x10
#define VGA_FLAGS_200 0x80

#define VECTOR(n) ((uint16_t)((n) * 4))

#define AC_INDEX 0x3c0
#define AC_READ 0x3c1
#define MISC_WRITE 0x3c2
#define VGA_ENABLE 0x3c3
#define SEQ_INDEX 0x3c4
#define PEL_MASK 0x3c6
#define DAC_READ_INDEX 0x3c7
#define DAC_WRITE_INDEX 0x3c8
#define DAC_DATA 0x3c9
#define GC_INDEX 0x3ce

#define AC_PAS 0x20
#define SEQ_SCREEN_OFF 0x20

#define DCC_VGA_COLOR 0x08
#define DCC_VGA_MONO 0x07

// The CGA mode control register value each of modes 0-7 has, which the BIOS
// data area records.
static const __seg_fs uint8_t cga_mode_control[8] = {
    0x2c, 0x28, 0x2d, 0x29, 0x2a, 0x2e, 0x1e, 0x29,
};

// What INT 10h AH=1Bh points to: modes 0-7, 0Dh-13h, 200, 350, and 400
// scan lines, 8 character blocks, 2 at once, and the usual functions.
static const __seg_fs uint8_t functionality[16] = {
    0xff, 0xe0, 0x0f, 0x00, 0x00, 0x00, 0x00, 0x07, 0x08, 0x02, 0xff, 0x0e, 0x00, 0x00, 0x00, 0x00,
};

static void seq_write(uint8_t index, uint8_t value) {
    outb(SEQ_INDEX, index);
    outb(SEQ_INDEX + 1, value);
}

static uint8_t seq_read(uint8_t index) {
    outb(SEQ_INDEX, index);
    return inb(SEQ_INDEX + 1);
}

static void gc_write(uint8_t index, uint8_t value) {
    outb(GC_INDEX, index);
    outb(GC_INDEX + 1, value);
}

static uint16_t crtc_port(void) {
    return peekw(BDA, BDA_CRTC);
}

static void crtc_write(uint8_t index, uint8_t value) {
    uint16_t port = crtc_port();
    outb(port, index);
    outb(port + 1, value);
}

static uint8_t crtc_read(uint8_t index) {
    uint16_t port = crtc_port();
    outb(port, index);
    return inb(port + 1);
}

static void ac_flip_flop_reset(void) {
    inb(crtc_port() + 6);
}

// Palette registers take writes only while the display does not read them,
// so it blanks for the write.
static void ac_write(uint8_t index, uint8_t value) {
    ac_flip_flop_reset();
    outb(AC_INDEX, index);
    outb(AC_INDEX, value);
    outb(AC_INDEX, AC_PAS);
}

static uint8_t ac_read(uint8_t index) {
    ac_flip_flop_reset();
    outb(AC_INDEX, index | AC_PAS);
    uint8_t value = inb(AC_READ);
    ac_flip_flop_reset();
    outb(AC_INDEX, AC_PAS);
    return value;
}

// The mode at a line count, or with any if lines is 0 or none has it.
static const __seg_fs struct vgabios_mode *find_mode_lines(uint8_t number, uint16_t lines) {
    const __seg_fs struct vgabios_mode *any = 0;
    for (unsigned i = 0; i < vgabios_mode_count; i++) {
        if (vgabios_modes[i].mode != number) {
            continue;
        }
        if (lines == 0 || vgabios_modes[i].scan_lines == lines) {
            return &vgabios_modes[i];
        }
        if (any == 0) {
            any = &vgabios_modes[i];
        }
    }
    return any;
}

static const __seg_fs struct vgabios_mode *find_mode(uint8_t number) {
    return find_mode_lines(number, 0);
}

// The line count INT 10h AH=12h BL=30h picked for the text modes.
static uint16_t text_lines(void) {
    uint8_t flags = peekb(BDA, BDA_VGA_FLAGS);
    if (flags & VGA_FLAGS_200) {
        return 200;
    }
    return (flags & VGA_FLAGS_400) ? 400 : 350;
}

static const __seg_fs struct vgabios_mode *current_mode(void) {
    return find_mode(peekb(BDA, BDA_MODE) & 0x7f);
}

static bool is_text(const __seg_fs struct vgabios_mode *m) {
    return m->kind == MODE_TEXT;
}

static void set_vector(uint8_t number, uint16_t segment, uint16_t offset) {
    pokew(0, VECTOR(number), offset);
    pokew(0, VECTOR(number) + 2, segment);
}

static const __seg_fs uint8_t *rom_font(uint8_t height) {
    if (height == 8) {
        return vgabios_font8;
    }
    if (height == 14) {
        return vgabios_font14;
    }
    return vgabios_font16;
}

static uint16_t rom_offset(const __seg_fs void *p) {
    return (uint16_t)(uintptr_t)p;
}

static void load_palette(uint8_t index) {
    const __seg_fs struct vgabios_palette *p = &vgabios_palettes[index];
    const __seg_fs uint8_t *colors = p->colors;
    uint16_t bytes = p->count * 3u;
    bool grey = peekb(BDA, BDA_VGA_FLAGS) & VGA_FLAGS_GREY;
    outb(DAC_WRITE_INDEX, 0);
    for (uint16_t i = 0; i < 256u * 3; i += 3) {
        uint8_t r = i < bytes ? colors[i] : 0;
        uint8_t g = i < bytes ? colors[i + 1] : 0;
        uint8_t b = i < bytes ? colors[i + 2] : 0;
        if (grey) {
            // The weighting of the luminance of a colour signal.
            uint8_t y = (uint8_t)((r * 77u + g * 151u + b * 28u + 128u) >> 8);
            r = g = b = y;
        }
        outb(DAC_DATA, r);
        outb(DAC_DATA, g);
        outb(DAC_DATA, b);
    }
}

void vgabios_load_palette_256(void) {
    const __seg_fs struct vgabios_mode *m = find_mode(0x13);
    if (m != 0) {
        load_palette(m->palette);
    }
}

// The offset of a character map in plane 2.
static uint16_t font_block(uint8_t block) {
    return (uint16_t)((block & 3) * 0x4000u + ((block >> 2) & 1) * 0x2000u);
}

// Plane 2 at A0000h, one byte per address, for character maps.
static void font_access_begin(void) {
    seq_write(0x02, 0x04);
    seq_write(0x04, 0x07);
    gc_write(0x04, 0x02);
    gc_write(0x05, 0x00);
    gc_write(0x06, 0x04);
}

static void font_access_end(const __seg_fs struct vgabios_mode *m) {
    seq_write(0x02, m->seq[1]);
    seq_write(0x04, m->seq[3]);
    gc_write(0x04, m->gc[4]);
    gc_write(0x05, m->gc[5]);
    gc_write(0x06, m->gc[6]);
}

static void load_rom_font(const __seg_fs struct vgabios_mode *m, const __seg_fs uint8_t *font,
                          uint8_t height, uint16_t first, uint16_t count, uint8_t block) {
    font_access_begin();
    uint16_t base = font_block(block);
    for (uint16_t i = 0; i < count; i++) {
        uint16_t c = (uint16_t)(first + i);
        far_copy_rom(0xa000, (uint16_t)(base + c * 32u), font + c * height, height);
        far_fill8(0xa000, (uint16_t)(base + c * 32u + height), 0, 32u - height);
    }
    font_access_end(m);
}

static void load_user_font(const __seg_fs struct vgabios_mode *m, uint16_t segment,
                           uint16_t offset, uint8_t height, uint16_t first, uint16_t count,
                           uint8_t block) {
    font_access_begin();
    uint16_t base = font_block(block);
    for (uint16_t i = 0; i < count; i++) {
        uint16_t c = (uint16_t)(first + i);
        far_copy(0xa000, (uint16_t)(base + c * 32u), segment, (uint16_t)(offset + i * height),
                 height);
    }
    font_access_end(m);
}

static void clear_screen(const __seg_fs struct vgabios_mode *m) {
    switch (m->kind) {
        case MODE_TEXT: {
            far_fill16(m->segment, 0, 0x0720, 0x4000);
            break;
        }
        case MODE_CGA:
        case MODE_CGA1: {
            far_fill16(0xb800, 0, 0, 0x4000);
            break;
        }
        default: {
            uint8_t mask = seq_read(0x02);
            seq_write(0x02, 0x0f);
            far_fill16(0xa000, 0, 0, 0x8000);
            seq_write(0x02, mask);
            break;
        }
    }
}

static void set_mode(uint8_t requested) {
    const __seg_fs struct vgabios_mode *m = find_mode(requested & 0x7f);
    if (m != 0 && is_text(m)) {
        m = find_mode_lines(m->mode, text_lines());
    }
    if (m == 0) {
        return;
    }
    bool keep = requested & 0x80;
    uint16_t port = (m->misc & 1) ? 0x3d4 : 0x3b4;

    vbe_disable();
    seq_write(0x01, m->seq[0] | SEQ_SCREEN_OFF);
    outb(MISC_WRITE, m->misc);
    seq_write(0x00, 0x01);
    for (uint8_t i = 0; i < 4; i++) {
        seq_write((uint8_t)(i + 1), i == 0 ? (uint8_t)(m->seq[0] | SEQ_SCREEN_OFF) : m->seq[i]);
    }
    seq_write(0x00, 0x03);

    // Unprotected first, so that registers 0-7 take their values.
    outb(port, 0x11);
    outb(port + 1, m->crtc[0x11] & 0x7f);
    for (uint8_t i = 0; i < 25; i++) {
        outb(port, i);
        outb(port + 1, m->crtc[i]);
    }

    inb(port + 6);
    for (uint8_t i = 0; i < 20; i++) {
        outb(AC_INDEX, i);
        outb(AC_INDEX, m->ac[i]);
    }
    outb(AC_INDEX, 0x14);
    outb(AC_INDEX, 0x00);

    for (uint8_t i = 0; i < 9; i++) {
        gc_write(i, m->gc[i]);
    }

    pokew(BDA, BDA_CRTC, port);
    outb(PEL_MASK, 0xff);
    if (!(peekb(BDA, BDA_VGA_FLAGS) & VGA_FLAGS_NO_PALETTE)) {
        load_palette(m->palette);
    }
    if (!keep) {
        clear_screen(m);
    }
    if (is_text(m)) {
        load_rom_font(m, rom_font(m->char_height), m->char_height, 0, 256, 0);
    }

    pokeb(BDA, BDA_MODE, m->mode);
    pokew(BDA, BDA_COLUMNS, m->columns);
    pokew(BDA, BDA_PAGE_SIZE, m->page_size);
    pokew(BDA, BDA_PAGE_START, 0);
    for (uint8_t page = 0; page < 8; page++) {
        pokew(BDA, (uint16_t)(BDA_CURSOR + page * 2), 0);
    }
    pokew(BDA, BDA_CURSOR_TYPE, is_text(m) ? 0x0607 : 0);
    pokeb(BDA, BDA_PAGE, 0);
    pokeb(BDA, BDA_MODE_CONTROL, m->mode < 8 ? cga_mode_control[m->mode] : 0);
    pokeb(BDA, BDA_PALETTE, m->mode == 6 ? 0x3f : 0x30);
    pokeb(BDA, BDA_ROWS, (uint8_t)(m->rows - 1));
    pokew(BDA, BDA_CHAR_HEIGHT, m->char_height);
    uint8_t control = peekb(BDA, BDA_VIDEO_CONTROL) & (uint8_t)~VIDEO_CONTROL_KEPT;
    pokeb(BDA, BDA_VIDEO_CONTROL, (uint8_t)(control | (keep ? VIDEO_CONTROL_KEPT : 0)));
    uint16_t equipment = peekw(BDA, BDA_EQUIPMENT) & (uint16_t)~0x30;
    pokew(BDA, BDA_EQUIPMENT, (uint16_t)(equipment | (port == 0x3b4 ? 0x30 : 0x20)));

    uint16_t cs = code_segment();
    const __seg_fs uint8_t *font = is_text(m) ? vgabios_font8 : rom_font(m->char_height);
    set_vector(0x43, cs, rom_offset(font));
    set_vector(0x1f, cs, rom_offset(vgabios_font8 + 128 * 8));

    inb(port + 6);
    outb(AC_INDEX, AC_PAS);
    seq_write(0x01, m->seq[0]);
}

void vgabios_set_mode(uint8_t mode) {
    set_mode(mode);
}

static void set_cursor(uint8_t page, uint8_t row, uint8_t column) {
    pokew(BDA, (uint16_t)(BDA_CURSOR + page * 2), (uint16_t)(row << 8 | column));
    if (page != peekb(BDA, BDA_PAGE)) {
        return;
    }
    uint16_t columns = peekw(BDA, BDA_COLUMNS);
    uint16_t address = (uint16_t)(peekw(BDA, BDA_PAGE_START) / 2 + row * columns + column);
    crtc_write(0x0e, (uint8_t)(address >> 8));
    crtc_write(0x0f, (uint8_t)address);
}

static uint16_t get_cursor(uint8_t page) {
    return peekw(BDA, (uint16_t)(BDA_CURSOR + (page & 7) * 2));
}

// The cursor's scan lines, from the CGA-style ones of an 8-line cell to
// those of the font in use, unless the caller turned that off.
static void set_cursor_type(uint8_t start, uint8_t end) {
    pokew(BDA, BDA_CURSOR_TYPE, (uint16_t)(start << 8 | end));
    uint8_t height = peekb(BDA, BDA_CHAR_HEIGHT);
    if (!(peekb(BDA, BDA_VIDEO_CONTROL) & VIDEO_CONTROL_NO_EMULATION) && height > 8
        && !(start & 0x20) && start < 8 && end < 8) {
        if (start == 6 && end == 7) {
            start = (uint8_t)(height - 3);
            end = (uint8_t)(height - 2);
        } else {
            start = (uint8_t)(start * height / 8);
            end = (uint8_t)((end + 1) * height / 8 - 1);
        }
    }
    crtc_write(0x0a, start);
    crtc_write(0x0b, end);
}

static void set_page(uint8_t page) {
    const __seg_fs struct vgabios_mode *m = current_mode();
    if (m == 0 || page > 7) {
        return;
    }
    uint16_t start = (uint16_t)(page * peekw(BDA, BDA_PAGE_SIZE));
    pokeb(BDA, BDA_PAGE, page);
    pokew(BDA, BDA_PAGE_START, start);
    // Text modes count the start in words.
    uint16_t address = is_text(m) ? start / 2 : start;
    crtc_write(0x0c, (uint8_t)(address >> 8));
    crtc_write(0x0d, (uint8_t)address);
    uint16_t cursor = get_cursor(page);
    set_cursor(page, (uint8_t)(cursor >> 8), (uint8_t)cursor);
}

static uint16_t text_offset(uint8_t page, uint8_t row, uint8_t column) {
    return (uint16_t)(page * peekw(BDA, BDA_PAGE_SIZE)
                      + (row * peekw(BDA, BDA_COLUMNS) + column) * 2u);
}

// A CGA mode's byte holding pixel x of scan line y: even lines in the
// first 8 KiB, odd ones in the second.
static uint16_t cga_offset(uint16_t x_byte, uint16_t y) {
    return (uint16_t)((y >> 1) * 80u + (y & 1) * 0x2000u + x_byte);
}

static uint16_t font_segment(void) {
    return peekw(0, VECTOR(0x43) + 2);
}

static uint16_t glyph_offset(const __seg_fs struct vgabios_mode *m, uint8_t c, uint8_t height) {
    // CGA modes find characters 128-255 through INT 1Fh.
    if ((m->kind == MODE_CGA || m->kind == MODE_CGA1) && c >= 128) {
        return (uint16_t)(peekw(0, VECTOR(0x1f)) + (c - 128) * height);
    }
    return (uint16_t)(peekw(0, VECTOR(0x43)) + c * height);
}

static uint16_t glyph_segment(const __seg_fs struct vgabios_mode *m, uint8_t c) {
    if ((m->kind == MODE_CGA || m->kind == MODE_CGA1) && c >= 128) {
        return peekw(0, VECTOR(0x1f) + 2);
    }
    return font_segment();
}

static void draw_char(const __seg_fs struct vgabios_mode *m, uint8_t row, uint8_t column,
                      uint8_t c, uint8_t color) {
    uint8_t height = peekb(BDA, BDA_CHAR_HEIGHT);
    uint16_t segment = glyph_segment(m, c);
    uint16_t offset = glyph_offset(m, c, height);
    bool xor = color & 0x80;
    switch (m->kind) {
        case MODE_CGA: {
            for (uint8_t r = 0; r < height; r++) {
                uint8_t bits = peekb(segment, (uint16_t)(offset + r));
                uint16_t pixels = 0;
                for (uint8_t b = 0; b < 8; b++) {
                    if (bits & (0x80 >> b)) {
                        pixels |= (uint16_t)((color & 3) << (14 - 2 * b));
                    }
                }
                uint16_t at = cga_offset((uint16_t)(column * 2), (uint16_t)(row * height + r));
                uint8_t high = (uint8_t)(pixels >> 8);
                uint8_t low = (uint8_t)pixels;
                if (xor) {
                    high ^= peekb(0xb800, at);
                    low ^= peekb(0xb800, (uint16_t)(at + 1));
                }
                pokeb(0xb800, at, high);
                pokeb(0xb800, (uint16_t)(at + 1), low);
            }
            break;
        }
        case MODE_CGA1: {
            for (uint8_t r = 0; r < height; r++) {
                uint8_t bits = (color & 1) ? peekb(segment, (uint16_t)(offset + r)) : 0;
                uint16_t at = cga_offset(column, (uint16_t)(row * height + r));
                if (xor) {
                    bits ^= peekb(0xb800, at);
                }
                pokeb(0xb800, at, bits);
            }
            break;
        }
        case MODE_PLANAR: {
            uint16_t line = m->width / 8;
            gc_write(0x00, color & 0x0f);
            gc_write(0x01, 0x0f);
            gc_write(0x03, xor ? 0x18 : 0x00);
            for (uint8_t r = 0; r < height; r++) {
                uint8_t bits = peekb(segment, (uint16_t)(offset + r));
                uint16_t at = (uint16_t)((row * height + r) * line + column);
                gc_write(0x08, bits);
                (void)peekb(0xa000, at);
                pokeb(0xa000, at, 0xff);
                if (!xor) {
                    gc_write(0x00, 0x00);
                    gc_write(0x08, (uint8_t)~bits);
                    (void)peekb(0xa000, at);
                    pokeb(0xa000, at, 0xff);
                    gc_write(0x00, color & 0x0f);
                }
            }
            gc_write(0x00, 0x00);
            gc_write(0x01, 0x00);
            gc_write(0x03, 0x00);
            gc_write(0x08, 0xff);
            break;
        }
        default: {
            for (uint8_t r = 0; r < height; r++) {
                uint8_t bits = peekb(segment, (uint16_t)(offset + r));
                uint16_t at = (uint16_t)((row * height + r) * m->width + column * 8u);
                for (uint8_t b = 0; b < 8; b++) {
                    pokeb(0xa000, (uint16_t)(at + b), (bits & (0x80 >> b)) ? color : 0);
                }
            }
            break;
        }
    }
}

static void write_chars(uint8_t c, uint8_t page, uint8_t attribute, uint16_t count,
                        bool with_attribute) {
    const __seg_fs struct vgabios_mode *m = current_mode();
    if (m == 0) {
        return;
    }
    uint16_t cursor = get_cursor(page);
    uint8_t row = (uint8_t)(cursor >> 8);
    uint8_t column = (uint8_t)cursor;
    uint16_t columns = peekw(BDA, BDA_COLUMNS);
    if (is_text(m)) {
        uint16_t at = text_offset(page, row, column);
        for (uint16_t i = 0; i < count; i++) {
            if (with_attribute) {
                pokew(m->segment, (uint16_t)(at + i * 2), (uint16_t)(attribute << 8 | c));
            } else {
                pokeb(m->segment, (uint16_t)(at + i * 2), c);
            }
        }
        return;
    }
    for (uint16_t i = 0; i < count && column < columns; i++) {
        draw_char(m, row, column++, c, attribute);
    }
}

static void scroll_text(const __seg_fs struct vgabios_mode *m, uint8_t page, int lines,
                        uint8_t top, uint8_t left, uint8_t bottom, uint8_t right,
                        uint8_t attribute) {
    uint16_t width = (uint16_t)(right - left + 1);
    int height = bottom - top + 1;
    uint16_t blank = (uint16_t)(attribute << 8 | ' ');
    int n = lines < 0 ? -lines : lines;
    if (n == 0 || n >= height) {
        n = height;
    }
    if (lines >= 0) {
        for (int r = top; r + n <= bottom; r++) {
            far_move(m->segment, text_offset(page, (uint8_t)r, left),
                     text_offset(page, (uint8_t)(r + n), left), width * 2u);
        }
        for (int r = bottom - n + 1; r <= bottom; r++) {
            far_fill16(m->segment, text_offset(page, (uint8_t)r, left), blank, width);
        }
    } else {
        for (int r = bottom; r - n >= top; r--) {
            far_move(m->segment, text_offset(page, (uint8_t)r, left),
                     text_offset(page, (uint8_t)(r - n), left), width * 2u);
        }
        for (int r = top; r < top + n; r++) {
            far_fill16(m->segment, text_offset(page, (uint8_t)r, left), blank, width);
        }
    }
}

// A graphics mode's scan line y, from byte column x, and how many bytes a
// character takes in it.
static uint16_t pixel_line(const __seg_fs struct vgabios_mode *m, uint16_t x, uint16_t y) {
    switch (m->kind) {
        case MODE_CGA:
        case MODE_CGA1: {
            return cga_offset(x, y);
        }
        case MODE_PLANAR: {
            return (uint16_t)(y * (m->width / 8u) + x);
        }
        default: {
            return (uint16_t)(y * m->width + x);
        }
    }
}

static uint8_t char_bytes(const __seg_fs struct vgabios_mode *m) {
    switch (m->kind) {
        case MODE_CGA: {
            return 2;
        }
        case MODE_LINEAR: {
            return 8;
        }
        default: {
            return 1;
        }
    }
}

static void fill_line(const __seg_fs struct vgabios_mode *m, uint16_t at, uint16_t bytes,
                      uint8_t color) {
    switch (m->kind) {
        case MODE_CGA: {
            uint8_t c = color & 3;
            far_fill8(0xb800, at, (uint8_t)(c | c << 2 | c << 4 | c << 6), bytes);
            break;
        }
        case MODE_CGA1: {
            far_fill8(0xb800, at, (color & 1) ? 0xff : 0x00, bytes);
            break;
        }
        case MODE_PLANAR: {
            gc_write(0x00, color & 0x0f);
            gc_write(0x01, 0x0f);
            far_fill8(0xa000, at, 0xff, bytes);
            gc_write(0x00, 0x00);
            gc_write(0x01, 0x00);
            break;
        }
        default: {
            far_fill8(0xa000, at, color, bytes);
            break;
        }
    }
}

static void scroll_graphics(const __seg_fs struct vgabios_mode *m, int lines, uint8_t top,
                            uint8_t left, uint8_t bottom, uint8_t right, uint8_t color) {
    uint8_t height = peekb(BDA, BDA_CHAR_HEIGHT);
    uint16_t segment = m->kind == MODE_CGA || m->kind == MODE_CGA1 ? 0xb800 : 0xa000;
    uint8_t per_char = char_bytes(m);
    uint16_t x = (uint16_t)(left * per_char);
    uint16_t bytes = (uint16_t)((right - left + 1) * per_char);
    int rows = bottom - top + 1;
    int n = lines < 0 ? -lines : lines;
    if (n == 0 || n >= rows) {
        n = rows;
    }
    // Planes move together through the latches.
    if (m->kind == MODE_PLANAR) {
        gc_write(0x05, (uint8_t)((m->gc[5] & ~3) | 1));
    }
    if (lines >= 0) {
        for (int r = top; r + n <= bottom; r++) {
            for (uint8_t s = 0; s < height; s++) {
                far_move(segment, pixel_line(m, x, (uint16_t)(r * height + s)),
                         pixel_line(m, x, (uint16_t)((r + n) * height + s)), bytes);
            }
        }
    } else {
        for (int r = bottom; r - n >= top; r--) {
            for (uint8_t s = 0; s < height; s++) {
                far_move(segment, pixel_line(m, x, (uint16_t)(r * height + s)),
                         pixel_line(m, x, (uint16_t)((r - n) * height + s)), bytes);
            }
        }
    }
    if (m->kind == MODE_PLANAR) {
        gc_write(0x05, m->gc[5]);
    }
    int first = lines >= 0 ? bottom - n + 1 : top;
    for (int r = first; r < first + n; r++) {
        for (uint8_t s = 0; s < height; s++) {
            fill_line(m, pixel_line(m, x, (uint16_t)(r * height + s)), bytes, color);
        }
    }
}

static void scroll(int lines, uint8_t attribute, uint8_t top, uint8_t left, uint8_t bottom,
                   uint8_t right) {
    const __seg_fs struct vgabios_mode *m = current_mode();
    if (m == 0) {
        return;
    }
    uint8_t rows = (uint8_t)(peekb(BDA, BDA_ROWS) + 1);
    uint8_t columns = (uint8_t)peekw(BDA, BDA_COLUMNS);
    if (bottom >= rows) {
        bottom = (uint8_t)(rows - 1);
    }
    if (right >= columns) {
        right = (uint8_t)(columns - 1);
    }
    if (top > bottom || left > right) {
        return;
    }
    if (is_text(m)) {
        scroll_text(m, peekb(BDA, BDA_PAGE), lines, top, left, bottom, right, attribute);
    } else {
        scroll_graphics(m, lines, top, left, bottom, right, attribute);
    }
}

static void teletype(uint8_t c, uint8_t color) {
    const __seg_fs struct vgabios_mode *m = current_mode();
    if (m == 0) {
        return;
    }
    uint8_t page = peekb(BDA, BDA_PAGE);
    uint16_t cursor = get_cursor(page);
    uint8_t row = (uint8_t)(cursor >> 8);
    uint8_t column = (uint8_t)cursor;
    uint8_t columns = (uint8_t)peekw(BDA, BDA_COLUMNS);
    uint8_t rows = (uint8_t)(peekb(BDA, BDA_ROWS) + 1);
    switch (c) {
        case 7: {
            return;
        }
        case 8: {
            if (column > 0) {
                column--;
            }
            break;
        }
        case 10: {
            row++;
            break;
        }
        case 13: {
            column = 0;
            break;
        }
        default: {
            if (is_text(m)) {
                pokeb(m->segment, text_offset(page, row, column), c);
            } else {
                draw_char(m, row, column, c, color);
            }
            if (++column >= columns) {
                column = 0;
                row++;
            }
            break;
        }
    }
    if (row >= rows) {
        // The new line takes the attribute where the cursor is.
        uint8_t attribute = 0;
        if (is_text(m)) {
            attribute = peekb(m->segment, (uint16_t)(text_offset(page, (uint8_t)(rows - 1), column) + 1));
        }
        scroll(1, attribute, 0, 0, (uint8_t)(rows - 1), (uint8_t)(columns - 1));
        row = (uint8_t)(rows - 1);
    }
    set_cursor(page, row, column);
}

static void write_pixel(uint16_t x, uint16_t y, uint8_t color) {
    const __seg_fs struct vgabios_mode *m = current_mode();
    if (m == 0 || is_text(m)) {
        return;
    }
    bool xor = color & 0x80;
    switch (m->kind) {
        case MODE_CGA: {
            uint16_t at = cga_offset(x / 4, y);
            uint8_t shift = (uint8_t)((3 - (x & 3)) * 2);
            uint8_t v = peekb(0xb800, at);
            if (xor) {
                v ^= (uint8_t)((color & 3) << shift);
            } else {
                v = (uint8_t)((v & ~(3 << shift)) | ((color & 3) << shift));
            }
            pokeb(0xb800, at, v);
            break;
        }
        case MODE_CGA1: {
            uint16_t at = cga_offset(x / 8, y);
            uint8_t bit = (uint8_t)(0x80 >> (x & 7));
            uint8_t v = peekb(0xb800, at);
            if (xor) {
                v ^= (color & 1) ? bit : 0;
            } else {
                v = (uint8_t)((v & ~bit) | ((color & 1) ? bit : 0));
            }
            pokeb(0xb800, at, v);
            break;
        }
        case MODE_PLANAR: {
            uint16_t at = (uint16_t)(y * (m->width / 8u) + x / 8);
            gc_write(0x08, (uint8_t)(0x80 >> (x & 7)));
            gc_write(0x05, (uint8_t)((m->gc[5] & ~3) | 2));
            if (xor) {
                gc_write(0x03, 0x18);
            }
            (void)peekb(0xa000, at);
            pokeb(0xa000, at, color);
            gc_write(0x03, 0x00);
            gc_write(0x05, m->gc[5]);
            gc_write(0x08, 0xff);
            break;
        }
        default: {
            pokeb(0xa000, (uint16_t)(y * m->width + x), color);
            break;
        }
    }
}

static uint8_t read_pixel(uint16_t x, uint16_t y) {
    const __seg_fs struct vgabios_mode *m = current_mode();
    if (m == 0 || is_text(m)) {
        return 0;
    }
    switch (m->kind) {
        case MODE_CGA: {
            return (uint8_t)((peekb(0xb800, cga_offset(x / 4, y)) >> ((3 - (x & 3)) * 2)) & 3);
        }
        case MODE_CGA1: {
            return (uint8_t)((peekb(0xb800, cga_offset(x / 8, y)) >> (7 - (x & 7))) & 1);
        }
        case MODE_PLANAR: {
            uint16_t at = (uint16_t)(y * (m->width / 8u) + x / 8);
            uint8_t color = 0;
            for (uint8_t p = 0; p < 4; p++) {
                gc_write(0x04, p);
                if ((peekb(0xa000, at) >> (7 - (x & 7))) & 1) {
                    color |= (uint8_t)(1 << p);
                }
            }
            gc_write(0x04, m->gc[4]);
            return color;
        }
        default: {
            return peekb(0xa000, (uint16_t)(y * m->width + x));
        }
    }
}

static void read_char(struct vgabios_regs *r) {
    const __seg_fs struct vgabios_mode *m = current_mode();
    if (m == 0 || !is_text(m)) {
        r->ax = 0;
        return;
    }
    uint16_t cursor = get_cursor(r->bh);
    r->ax = peekw(m->segment, text_offset(r->bh & 7, (uint8_t)(cursor >> 8), (uint8_t)cursor));
}

static void write_string(struct vgabios_regs *r) {
    uint8_t page = r->bh & 7;
    uint16_t saved = get_cursor(page);
    uint8_t active = peekb(BDA, BDA_PAGE);
    // The string goes to its page through the active page's teletype.
    pokeb(BDA, BDA_PAGE, page);
    set_cursor(page, r->dh, r->dl);
    bool attributes = r->al & 2;
    uint16_t at = r->bp;
    for (uint16_t i = 0; i < r->cx; i++) {
        uint8_t c = peekb(r->es, at++);
        uint8_t attribute = r->bl;
        if (attributes) {
            attribute = peekb(r->es, at++);
        }
        if (c == 7 || c == 8 || c == 10 || c == 13) {
            teletype(c, attribute);
            continue;
        }
        const __seg_fs struct vgabios_mode *m = current_mode();
        if (m != 0 && is_text(m)) {
            uint16_t cursor = get_cursor(page);
            pokew(m->segment, text_offset(page, (uint8_t)(cursor >> 8), (uint8_t)cursor),
                  (uint16_t)(attribute << 8 | c));
            // The teletype moves on, overwriting the character with itself.
            teletype(c, attribute);
        } else {
            teletype(c, attribute);
        }
    }
    if (!(r->al & 1)) {
        set_cursor(page, (uint8_t)(saved >> 8), (uint8_t)saved);
    }
    pokeb(BDA, BDA_PAGE, active);
}

static void palette_functions(struct vgabios_regs *r) {
    switch (r->al) {
        case 0x00: {
            ac_write(r->bl & 0x1f, r->bh);
            break;
        }
        case 0x01: {
            ac_write(0x11, r->bh);
            break;
        }
        case 0x02: {
            for (uint8_t i = 0; i < 16; i++) {
                ac_write(i, peekb(r->es, (uint16_t)(r->dx + i)));
            }
            ac_write(0x11, peekb(r->es, (uint16_t)(r->dx + 16)));
            break;
        }
        case 0x03: {
            uint8_t mode = ac_read(0x10);
            ac_write(0x10, (uint8_t)((mode & ~0x08) | (r->bl ? 0x08 : 0x00)));
            break;
        }
        case 0x07: {
            r->bh = ac_read(r->bl & 0x1f);
            break;
        }
        case 0x08: {
            r->bh = ac_read(0x11);
            break;
        }
        case 0x09: {
            for (uint8_t i = 0; i < 16; i++) {
                pokeb(r->es, (uint16_t)(r->dx + i), ac_read(i));
            }
            pokeb(r->es, (uint16_t)(r->dx + 16), ac_read(0x11));
            break;
        }
        case 0x10: {
            outb(DAC_WRITE_INDEX, r->bl);
            outb(DAC_DATA, r->dh);
            outb(DAC_DATA, r->ch);
            outb(DAC_DATA, r->cl);
            break;
        }
        case 0x12: {
            outb(DAC_WRITE_INDEX, r->bl);
            for (uint16_t i = 0; i < r->cx * 3u; i++) {
                outb(DAC_DATA, peekb(r->es, (uint16_t)(r->dx + i)));
            }
            break;
        }
        case 0x13: {
            uint8_t mode = ac_read(0x10);
            if (r->bl == 0) {
                ac_write(0x10, (uint8_t)((mode & ~0x80) | (r->bh ? 0x80 : 0x00)));
            } else {
                uint8_t page = r->bh;
                ac_write(0x14, (mode & 0x80) ? (uint8_t)(page & 0x0f) : (uint8_t)((page & 3) << 2));
            }
            break;
        }
        case 0x15: {
            outb(DAC_READ_INDEX, r->bl);
            r->dh = inb(DAC_DATA);
            r->ch = inb(DAC_DATA);
            r->cl = inb(DAC_DATA);
            break;
        }
        case 0x17: {
            outb(DAC_READ_INDEX, r->bl);
            for (uint16_t i = 0; i < r->cx * 3u; i++) {
                pokeb(r->es, (uint16_t)(r->dx + i), inb(DAC_DATA));
            }
            break;
        }
        case 0x18: {
            outb(PEL_MASK, r->bl);
            break;
        }
        case 0x19: {
            r->bl = inb(PEL_MASK);
            break;
        }
        case 0x1a: {
            uint8_t mode = ac_read(0x10);
            uint8_t select = ac_read(0x14);
            r->bl = (mode & 0x80) ? 1 : 0;
            r->bh = (mode & 0x80) ? (uint8_t)(select & 0x0f) : (uint8_t)((select >> 2) & 3);
            break;
        }
        case 0x1b: {
            uint8_t flags = peekb(BDA, BDA_VGA_FLAGS);
            pokeb(BDA, BDA_VGA_FLAGS, (uint8_t)(flags | VGA_FLAGS_GREY));
            for (uint16_t i = 0; i < r->cx; i++) {
                uint8_t index = (uint8_t)(r->bl + i);
                outb(DAC_READ_INDEX, index);
                uint8_t red = inb(DAC_DATA);
                uint8_t green = inb(DAC_DATA);
                uint8_t blue = inb(DAC_DATA);
                uint8_t y = (uint8_t)((red * 77u + green * 151u + blue * 28u + 128u) >> 8);
                outb(DAC_WRITE_INDEX, index);
                outb(DAC_DATA, y);
                outb(DAC_DATA, y);
                outb(DAC_DATA, y);
            }
            pokeb(BDA, BDA_VGA_FLAGS, flags);
            break;
        }
        default: {
            break;
        }
    }
}

// After a text font load that recalculates: the cell height in the CRT
// controller, and the rows, page size, and cursor that follow from it.
static void text_rows(const __seg_fs struct vgabios_mode *m, uint8_t height) {
    if (height == 0 || height > 32) {
        return;
    }
    uint8_t scan = crtc_read(0x09);
    crtc_write(0x09, (uint8_t)((scan & 0xe0) | (height - 1)));
    uint16_t lines = (uint16_t)(crtc_read(0x12) | ((crtc_read(0x07) & 0x02) << 7)
                                | ((crtc_read(0x07) & 0x40) << 3)) + 1u;
    uint8_t rows = (uint8_t)(lines / height);
    pokeb(BDA, BDA_ROWS, (uint8_t)(rows - 1));
    pokew(BDA, BDA_CHAR_HEIGHT, height);
    pokew(BDA, BDA_PAGE_SIZE, (uint16_t)((rows * m->columns * 2u + 0xff) & ~0xffu));
    crtc_write(0x0a, (uint8_t)(height - 3));
    crtc_write(0x0b, (uint8_t)(height - 2));
}

static void font_functions(struct vgabios_regs *r) {
    const __seg_fs struct vgabios_mode *m = current_mode();
    if (m == 0) {
        return;
    }
    uint16_t cs = code_segment();
    switch (r->al) {
        case 0x00:
        case 0x10: {
            load_user_font(m, r->es, r->bp, r->bh, r->dx, r->cx, r->bl);
            if (r->al == 0x10) {
                text_rows(m, r->bh);
            }
            break;
        }
        case 0x01:
        case 0x11: {
            load_rom_font(m, vgabios_font14, 14, 0, 256, r->bl);
            if (r->al == 0x11) {
                text_rows(m, 14);
            }
            break;
        }
        case 0x02:
        case 0x12: {
            load_rom_font(m, vgabios_font8, 8, 0, 256, r->bl);
            if (r->al == 0x12) {
                text_rows(m, 8);
            }
            break;
        }
        case 0x04:
        case 0x14: {
            load_rom_font(m, vgabios_font16, 16, 0, 256, r->bl);
            if (r->al == 0x14) {
                text_rows(m, 16);
            }
            break;
        }
        case 0x03: {
            seq_write(0x03, r->bl);
            break;
        }
        case 0x20: {
            set_vector(0x1f, r->es, r->bp);
            break;
        }
        case 0x21:
        case 0x22:
        case 0x23:
        case 0x24: {
            uint8_t height = r->al == 0x21 ? r->cl : r->al == 0x22 ? 14 : r->al == 0x23 ? 8 : 16;
            if (r->al == 0x21) {
                set_vector(0x43, r->es, r->bp);
            } else {
                set_vector(0x43, cs, rom_offset(rom_font(height)));
            }
            pokew(BDA, BDA_CHAR_HEIGHT, height);
            uint8_t rows = r->bl == 1 ? 14 : r->bl == 2 ? 25 : r->bl == 3 ? 43 : r->dl;
            if (rows != 0) {
                pokeb(BDA, BDA_ROWS, (uint8_t)(rows - 1));
            }
            break;
        }
        case 0x30: {
            uint16_t segment = cs;
            uint16_t offset = 0;
            switch (r->bh) {
                case 0x00: {
                    segment = peekw(0, VECTOR(0x1f) + 2);
                    offset = peekw(0, VECTOR(0x1f));
                    break;
                }
                case 0x01: {
                    segment = peekw(0, VECTOR(0x43) + 2);
                    offset = peekw(0, VECTOR(0x43));
                    break;
                }
                case 0x02:
                case 0x05: {
                    offset = rom_offset(vgabios_font14);
                    break;
                }
                case 0x03: {
                    offset = rom_offset(vgabios_font8);
                    break;
                }
                case 0x04: {
                    offset = rom_offset(vgabios_font8 + 128 * 8);
                    break;
                }
                default: {
                    offset = rom_offset(vgabios_font16);
                    break;
                }
            }
            r->es = segment;
            r->bp = offset;
            r->cx = peekw(BDA, BDA_CHAR_HEIGHT);
            r->dl = peekb(BDA, BDA_ROWS);
            break;
        }
        default: {
            break;
        }
    }
}

static void alternate_select(struct vgabios_regs *r) {
    switch (r->bl) {
        case 0x10: {
            r->bh = crtc_port() == 0x3b4 ? 1 : 0;
            r->bl = 0x03;
            r->ch = 0;
            r->cl = peekb(BDA, BDA_SWITCHES) & 0x0f;
            break;
        }
        case 0x30: {
            uint8_t flags = peekb(BDA, BDA_VGA_FLAGS) & (uint8_t)~(VGA_FLAGS_200 | VGA_FLAGS_400);
            if (r->al == 0) {
                flags |= VGA_FLAGS_200;
            } else if (r->al == 2) {
                flags |= VGA_FLAGS_400;
            }
            pokeb(BDA, BDA_VGA_FLAGS, flags);
            r->al = 0x12;
            break;
        }
        case 0x31: {
            uint8_t flags = peekb(BDA, BDA_VGA_FLAGS) & (uint8_t)~VGA_FLAGS_NO_PALETTE;
            pokeb(BDA, BDA_VGA_FLAGS, (uint8_t)(flags | (r->al ? VGA_FLAGS_NO_PALETTE : 0)));
            r->al = 0x12;
            break;
        }
        case 0x32: {
            outb(VGA_ENABLE, r->al ? 0x00 : 0x01);
            r->al = 0x12;
            break;
        }
        case 0x33: {
            uint8_t flags = peekb(BDA, BDA_VGA_FLAGS) & (uint8_t)~VGA_FLAGS_GREY;
            pokeb(BDA, BDA_VGA_FLAGS, (uint8_t)(flags | (r->al ? 0 : VGA_FLAGS_GREY)));
            r->al = 0x12;
            break;
        }
        case 0x34: {
            uint8_t control = peekb(BDA, BDA_VIDEO_CONTROL) & (uint8_t)~VIDEO_CONTROL_NO_EMULATION;
            pokeb(BDA, BDA_VIDEO_CONTROL,
                  (uint8_t)(control | (r->al ? VIDEO_CONTROL_NO_EMULATION : 0)));
            r->al = 0x12;
            break;
        }
        case 0x35: {
            r->al = 0x12;
            break;
        }
        case 0x36: {
            uint8_t clocking = seq_read(0x01) & (uint8_t)~SEQ_SCREEN_OFF;
            seq_write(0x01, (uint8_t)(clocking | (r->al ? SEQ_SCREEN_OFF : 0)));
            r->al = 0x12;
            break;
        }
        default: {
            break;
        }
    }
}

static void state_information(struct vgabios_regs *r) {
    const __seg_fs struct vgabios_mode *m = current_mode();
    uint16_t to = r->di;
    uint16_t es = r->es;
    pokew(es, to, rom_offset(functionality));
    pokew(es, (uint16_t)(to + 2), code_segment());
    pokeb(es, (uint16_t)(to + 4), peekb(BDA, BDA_MODE));
    pokew(es, (uint16_t)(to + 5), peekw(BDA, BDA_COLUMNS));
    pokew(es, (uint16_t)(to + 7), peekw(BDA, BDA_PAGE_SIZE));
    pokew(es, (uint16_t)(to + 9), peekw(BDA, BDA_PAGE_START));
    for (uint8_t i = 0; i < 16; i++) {
        pokeb(es, (uint16_t)(to + 11 + i), peekb(BDA, (uint16_t)(BDA_CURSOR + i)));
    }
    pokew(es, (uint16_t)(to + 27), peekw(BDA, BDA_CURSOR_TYPE));
    pokeb(es, (uint16_t)(to + 29), peekb(BDA, BDA_PAGE));
    pokew(es, (uint16_t)(to + 30), crtc_port());
    pokeb(es, (uint16_t)(to + 32), peekb(BDA, BDA_MODE_CONTROL));
    pokeb(es, (uint16_t)(to + 33), peekb(BDA, BDA_PALETTE));
    pokeb(es, (uint16_t)(to + 34), (uint8_t)(peekb(BDA, BDA_ROWS) + 1));
    pokew(es, (uint16_t)(to + 35), peekw(BDA, BDA_CHAR_HEIGHT));
    pokeb(es, (uint16_t)(to + 37), crtc_port() == 0x3b4 ? DCC_VGA_MONO : DCC_VGA_COLOR);
    pokeb(es, (uint16_t)(to + 38), 0);
    uint16_t colors = 16;
    uint8_t pages = 1;
    uint8_t lines = 2;
    if (m != 0) {
        if (m->kind == MODE_LINEAR) {
            colors = 256;
        } else if (m->kind == MODE_CGA) {
            colors = 4;
        } else if (m->kind == MODE_CGA1 || m->mode == 0x07 || m->mode == 0x0f || m->mode == 0x11) {
            colors = 2;
        }
        if (m->page_size != 0) {
            pages = (uint8_t)(m->kind == MODE_TEXT ? 0x8000u / m->page_size : 0x10000u / m->page_size);
        }
        lines = m->height == 200 ? 0 : m->height == 350 ? 1 : m->height == 480 ? 3 : 2;
    }
    pokew(es, (uint16_t)(to + 39), colors);
    pokeb(es, (uint16_t)(to + 41), pages);
    pokeb(es, (uint16_t)(to + 42), lines);
    uint8_t map = seq_read(0x03);
    pokeb(es, (uint16_t)(to + 43), (uint8_t)(((map >> 3) & 4) | ((map >> 2) & 3)));
    pokeb(es, (uint16_t)(to + 44), (uint8_t)(((map >> 2) & 4) | (map & 3)));
    pokeb(es, (uint16_t)(to + 45), (uint8_t)(peekb(BDA, BDA_VGA_FLAGS) & 0x0f));
    for (uint8_t i = 46; i < 64; i++) {
        pokeb(es, (uint16_t)(to + i), 0);
    }
    pokeb(es, (uint16_t)(to + 49), 3);
    r->al = 0x1b;
}

void vgabios_int10(struct vgabios_regs *r) {
    switch (r->ah) {
        case 0x00: {
            set_mode(r->al);
            break;
        }
        case 0x01: {
            set_cursor_type(r->ch, r->cl);
            break;
        }
        case 0x02: {
            set_cursor(r->bh & 7, r->dh, r->dl);
            break;
        }
        case 0x03: {
            uint16_t cursor = get_cursor(r->bh);
            r->dx = cursor;
            r->cx = peekw(BDA, BDA_CURSOR_TYPE);
            break;
        }
        case 0x04: {
            r->ah = 0;
            break;
        }
        case 0x05: {
            set_page(r->al);
            break;
        }
        case 0x06: {
            scroll(r->al, r->bh, r->ch, r->cl, r->dh, r->dl);
            break;
        }
        case 0x07: {
            scroll(-(int)r->al, r->bh, r->ch, r->cl, r->dh, r->dl);
            break;
        }
        case 0x08: {
            read_char(r);
            break;
        }
        case 0x09: {
            write_chars(r->al, r->bh & 7, r->bl, r->cx, true);
            break;
        }
        case 0x0a: {
            write_chars(r->al, r->bh & 7, r->bl, r->cx, false);
            break;
        }
        case 0x0b: {
            if (r->bh == 0) {
                ac_write(0x11, r->bl & 0x0f);
                const __seg_fs struct vgabios_mode *m = current_mode();
                if (m != 0 && !is_text(m)) {
                    ac_write(0x00, r->bl & 0x0f);
                }
            } else {
                // The CGA palettes: green, red, and brown, or cyan, magenta,
                // and white.
                uint8_t base = r->bl & 1 ? 3 : 2;
                for (uint8_t i = 1; i < 4; i++) {
                    ac_write(i, (uint8_t)(base + (i - 1) * 2));
                }
            }
            break;
        }
        case 0x0c: {
            write_pixel(r->cx, r->dx, r->al);
            break;
        }
        case 0x0d: {
            r->al = read_pixel(r->cx, r->dx);
            break;
        }
        case 0x0e: {
            teletype(r->al, r->bl);
            break;
        }
        case 0x0f: {
            r->al = (uint8_t)(peekb(BDA, BDA_MODE) | (peekb(BDA, BDA_VIDEO_CONTROL) & 0x80));
            r->ah = (uint8_t)peekw(BDA, BDA_COLUMNS);
            r->bh = peekb(BDA, BDA_PAGE);
            break;
        }
        case 0x10: {
            palette_functions(r);
            break;
        }
        case 0x11: {
            font_functions(r);
            break;
        }
        case 0x12: {
            alternate_select(r);
            break;
        }
        case 0x13: {
            write_string(r);
            break;
        }
        case 0x1a: {
            if (r->al == 0x00) {
                r->bl = crtc_port() == 0x3b4 ? DCC_VGA_MONO : DCC_VGA_COLOR;
                r->bh = 0;
            }
            r->al = 0x1a;
            break;
        }
        case 0x1b: {
            if (r->bx == 0) {
                state_information(r);
            }
            break;
        }
        case 0x4f: {
            vbe_int10(r);
            break;
        }
        default: {
            break;
        }
    }
}

void vgabios_init(void) {
    uint16_t cs = code_segment();
    set_vector(0x10, cs, (uint16_t)(uintptr_t)vgabios_int10_entry);
    pokeb(BDA, BDA_VIDEO_CONTROL, 0x60);
    pokeb(BDA, BDA_SWITCHES, 0xf9);
    pokeb(BDA, BDA_VGA_FLAGS, 0x51);
    pokeb(BDA, BDA_DCC, 0x08);
    pokew(BDA, BDA_CRTC, 0x3d4);
    set_mode(0x03);
}
