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
#include <stddef.h>
#include <stdbool.h>
#include <memory.h>
#include <core.h>
#include <handoff.h>
#include <lib/log.h>
#include <lib/spinlock.h>
#include <lib/time.h>
#include <sys/cpu.h>
#include <sys/mm.h>
#include <virt/gpt.h>
#include <virt/io.h>
#include <virt/mmio.h>
#include <virt/vga.h>

#define WINDOW_BASE 0xa0000
#define WINDOW_SIZE 0x20000
#define PLANE_SIZE 0x10000
#define PORT_BASE 0x3b0
#define PORT_COUNT 0x30

#define SEQ_COUNT 5
#define SEQ_CLOCKING 0x01
#define SEQ_MAP_MASK 0x02
#define SEQ_CHAR_MAP 0x03
#define SEQ_MEMORY_MODE 0x04
#define SEQ_CLOCKING_8DOT 0x01
#define SEQ_CLOCKING_HALF 0x08
#define SEQ_CLOCKING_OFF 0x20
#define SEQ_MEMORY_SEQUENTIAL 0x04
#define SEQ_MEMORY_CHAIN4 0x08

#define CRTC_COUNT 0x19
#define CRTC_HDE 0x01
#define CRTC_OVERFLOW 0x07
#define CRTC_MAX_SCAN 0x09
#define CRTC_CURSOR_START 0x0a
#define CRTC_CURSOR_END 0x0b
#define CRTC_START_HIGH 0x0c
#define CRTC_START_LOW 0x0d
#define CRTC_CURSOR_HIGH 0x0e
#define CRTC_CURSOR_LOW 0x0f
#define CRTC_PRESET 0x08
#define CRTC_VRE 0x11
#define CRTC_VDE 0x12
#define CRTC_OFFSET 0x13
#define CRTC_UNDERLINE 0x14
#define CRTC_MODE 0x17
#define CRTC_LINE_COMPARE 0x18
#define CRTC_VRE_PROTECT 0x80
#define CRTC_DOUBLE_SCAN 0x80
#define CRTC_CURSOR_OFF 0x20
#define CRTC_DWORD 0x40
#define CRTC_MODE_CMS 0x01
#define CRTC_MODE_SRS 0x02
#define CRTC_MODE_WRAP15 0x20
#define CRTC_MODE_BYTE 0x40

#define GC_COUNT 9
#define GC_SET_RESET 0x00
#define GC_ENABLE_SET_RESET 0x01
#define GC_COLOR_COMPARE 0x02
#define GC_ROTATE 0x03
#define GC_READ_MAP 0x04
#define GC_MODE 0x05
#define GC_MISC 0x06
#define GC_DONT_CARE 0x07
#define GC_BIT_MASK 0x08
#define GC_MODE_READ1 0x08
#define GC_MODE_INTERLEAVE 0x20
#define GC_MODE_256 0x40
#define GC_MISC_GRAPHICS 0x01

#define AC_COUNT 0x15
#define AC_MODE 0x10
#define AC_PLANE_ENABLE 0x12
#define AC_PANNING 0x13
#define AC_COLOR_SELECT 0x14
#define AC_INDEX_PAS 0x20
#define AC_MODE_LINE_GRAPHICS 0x04
#define AC_MODE_BLINK 0x08
#define AC_MODE_PAN_SPLIT 0x20
#define AC_MODE_P54 0x80

#define MISC_COLOR 0x01
#define MISC_LINES 0xc0
#define MISC_LINES_480 0xc0

// The display runs at 60 Hz with 480 lines, else at 70 Hz, and is in
// vertical retrace for the first part of each frame.
#define FRAME_NS_70 14285714u
#define FRAME_NS_60 16683217u
#define RETRACE_NS 600000u

// The cursor shows for 16 frames and hides for 16, characters with the blink
// attribute for 32 each.
#define CURSOR_FRAMES_SHIFT 4
#define BLINK_FRAMES_SHIFT 5

#define MAX_WIDTH 1024
#define MAX_HEIGHT 768
#define REFRESH_NS 20000000u

static struct {
    bool on;
    struct spinlock lock;
    uint8_t misc;
    uint8_t feature;
    uint8_t enable;
    uint8_t seq_index;
    uint8_t seq[SEQ_COUNT];
    uint8_t crtc_index;
    uint8_t crtc[CRTC_COUNT];
    uint8_t gc_index;
    uint8_t gc[GC_COUNT];
    uint8_t ac_index;
    bool ac_data_next;
    uint8_t ac[AC_COUNT];
    uint8_t pel_mask;
    uint8_t dac[256][3];
    uint8_t dac_read_index;
    uint8_t dac_write_index;
    uint8_t dac_step;
    bool dac_reading;
    uint8_t dac_latch[3];
    uint8_t latch[4];
    uint8_t status_toggle;
    uint8_t *planes[4];
    bool dirty;
} vga;

// The display as last drawn, and where on the framebuffer.
static struct {
    uint32_t *frame;
    uint32_t *shown;
    uint32_t width;
    uint32_t height;
    uint32_t x;
    uint32_t y;
    uint32_t w;
    uint32_t h;
    uint32_t *xmap;
    bool valid;
    uint64_t last;
    uint32_t blink;
    uint32_t border;
    // One scan line, with a character clock to spare for panning.
    uint32_t line[MAX_WIDTH + 2 * 9 * 2];
} out;

static uint8_t rotate_right(uint8_t value, unsigned count) {
    count &= 7;
    return (uint8_t)((value >> count) | (value << ((8 - count) & 7)));
}

static uint8_t logic(uint8_t value, uint8_t latch) {
    switch ((vga.gc[GC_ROTATE] >> 3) & 3) {
        case 1: {
            return value & latch;
        }
        case 2: {
            return value | latch;
        }
        case 3: {
            return value ^ latch;
        }
        default: {
            return value;
        }
    }
}

// The offset into the planes' address space of a CPU access, from the memory
// map the graphics controller selects; false where it decodes nothing.
static bool window_address(uint32_t offset, uint32_t *address) {
    switch ((vga.gc[GC_MISC] >> 2) & 3) {
        case 1: {
            *address = offset;
            return offset < 0x10000;
        }
        case 2: {
            *address = offset - 0x10000;
            return offset >= 0x10000 && offset < 0x18000;
        }
        case 3: {
            *address = offset - 0x18000;
            return offset >= 0x18000;
        }
        default: {
            *address = offset;
            return true;
        }
    }
}

static void mem_write(uint32_t offset, uint8_t value) {
    uint32_t address;
    if (!window_address(offset, &address)) {
        return;
    }
    uint8_t planes = vga.seq[SEQ_MAP_MASK] & 0x0f;
    if (vga.seq[SEQ_MEMORY_MODE] & SEQ_MEMORY_CHAIN4) {
        planes &= (uint8_t)(1u << (address & 3));
        address &= ~3u;
    } else if (!(vga.seq[SEQ_MEMORY_MODE] & SEQ_MEMORY_SEQUENTIAL)) {
        planes &= (address & 1) ? 0x0a : 0x05;
        address &= ~1u;
    }
    address &= PLANE_SIZE - 1;

    uint8_t bit_mask = vga.gc[GC_BIT_MASK];
    uint8_t rotated = rotate_right(value, vga.gc[GC_ROTATE]);
    for (unsigned p = 0; p < 4; p++) {
        if (!(planes & (1u << p))) {
            continue;
        }
        uint8_t latch = vga.latch[p];
        uint8_t set_reset = (vga.gc[GC_SET_RESET] & (1u << p)) ? 0xff : 0x00;
        uint8_t result;
        switch (vga.gc[GC_MODE] & 3) {
            case 0: {
                uint8_t v = (vga.gc[GC_ENABLE_SET_RESET] & (1u << p)) ? set_reset : rotated;
                result = (uint8_t)((logic(v, latch) & bit_mask) | (latch & ~bit_mask));
                break;
            }
            case 1: {
                result = latch;
                break;
            }
            case 2: {
                uint8_t v = (value & (1u << p)) ? 0xff : 0x00;
                result = (uint8_t)((logic(v, latch) & bit_mask) | (latch & ~bit_mask));
                break;
            }
            default: {
                uint8_t mask = rotated & bit_mask;
                result = (uint8_t)((logic(set_reset, latch) & mask) | (latch & ~mask));
                break;
            }
        }
        vga.planes[p][address] = result;
    }
    vga.dirty = true;
}

static uint8_t mem_read(uint32_t offset) {
    uint32_t address;
    if (!window_address(offset, &address)) {
        return 0xff;
    }
    unsigned plane;
    if (vga.seq[SEQ_MEMORY_MODE] & SEQ_MEMORY_CHAIN4) {
        plane = address & 3;
        address &= ~3u;
    } else if (!(vga.seq[SEQ_MEMORY_MODE] & SEQ_MEMORY_SEQUENTIAL)) {
        plane = (vga.gc[GC_READ_MAP] & 2) | (address & 1);
        address &= ~1u;
    } else {
        plane = vga.gc[GC_READ_MAP] & 3;
    }
    address &= PLANE_SIZE - 1;

    for (unsigned p = 0; p < 4; p++) {
        vga.latch[p] = vga.planes[p][address];
    }
    if (!(vga.gc[GC_MODE] & GC_MODE_READ1)) {
        return vga.latch[plane];
    }
    // Colour compare: ones where every plane that counts matches.
    uint8_t differ = 0;
    for (unsigned p = 0; p < 4; p++) {
        if (vga.gc[GC_DONT_CARE] & (1u << p)) {
            differ |= vga.latch[p] ^ ((vga.gc[GC_COLOR_COMPARE] & (1u << p)) ? 0xff : 0x00);
        }
    }
    return (uint8_t)~differ;
}

static uint64_t window_read(struct vcpu *v, void *ctx, uint64_t offset, unsigned size) {
    (void)v;
    (void)ctx;
    uint64_t value = 0;
    spinlock_acquire(&vga.lock);
    for (unsigned i = 0; i < size; i++) {
        value |= (uint64_t)mem_read((uint32_t)offset + i) << (8 * i);
    }
    spinlock_release(&vga.lock);
    return value;
}

static void window_write(struct vcpu *v, void *ctx, uint64_t offset, unsigned size,
                         uint64_t value) {
    (void)v;
    (void)ctx;
    spinlock_acquire(&vga.lock);
    for (unsigned i = 0; i < size; i++) {
        mem_write((uint32_t)offset + i, (uint8_t)(value >> (8 * i)));
    }
    spinlock_release(&vga.lock);
}

static const struct mmio_ops window_ops = {window_read, window_write, false};

// The CRT controller's registers answer at 3Bxh or 3Dxh, as the
// miscellaneous output register selects.
static bool port_decoded(uint16_t port) {
    bool color = vga.misc & MISC_COLOR;
    if (port < 0x3c0) {
        return !color;
    }
    if (port >= 0x3d0) {
        return color;
    }
    return true;
}

static uint64_t frame_ns(void) {
    return (vga.misc & MISC_LINES) == MISC_LINES_480 ? FRAME_NS_60 : FRAME_NS_70;
}

static uint8_t input_status1(void) {
    vga.ac_data_next = false;
    if (time_ns() % frame_ns() < RETRACE_NS) {
        return 0x09;
    }
    // Display enable flips, as with each line's horizontal blanking.
    vga.status_toggle ^= 1;
    return vga.status_toggle;
}

static uint8_t port_read(uint16_t port) {
    switch (port) {
        case 0x3c0: {
            return vga.ac_index;
        }
        case 0x3c1: {
            uint8_t index = vga.ac_index & 0x1f;
            return index < AC_COUNT ? vga.ac[index] : 0;
        }
        case 0x3c2: {
            return 0;
        }
        case 0x3c3: {
            return vga.enable;
        }
        case 0x3c4: {
            return vga.seq_index;
        }
        case 0x3c5: {
            return vga.seq_index < SEQ_COUNT ? vga.seq[vga.seq_index] : 0;
        }
        case 0x3c6: {
            return vga.pel_mask;
        }
        case 0x3c7: {
            return vga.dac_reading ? 0x03 : 0x00;
        }
        case 0x3c8: {
            return vga.dac_write_index;
        }
        case 0x3c9: {
            uint8_t value = vga.dac[vga.dac_read_index][vga.dac_step];
            if (++vga.dac_step == 3) {
                vga.dac_step = 0;
                vga.dac_read_index++;
            }
            return value;
        }
        case 0x3ca: {
            return vga.feature;
        }
        case 0x3cc: {
            return vga.misc;
        }
        case 0x3ce: {
            return vga.gc_index;
        }
        case 0x3cf: {
            return vga.gc_index < GC_COUNT ? vga.gc[vga.gc_index] : 0;
        }
        case 0x3b4:
        case 0x3d4: {
            return vga.crtc_index;
        }
        case 0x3b5:
        case 0x3d5: {
            return vga.crtc_index < CRTC_COUNT ? vga.crtc[vga.crtc_index] : 0;
        }
        case 0x3ba:
        case 0x3da: {
            return input_status1();
        }
        default: {
            return 0xff;
        }
    }
}

static void port_write(uint16_t port, uint8_t value) {
    switch (port) {
        case 0x3c0: {
            if (!vga.ac_data_next) {
                vga.ac_index = value & 0x3f;
            } else {
                uint8_t index = vga.ac_index & 0x1f;
                // The palette takes writes only while the display does not
                // read it.
                if (index < AC_COUNT && (index >= 0x10 || !(vga.ac_index & AC_INDEX_PAS))) {
                    vga.ac[index] = value;
                }
            }
            vga.ac_data_next = !vga.ac_data_next;
            break;
        }
        case 0x3c2: {
            vga.misc = value;
            break;
        }
        case 0x3c3: {
            vga.enable = value;
            break;
        }
        case 0x3c4: {
            vga.seq_index = value & 0x07;
            break;
        }
        case 0x3c5: {
            if (vga.seq_index < SEQ_COUNT) {
                vga.seq[vga.seq_index] = value;
            }
            break;
        }
        case 0x3c6: {
            vga.pel_mask = value;
            break;
        }
        case 0x3c7: {
            vga.dac_read_index = value;
            vga.dac_step = 0;
            vga.dac_reading = true;
            break;
        }
        case 0x3c8: {
            vga.dac_write_index = value;
            vga.dac_step = 0;
            vga.dac_reading = false;
            break;
        }
        case 0x3c9: {
            vga.dac_latch[vga.dac_step] = value & 0x3f;
            if (++vga.dac_step == 3) {
                memcpy(vga.dac[vga.dac_write_index], vga.dac_latch, 3);
                vga.dac_step = 0;
                vga.dac_write_index++;
            }
            break;
        }
        case 0x3ce: {
            vga.gc_index = value & 0x0f;
            break;
        }
        case 0x3cf: {
            if (vga.gc_index < GC_COUNT) {
                vga.gc[vga.gc_index] = value;
            }
            break;
        }
        case 0x3b4:
        case 0x3d4: {
            vga.crtc_index = value & 0x3f;
            break;
        }
        case 0x3b5:
        case 0x3d5: {
            uint8_t index = vga.crtc_index;
            if (index >= CRTC_COUNT) {
                break;
            }
            // Protection leaves registers 0-7 alone, but the line compare
            // bit of the overflow register.
            if ((vga.crtc[CRTC_VRE] & CRTC_VRE_PROTECT) && index <= CRTC_OVERFLOW) {
                if (index == CRTC_OVERFLOW) {
                    vga.crtc[index] = (uint8_t)((vga.crtc[index] & ~0x10) | (value & 0x10));
                }
                break;
            }
            vga.crtc[index] = value;
            break;
        }
        case 0x3ba:
        case 0x3da: {
            vga.feature = value;
            break;
        }
        default: {
            break;
        }
    }
    vga.dirty = true;
}

static void ports_access(struct vcpu *v, void *ctx, uint16_t offset, unsigned size, bool in,
                         uint32_t *value) {
    (void)v;
    (void)ctx;
    spinlock_acquire(&vga.lock);
    uint16_t port = (uint16_t)(PORT_BASE + offset);
    if (in) {
        uint32_t result = 0;
        for (unsigned i = 0; i < size; i++) {
            uint16_t p = (uint16_t)(port + i);
            bool here = p < PORT_BASE + PORT_COUNT && port_decoded(p);
            result |= (uint32_t)(here ? port_read(p) : 0xff) << (8 * i);
        }
        *value = result;
    } else {
        for (unsigned i = 0; i < size; i++) {
            uint16_t p = (uint16_t)(port + i);
            if (p < PORT_BASE + PORT_COUNT && port_decoded(p)) {
                port_write(p, (uint8_t)(*value >> (8 * i)));
            }
        }
    }
    spinlock_release(&vga.lock);
}

static uint32_t expand6(uint8_t v) {
    v &= 0x3f;
    return (uint32_t)((v << 2) | (v >> 4));
}

static uint32_t dac_rgb(uint8_t index) {
    const uint8_t *c = vga.dac[index & vga.pel_mask];
    return expand6(c[0]) << 16 | expand6(c[1]) << 8 | expand6(c[2]);
}

// A 4-bit colour through the attribute controller's palette, as a DAC index.
static uint8_t ac_color(uint8_t color) {
    uint8_t p = vga.ac[color & 0x0f];
    uint8_t select = vga.ac[AC_COLOR_SELECT];
    if (vga.ac[AC_MODE] & AC_MODE_P54) {
        p = (uint8_t)((p & 0x0f) | ((select & 0x03) << 4));
    }
    return (uint8_t)((p & 0x3f) | ((select & 0x0c) << 4));
}

// The overscan colour register's DAC index, as the colour select register
// extends it.
static uint8_t ac_color_border(void) {
    return (uint8_t)((vga.ac[0x11] & 0x3f) | ((vga.ac[AC_COLOR_SELECT] & 0x0c) << 4));
}

// Where the CRT controller fetches a character clock's bytes from.
static uint32_t crtc_address(uint32_t ma, unsigned row_scan) {
    uint32_t a;
    if (vga.crtc[CRTC_UNDERLINE] & CRTC_DWORD) {
        a = (ma << 2) | ((ma >> 14) & 3);
    } else if (!(vga.crtc[CRTC_MODE] & CRTC_MODE_BYTE)) {
        uint32_t bit = (vga.crtc[CRTC_MODE] & CRTC_MODE_WRAP15) ? (ma >> 15) & 1 : (ma >> 13) & 1;
        a = (ma << 1) | bit;
    } else {
        a = ma;
    }
    if (!(vga.crtc[CRTC_MODE] & CRTC_MODE_CMS)) {
        a = (a & ~0x2000u) | ((row_scan & 1u) << 13);
    }
    if (!(vga.crtc[CRTC_MODE] & CRTC_MODE_SRS)) {
        a = (a & ~0x4000u) | ((row_scan & 2u) << 13);
    }
    return a & (PLANE_SIZE - 1);
}

// The character map an attribute selects, map A where its bit 3 is set.
static uint32_t font_base(uint8_t attribute) {
    uint8_t select = vga.seq[SEQ_CHAR_MAP];
    unsigned map;
    if (attribute & 0x08) {
        map = ((select >> 3) & 4) | ((select >> 2) & 3);
    } else {
        map = ((select >> 2) & 4) | (select & 3);
    }
    return (map & 3) * 0x4000u + (map >> 2) * 0x2000u;
}

static void put(uint32_t *line, uint32_t *x, uint32_t width, uint32_t rgb, unsigned count) {
    for (unsigned i = 0; i < count && *x < width; i++) {
        line[(*x)++] = rgb;
    }
}

static void text_line(uint32_t *line, uint32_t width, uint32_t columns, uint32_t ma,
                      unsigned row_scan, unsigned dots, unsigned repeat, bool cursor_on,
                      bool blink_on) {
    uint32_t cursor = (uint32_t)vga.crtc[CRTC_CURSOR_HIGH] << 8 | vga.crtc[CRTC_CURSOR_LOW];
    unsigned cursor_start = vga.crtc[CRTC_CURSOR_START] & 0x1f;
    unsigned cursor_end = vga.crtc[CRTC_CURSOR_END] & 0x1f;
    bool cursor_row = !(vga.crtc[CRTC_CURSOR_START] & CRTC_CURSOR_OFF) && cursor_on
                      && row_scan >= cursor_start && row_scan <= cursor_end;
    bool underline_row = row_scan == (vga.crtc[CRTC_UNDERLINE] & 0x1fu);
    bool blink = vga.ac[AC_MODE] & AC_MODE_BLINK;
    uint32_t x = 0;
    for (uint32_t c = 0; c < columns; c++) {
        uint32_t a = crtc_address(ma + c, row_scan);
        uint8_t ch = vga.planes[0][a];
        uint8_t attribute = vga.planes[1][a];
        uint8_t glyph = vga.planes[2][(font_base(attribute) + ch * 32u + row_scan) & (PLANE_SIZE - 1)];
        uint8_t fg = attribute & 0x0f;
        uint8_t bg = attribute >> 4;
        // The underline of a monochrome display's attributes, which colour
        // modes keep below the character cell, and the cursor span the
        // whole cell, ninth dot included.
        bool whole = false;
        if (underline_row && (attribute & 0x77) == 0x01) {
            glyph = 0xff;
            whole = true;
        }
        if (blink) {
            bg &= 7;
            if ((attribute & 0x80) && !blink_on) {
                glyph = 0;
                whole = false;
            }
        }
        if (cursor_row && ma + c == cursor) {
            glyph = 0xff;
            whole = true;
        }
        uint32_t fg_rgb = dac_rgb(ac_color(fg));
        uint32_t bg_rgb = dac_rgb(ac_color(bg));
        for (unsigned k = 0; k < dots; k++) {
            bool on;
            if (k < 8) {
                on = (glyph >> (7 - k)) & 1;
            } else {
                // Line graphics characters continue into the ninth column.
                on = whole
                     || ((vga.ac[AC_MODE] & AC_MODE_LINE_GRAPHICS) && ch >= 0xc0 && ch <= 0xdf
                         && (glyph & 1));
            }
            put(line, &x, width, on ? fg_rgb : bg_rgb, repeat);
        }
    }
    while (x < width) {
        line[x++] = 0;
    }
}

static void graphics_line(uint32_t *line, uint32_t width, uint32_t columns, uint32_t ma,
                          unsigned row_scan, unsigned repeat) {
    uint8_t mode = vga.gc[GC_MODE];
    uint8_t enable = vga.ac[AC_PLANE_ENABLE] & 0x0f;
    uint32_t x = 0;
    for (uint32_t c = 0; c < columns; c++) {
        uint32_t a = crtc_address(ma + c, row_scan);
        uint8_t b[4] = {vga.planes[0][a], vga.planes[1][a], vga.planes[2][a], vga.planes[3][a]};
        if (mode & GC_MODE_256) {
            for (unsigned k = 0; k < 4; k++) {
                put(line, &x, width, dac_rgb(b[k]), 2 * repeat);
            }
        } else if (mode & GC_MODE_INTERLEAVE) {
            for (unsigned k = 0; k < 8; k++) {
                unsigned shift = 6 - 2 * (k & 3);
                uint8_t low = k < 4 ? b[0] : b[1];
                uint8_t high = k < 4 ? b[2] : b[3];
                uint8_t color = (uint8_t)(((low >> shift) & 3) | (((high >> shift) & 3) << 2));
                put(line, &x, width, dac_rgb(ac_color(color & enable)), repeat);
            }
        } else {
            for (unsigned k = 0; k < 8; k++) {
                unsigned shift = 7 - k;
                uint8_t color = (uint8_t)(((b[0] >> shift) & 1) | (((b[1] >> shift) & 1) << 1)
                                          | (((b[2] >> shift) & 1) << 2)
                                          | (((b[3] >> shift) & 1) << 3));
                put(line, &x, width, dac_rgb(ac_color(color & enable)), repeat);
            }
        }
    }
    while (x < width) {
        line[x++] = 0;
    }
}

// How many dots the attribute controller's pel panning shifts the display.
static unsigned pel_panning(unsigned dots) {
    unsigned pan = vga.ac[AC_PANNING] & 0x0f;
    if (dots == 9) {
        return pan >= 8 ? 0 : pan + 1;
    }
    return pan & 7;
}

// Renders the display as the CRT controller would scan it out.
static void make_frame(uint32_t *width_out, uint32_t *height_out, bool cursor_on, bool blink_on) {
    bool graphics = vga.gc[GC_MISC] & GC_MISC_GRAPHICS;
    unsigned dots = (graphics || (vga.seq[SEQ_CLOCKING] & SEQ_CLOCKING_8DOT)) ? 8 : 9;
    unsigned repeat = (vga.seq[SEQ_CLOCKING] & SEQ_CLOCKING_HALF) ? 2 : 1;
    uint32_t width = (vga.crtc[CRTC_HDE] + 1u) * dots * repeat;
    uint32_t height = (vga.crtc[CRTC_VDE] | ((vga.crtc[CRTC_OVERFLOW] & 0x02u) << 7)
                       | ((vga.crtc[CRTC_OVERFLOW] & 0x40u) << 3)) + 1u;
    if (width > MAX_WIDTH) {
        width = MAX_WIDTH;
    }
    if (height > MAX_HEIGHT) {
        height = MAX_HEIGHT;
    }
    *width_out = width;
    *height_out = height;

    if (!(vga.ac_index & AC_INDEX_PAS) || (vga.seq[SEQ_CLOCKING] & SEQ_CLOCKING_OFF)) {
        memset(out.frame, 0, width * height * sizeof(uint32_t));
        return;
    }

    unsigned max_scan = vga.crtc[CRTC_MAX_SCAN] & 0x1f;
    bool double_scan = vga.crtc[CRTC_MAX_SCAN] & CRTC_DOUBLE_SCAN;
    uint32_t line_compare = vga.crtc[CRTC_LINE_COMPARE] | ((vga.crtc[CRTC_OVERFLOW] & 0x10u) << 4)
                            | ((vga.crtc[CRTC_MAX_SCAN] & 0x40u) << 3);
    // The start address, with the CRT controller's byte panning, and the
    // first row's first scan line.
    uint32_t ma = ((uint32_t)vga.crtc[CRTC_START_HIGH] << 8 | vga.crtc[CRTC_START_LOW])
                  + ((vga.crtc[CRTC_PRESET] >> 5) & 3u);
    uint32_t row_step = vga.crtc[CRTC_OFFSET] * 2u;
    unsigned row_scan = vga.crtc[CRTC_PRESET] & 0x1fu;
    uint32_t columns = vga.crtc[CRTC_HDE] + 1u;
    unsigned pan = pel_panning(dots) * repeat;
    uint32_t span = width + 2 * dots * repeat;
    bool split = false;
    for (uint32_t y = 0; y < height; y++) {
        if (y == line_compare) {
            ma = 0;
            row_scan = 0;
            split = true;
        }
        // The screen below a split can be kept from panning.
        unsigned shift = split && (vga.ac[AC_MODE] & AC_MODE_PAN_SPLIT) ? 0 : pan;
        if (graphics) {
            graphics_line(out.line, span, columns + 1, ma, row_scan, repeat);
        } else {
            text_line(out.line, span, columns + 1, ma, row_scan, dots, repeat, cursor_on,
                      blink_on);
        }
        memcpy(out.frame + y * width, out.line + shift, width * sizeof(uint32_t));
        if (!double_scan || (y & 1)) {
            if (row_scan >= max_scan) {
                row_scan = 0;
                ma += row_step;
            } else {
                row_scan++;
            }
        }
    }
}

static uint32_t channel(uint32_t value, uint8_t size, uint8_t shift) {
    if (size == 0) {
        return 0;
    }
    if (size < 8) {
        value >>= 8 - size;
    }
    return value << shift;
}

static uint32_t fb_pixel(uint32_t rgb) {
    const struct handoff_framebuffer *fb = &core_handoff->video.framebuffer;
    return channel((rgb >> 16) & 0xff, fb->red_size, fb->red_shift)
           | channel((rgb >> 8) & 0xff, fb->green_size, fb->green_shift)
           | channel(rgb & 0xff, fb->blue_size, fb->blue_shift);
}

static void fb_put(uint8_t *row, uint32_t x, uint32_t pixel, uint8_t bpp) {
    switch (bpp) {
        case 32: {
            ((volatile uint32_t *)row)[x] = pixel;
            break;
        }
        case 24: {
            volatile uint8_t *p = row + x * 3;
            p[0] = (uint8_t)pixel;
            p[1] = (uint8_t)(pixel >> 8);
            p[2] = (uint8_t)(pixel >> 16);
            break;
        }
        default: {
            ((volatile uint16_t *)row)[x] = (uint16_t)pixel;
            break;
        }
    }
}

// The framebuffer's rectangle the display fills, as the configuration asks.
static void place(uint32_t width, uint32_t height) {
    const struct handoff *h = core_handoff;
    uint32_t fw = h->video.framebuffer.width;
    uint32_t fh = h->video.framebuffer.height;
    uint64_t w = fw;
    uint64_t hgt = fh;
    switch (h->config.vga_scaling) {
        case HANDOFF_VGA_SCALING_ASPECT: {
            // As a VGA monitor shows every mode, at 4:3.
            if ((uint64_t)fw * 3 >= (uint64_t)fh * 4) {
                w = (uint64_t)fh * 4 / 3;
            } else {
                hgt = (uint64_t)fw * 3 / 4;
            }
            break;
        }
        case HANDOFF_VGA_SCALING_CENTER: {
            w = width;
            hgt = height;
            break;
        }
        case HANDOFF_VGA_SCALING_MANUAL: {
            unsigned scale = h->config.vga_scale != 0 ? h->config.vga_scale : 1;
            w = (uint64_t)width * scale;
            hgt = (uint64_t)height * scale;
            break;
        }
        case HANDOFF_VGA_SCALING_INTEGER: {
            uint32_t scale = fw / width < fh / height ? fw / width : fh / height;
            if (scale == 0) {
                scale = 1;
            }
            w = (uint64_t)width * scale;
            hgt = (uint64_t)height * scale;
            break;
        }
        default: {
            break;
        }
    }
    if (w > fw) {
        w = fw;
    }
    if (hgt > fh) {
        hgt = fh;
    }
    out.w = (uint32_t)w;
    out.h = (uint32_t)hgt;
    out.x = (fw - out.w) / 2;
    out.y = (fh - out.h) / 2;
    for (uint32_t x = 0; x < out.w; x++) {
        out.xmap[x] = (uint32_t)((uint64_t)x * width / out.w);
    }
}

// Around the display, the overscan colour, as a monitor shows it.
static void fb_clear(uint32_t rgb) {
    const struct handoff_framebuffer *fb = &core_handoff->video.framebuffer;
    uint32_t black = fb_pixel(rgb);
    for (uint32_t y = 0; y < fb->height; y++) {
        uint8_t *row = (uint8_t *)(uintptr_t)fb->base + (uint64_t)y * fb->pitch;
        for (uint32_t x = 0; x < fb->width; x++) {
            fb_put(row, x, black, fb->bpp);
        }
    }
}

// Copies the lines that changed since the last time to the framebuffer.
static void draw(uint32_t width, uint32_t height, uint32_t border) {
    const struct handoff_framebuffer *fb = &core_handoff->video.framebuffer;
    bool all = !out.valid || width != out.width || height != out.height || border != out.border;
    if (all) {
        out.width = width;
        out.height = height;
        out.border = border;
        place(width, height);
        fb_clear(border);
        out.valid = true;
    }

    for (uint32_t sy = 0; sy < height; sy++) {
        const uint32_t *now = out.frame + sy * width;
        uint32_t *was = out.shown + sy * width;
        uint32_t first = 0;
        uint32_t last = width;
        if (!all) {
            while (first < width && now[first] == was[first]) {
                first++;
            }
            if (first == width) {
                continue;
            }
            while (last > first && now[last - 1] == was[last - 1]) {
                last--;
            }
        }
        memcpy(was + first, now + first, (last - first) * sizeof(uint32_t));

        // The destination rows and columns these source pixels cover.
        uint32_t y0 = (uint32_t)(((uint64_t)sy * out.h + height - 1) / height);
        uint32_t y1 = (uint32_t)(((uint64_t)(sy + 1) * out.h + height - 1) / height);
        uint32_t x0 = (uint32_t)(((uint64_t)first * out.w + width - 1) / width);
        uint32_t x1 = (uint32_t)(((uint64_t)last * out.w + width - 1) / width);
        for (uint32_t y = y0; y < y1 && y < out.h; y++) {
            uint8_t *row = (uint8_t *)(uintptr_t)fb->base + (uint64_t)(out.y + y) * fb->pitch;
            uint32_t source = UINT32_MAX;
            uint32_t pixel = 0;
            for (uint32_t x = x0; x < x1 && x < out.w; x++) {
                uint32_t rgb = now[out.xmap[x]];
                if (rgb != source) {
                    source = rgb;
                    pixel = fb_pixel(rgb);
                }
                fb_put(row, out.x + x, pixel, fb->bpp);
            }
        }
    }
}

void vga_refresh(void) {
    if (!vga.on || !this_cpu()->bsp) {
        return;
    }
    uint64_t now = time_ns();
    if (now - out.last < REFRESH_NS) {
        return;
    }
    out.last = now;
    uint64_t frames = now / frame_ns();
    bool cursor_on = !((frames >> CURSOR_FRAMES_SHIFT) & 1);
    bool blink_on = !((frames >> BLINK_FRAMES_SHIFT) & 1);
    uint32_t blink = (cursor_on ? 1u : 0u) | (blink_on ? 2u : 0u);
    if (!vga.dirty && blink == out.blink && out.valid) {
        return;
    }
    out.blink = blink;

    uint32_t width;
    uint32_t height;
    spinlock_acquire(&vga.lock);
    vga.dirty = false;
    make_frame(&width, &height, cursor_on, blink_on);
    uint32_t border = dac_rgb(ac_color_border());
    spinlock_release(&vga.lock);
    draw(width, height, border);
}

bool vga_emulated(void) {
    return vga.on;
}

static const char *const scaling_names[] = {"stretched", "kept at 4:3", "centred", "scaled",
                                            "scaled sharply"};

bool vga_init(void) {
    const struct handoff *h = core_handoff;
    switch (h->config.vga_emulation) {
        case HANDOFF_VGA_EMULATION_NEVER: {
            return false;
        }
        case HANDOFF_VGA_EMULATION_ALWAYS: {
            break;
        }
        default: {
            if (h->video.vbios_present) {
                return false;
            }
            break;
        }
    }
    const struct handoff_framebuffer *fb = &h->video.framebuffer;
    if (!fb->present || (fb->bpp != 32 && fb->bpp != 24 && fb->bpp != 16)) {
        printf("No framebuffer to show an emulated VGA on\n");
        return false;
    }

    for (unsigned p = 0; p < 4; p++) {
        vga.planes[p] = page_alloc(PLANE_SIZE / PAGE_SIZE);
        memset(vga.planes[p], 0, PLANE_SIZE);
    }
    size_t frame_pages = (MAX_WIDTH * MAX_HEIGHT * sizeof(uint32_t) + PAGE_SIZE - 1) / PAGE_SIZE;
    out.frame = page_alloc(frame_pages);
    out.shown = page_alloc(frame_pages);
    out.xmap = page_alloc((fb->width * sizeof(uint32_t) + PAGE_SIZE - 1) / PAGE_SIZE);
    mm_write_combining(fb->base, (uint64_t)fb->pitch * fb->height);
    vga.pel_mask = 0xff;
    vga.misc = MISC_COLOR;

    struct io_handler ports = {PORT_BASE, PORT_COUNT, ports_access, NULL};
    io_register(&ports);
    mmio_register(WINDOW_BASE, WINDOW_SIZE, &window_ops, NULL);
    gpt_map(WINDOW_BASE, 0, WINDOW_SIZE, 0, 0);
    gpt_flush();
    vga.on = true;

    unsigned scaling = h->config.vga_scaling < 5 ? h->config.vga_scaling : 0;
    printf("Emulating a VGA, %s on the %ux%u framebuffer at %#llx\n", scaling_names[scaling],
           fb->width, fb->height, (unsigned long long)fb->base);
    return true;
}
