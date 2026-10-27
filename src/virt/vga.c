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
#include <virt/layout.h>
#include <virt/mmio.h>
#include <virt/vcpu.h>
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
// The lines of a turned display written to the framebuffer together.
#define BAND_LINES 32

// The display keeps track of the guest's writes to the planes in chunks of
// 1 << CHUNK_SHIFT bytes of plane address.
#define CHUNK_SHIFT 6
#define CHUNKS (PLANE_SIZE >> CHUNK_SHIFT)

// The Bochs display interface's registers, at an index port and a data port,
// with QEMU's include/hw/display/bochs-vbe.h values and semantics.
#define DISPI_PORT 0x1ce
#define DISPI_PORT_COUNT 3
#define DISPI_ID 0x0
#define DISPI_XRES 0x1
#define DISPI_YRES 0x2
#define DISPI_BPP 0x3
#define DISPI_ENABLE 0x4
#define DISPI_BANK 0x5
#define DISPI_VIRT_WIDTH 0x6
#define DISPI_VIRT_HEIGHT 0x7
#define DISPI_X_OFFSET 0x8
#define DISPI_Y_OFFSET 0x9
#define DISPI_REG_COUNT 0xa
#define DISPI_VIDEO_MEMORY_64K 0xa
#define DISPI_ID0 0xb0c0
#define DISPI_ID5 0xb0c5
#define DISPI_ENABLED 0x01
#define DISPI_GETCAPS 0x02
#define DISPI_8BIT_DAC 0x20
#define DISPI_LFB_ENABLED 0x40
#define DISPI_NOCLEARMEM 0x80
#define DISPI_MAX_XRES 16000
#define DISPI_MAX_YRES 12000
#define DISPI_MAX_BPP 32

#define NO_BANK UINT32_MAX

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
    // The chunks of plane address the guest wrote since the display last
    // showed them, and whether it wrote plane 2, which holds text modes' fonts.
    uint64_t changed[CHUNKS / 64];
    bool font_changed;
    // The DAC takes 8 bits per primary, as the Bochs interface can select.
    bool dac8;
    bool palette_dirty;
} vga;

// The Bochs display interface, on video memory of its own, which the guest
// finds as reserved memory.
static struct {
    bool present;
    uint8_t *vram;
    uint64_t vram_phys;
    uint64_t vram_size;
    uint16_t index;
    uint16_t regs[DISPI_REG_COUNT];
    uint32_t line_bytes;
    uint32_t start;
    // The bank the window at A0000h shows, while the interface is enabled.
    uint32_t window_bank;
    // Where the guest finds the video memory as its linear framebuffer, 0 if
    // nowhere.
    uint64_t lfb;
    // While the guest's mode is the firmware framebuffer's own, shown from the
    // start of video memory, the first direct_size bytes of video memory are
    // that framebuffer, which nothing then has to copy to. Panning or flipping
    // pages ends that until the next mode set.
    uint64_t direct_size;
    bool direct;
    bool panned;
} vbe;

// The registers and palette a frame shows the planes through.
struct display_state {
    uint8_t clocking;
    uint8_t char_map;
    uint8_t gc_misc;
    uint8_t gc_mode;
    uint8_t crtc[CRTC_COUNT];
    uint8_t ac[AC_COUNT];
    uint8_t ac_pas;
    uint8_t pel_mask;
    uint8_t dac8;
    uint8_t dac[256][3];
};

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
    // The registers and palette the frame was last made with, and which of
    // its scan lines were made again then.
    struct display_state state;
    bool made[MAX_HEIGHT];
    // One scan line, with a character clock to spare for panning.
    uint32_t line[MAX_WIDTH + 2 * 9 * 2];
    // What the Bochs interface last showed: each line's hash, and the palette.
    bool vbe_valid;
    uint16_t vbe_bpp;
    uint32_t vbe_line_bytes;
    uint32_t vbe_start;
    uint64_t *vbe_hashes;
    uint32_t *vbe_line;
    uint32_t vbe_palette[256];
    // One line of the display, in the framebuffer's pixel format.
    uint32_t *fb_line;
    // Lines of a turned display, which run down the framebuffer's columns:
    // band_count lines of out.w pixels, from line band_y on, of which pixels
    // band_x0 to band_x1 changed.
    uint32_t *band;
    uint32_t band_y;
    uint32_t band_count;
    uint32_t band_x0;
    uint32_t band_x1;
    // A redraw under way, which later calls carry on with: whether it is the
    // Bochs interface's, whether it redraws every line, and its next line.
    bool busy;
    bool busy_vbe;
    bool redraw_all;
    uint32_t next_line;
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
    if (planes != 0) {
        uint32_t chunk = address >> CHUNK_SHIFT;
        vga.changed[chunk / 64] |= 1ull << (chunk % 64);
        vga.font_changed |= (planes & 4) != 0;
    }

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

static bool vbe_on(void) {
    return vbe.present && (vbe.regs[DISPI_ENABLE] & DISPI_ENABLED);
}

// Video memory as the guest reaches it now.
static uint8_t *vram_at(uint64_t offset) {
    if (vbe.direct && offset < vbe.direct_size) {
        return (uint8_t *)(uintptr_t)core_handoff->video.framebuffer.base + offset;
    }
    return vbe.vram + offset;
}

static void vram_clear(uint64_t size) {
    uint64_t done = 0;
    if (vbe.direct) {
        done = size < vbe.direct_size ? size : vbe.direct_size;
        memset(vram_at(0), 0, done);
    }
    memset(vbe.vram + done, 0, size - done);
}

// Maps size bytes of video memory from offset on at gpa, as the guest reaches
// them now.
static void map_vram(uint64_t gpa, uint64_t offset, uint64_t size) {
    if (vbe.direct && offset < vbe.direct_size) {
        uint64_t n = vbe.direct_size - offset < size ? vbe.direct_size - offset : size;
        gpt_map(gpa, core_handoff->video.framebuffer.base + offset, n, GPT_RWX, MT_WC);
        gpa += n;
        offset += n;
        size -= n;
    }
    if (size != 0) {
        gpt_map(gpa, vbe.vram_phys + offset, size, GPT_RWX, MT_WB);
    }
}

// While the Bochs interface is on, the window's first 64 KiB show a bank of
// its video memory, normally mapped there directly, and the rest nothing.
static uint8_t *vbe_window_byte(uint32_t offset) {
    uint64_t at = ((uint64_t)vbe.regs[DISPI_BANK] << 16) + offset;
    return offset < PLANE_SIZE && at < vbe.vram_size ? vram_at(at) : NULL;
}

static uint64_t window_read(struct vcpu *v, void *ctx, uint64_t offset, unsigned size) {
    (void)v;
    (void)ctx;
    uint64_t value = 0;
    spinlock_acquire(&vga.lock);
    for (unsigned i = 0; i < size; i++) {
        uint8_t byte;
        if (vbe_on()) {
            uint8_t *p = vbe_window_byte((uint32_t)offset + i);
            byte = p != NULL ? *p : 0xff;
        } else {
            byte = mem_read((uint32_t)offset + i);
        }
        value |= (uint64_t)byte << (8 * i);
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
        uint8_t byte = (uint8_t)(value >> (8 * i));
        if (vbe_on()) {
            uint8_t *p = vbe_window_byte((uint32_t)offset + i);
            if (p != NULL) {
                *p = byte;
            }
        } else {
            mem_write((uint32_t)offset + i, byte);
        }
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
            vga.palette_dirty = true;
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
            vga.dac_latch[vga.dac_step] = vga.dac8 ? value : value & 0x3f;
            if (++vga.dac_step == 3) {
                memcpy(vga.dac[vga.dac_write_index], vga.dac_latch, 3);
                vga.dac_step = 0;
                vga.dac_write_index++;
                vga.palette_dirty = true;
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

static unsigned dispi_bits(void) {
    return vbe.regs[DISPI_BPP] == 15 ? 16 : vbe.regs[DISPI_BPP];
}

// The registers brought back in range, and the scan line and display start
// that follow from them, as QEMU's vbe_fixup_regs() does.
static void dispi_fixup(void) {
    uint16_t *r = vbe.regs;
    if (!(r[DISPI_ENABLE] & DISPI_ENABLED)) {
        return;
    }
    switch (r[DISPI_BPP]) {
        case 4:
        case 8:
        case 15:
        case 16:
        case 24:
        case 32: {
            break;
        }
        default: {
            r[DISPI_BPP] = 8;
            break;
        }
    }
    unsigned bits = dispi_bits();
    r[DISPI_XRES] &= (uint16_t)~7u;
    if (r[DISPI_XRES] == 0) {
        r[DISPI_XRES] = 8;
    }
    if (r[DISPI_XRES] > DISPI_MAX_XRES) {
        r[DISPI_XRES] = DISPI_MAX_XRES;
    }
    r[DISPI_VIRT_WIDTH] &= (uint16_t)~7u;
    if (r[DISPI_VIRT_WIDTH] > DISPI_MAX_XRES) {
        r[DISPI_VIRT_WIDTH] = DISPI_MAX_XRES;
    }
    if (r[DISPI_VIRT_WIDTH] < r[DISPI_XRES]) {
        r[DISPI_VIRT_WIDTH] = r[DISPI_XRES];
    }
    uint32_t line = r[DISPI_VIRT_WIDTH] * bits / 8;
    uint64_t max_y = vbe.vram_size / line;
    if (r[DISPI_YRES] == 0) {
        r[DISPI_YRES] = 1;
    }
    if (r[DISPI_YRES] > DISPI_MAX_YRES) {
        r[DISPI_YRES] = DISPI_MAX_YRES;
    }
    if (r[DISPI_YRES] > max_y) {
        r[DISPI_YRES] = (uint16_t)max_y;
    }
    if (r[DISPI_X_OFFSET] > DISPI_MAX_XRES) {
        r[DISPI_X_OFFSET] = DISPI_MAX_XRES;
    }
    if (r[DISPI_Y_OFFSET] > DISPI_MAX_YRES) {
        r[DISPI_Y_OFFSET] = DISPI_MAX_YRES;
    }
    uint64_t start = (uint64_t)r[DISPI_X_OFFSET] * bits / 8 + (uint64_t)r[DISPI_Y_OFFSET] * line;
    uint64_t shown = (uint64_t)r[DISPI_YRES] * line;
    if (start + shown > vbe.vram_size) {
        r[DISPI_Y_OFFSET] = 0;
        start = (uint64_t)r[DISPI_X_OFFSET] * bits / 8;
        if (start + shown > vbe.vram_size) {
            r[DISPI_X_OFFSET] = 0;
            start = 0;
        }
    }
    r[DISPI_VIRT_HEIGHT] = max_y > 0xffff ? 0xffff : (uint16_t)max_y;
    vbe.line_bytes = line;
    vbe.start = (uint32_t)start;
}

// The window at A0000h shows the bank the interface selects while it is on,
// mapped there directly, and is the VGA's while it is off.
static void window_update(void) {
    uint32_t bank = vbe_on() ? vbe.regs[DISPI_BANK] : NO_BANK;
    if (bank == vbe.window_bank) {
        return;
    }
    if (bank == NO_BANK) {
        gpt_map(WINDOW_BASE, 0, PLANE_SIZE, 0, 0);
    } else {
        map_vram(WINDOW_BASE, (uint64_t)bank << 16, PLANE_SIZE);
    }
    vbe.window_bank = bank;
    gpt_flush();
}

static uint16_t dispi_read(uint16_t index) {
    if (index < DISPI_REG_COUNT) {
        if (vbe.regs[DISPI_ENABLE] & DISPI_GETCAPS) {
            switch (index) {
                case DISPI_XRES: {
                    return DISPI_MAX_XRES;
                }
                case DISPI_YRES: {
                    return DISPI_MAX_YRES;
                }
                case DISPI_BPP: {
                    return DISPI_MAX_BPP;
                }
                default: {
                    break;
                }
            }
        }
        return vbe.regs[index];
    }
    if (index == DISPI_VIDEO_MEMORY_64K) {
        uint64_t blocks = vbe.vram_size >> 16;
        return blocks > 0xffff ? 0xffff : (uint16_t)blocks;
    }
    return 0;
}

static void dispi_write(uint16_t index, uint16_t value) {
    uint16_t *r = vbe.regs;
    switch (index) {
        case DISPI_ID: {
            if (value >= DISPI_ID0 && value <= DISPI_ID5) {
                r[DISPI_ID] = value;
            }
            break;
        }
        case DISPI_XRES:
        case DISPI_YRES:
        case DISPI_BPP:
        case DISPI_VIRT_WIDTH:
        case DISPI_X_OFFSET:
        case DISPI_Y_OFFSET: {
            r[index] = value;
            dispi_fixup();
            if (vbe_on() && vbe.start != 0) {
                vbe.panned = true;
            }
            break;
        }
        case DISPI_BANK: {
            r[DISPI_BANK] = (uint16_t)(value & ((vbe.vram_size >> 16) - 1));
            window_update();
            break;
        }
        case DISPI_ENABLE: {
            if ((value & DISPI_ENABLED) && !(r[DISPI_ENABLE] & DISPI_ENABLED)) {
                r[DISPI_VIRT_WIDTH] = 0;
                r[DISPI_X_OFFSET] = 0;
                r[DISPI_Y_OFFSET] = 0;
                r[DISPI_ENABLE] |= DISPI_ENABLED;
                dispi_fixup();
                vbe.panned = false;
                if (!(value & DISPI_NOCLEARMEM)) {
                    vram_clear((uint64_t)r[DISPI_YRES] * vbe.line_bytes);
                }
            } else {
                r[DISPI_BANK] = 0;
            }
            vga.dac8 = value & DISPI_8BIT_DAC;
            vga.palette_dirty = true;
            r[DISPI_ENABLE] = value;
            window_update();
            break;
        }
        default: {
            break;
        }
    }
    vga.dirty = true;
}

static void dispi_ports(struct vcpu *v, void *ctx, uint16_t offset, unsigned size, bool in,
                        uint32_t *value) {
    (void)v;
    (void)ctx;
    uint16_t mask = size == 1 ? 0xff : 0xffff;
    spinlock_acquire(&vga.lock);
    if (offset == 0) {
        if (in) {
            *value = vbe.index & mask;
        } else {
            vbe.index = (uint16_t)(*value & mask);
        }
    } else if (in) {
        *value = dispi_read(vbe.index) & mask;
    } else {
        dispi_write(vbe.index, (uint16_t)(*value & mask));
    }
    spinlock_release(&vga.lock);
}

static uint32_t expand6(uint8_t v) {
    v &= 0x3f;
    return (uint32_t)((v << 2) | (v >> 4));
}

static uint32_t dac_rgb(uint8_t index) {
    const uint8_t *c = vga.dac[index & vga.pel_mask];
    if (vga.dac8) {
        return (uint32_t)c[0] << 16 | (uint32_t)c[1] << 8 | c[2];
    }
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

static void display_state(struct display_state *s) {
    s->clocking = vga.seq[SEQ_CLOCKING];
    s->char_map = vga.seq[SEQ_CHAR_MAP];
    s->gc_misc = vga.gc[GC_MISC];
    s->gc_mode = vga.gc[GC_MODE] & (GC_MODE_256 | GC_MODE_INTERLEAVE);
    memcpy(s->crtc, vga.crtc, sizeof(s->crtc));
    memcpy(s->ac, vga.ac, sizeof(s->ac));
    s->ac_pas = vga.ac_index & AC_INDEX_PAS;
    s->pel_mask = vga.pel_mask;
    s->dac8 = vga.dac8;
    memcpy(s->dac, vga.dac, sizeof(s->dac));
}

// Whether the guest wrote any of the bytes a scan line fetches since the
// display last showed them.
static bool line_changed(uint32_t columns, uint32_t ma, unsigned row_scan) {
    for (uint32_t c = 0; c < columns; c++) {
        uint32_t chunk = crtc_address(ma + c, row_scan) >> CHUNK_SHIFT;
        if (vga.changed[chunk / 64] & (1ull << (chunk % 64))) {
            return true;
        }
    }
    return false;
}

// Renders the display as the CRT controller would scan it out: all of it, or
// the scan lines that fetch what the guest wrote since the last time, the
// others staying as they were made then.
static void make_frame(uint32_t *width_out, uint32_t *height_out, bool cursor_on, bool blink_on,
                       bool all) {
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
        memset(out.made, all, height);
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
        out.made[y] = all || line_changed(columns + 1, ma, row_scan);
        if (out.made[y]) {
            // The screen below a split can be kept from panning.
            unsigned shift = split && (vga.ac[AC_MODE] & AC_MODE_PAN_SPLIT) ? 0 : pan;
            if (graphics) {
                graphics_line(out.line, span, columns + 1, ma, row_scan, repeat);
            } else {
                text_line(out.line, span, columns + 1, ma, row_scan, dots, repeat, cursor_on,
                          blink_on);
            }
            memcpy(out.frame + y * width, out.line + shift, width * sizeof(uint32_t));
        }
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

void vga_screen_size(uint32_t *width, uint32_t *height) {
    const struct handoff_framebuffer *fb = &core_handoff->video.framebuffer;
    bool turned = fb->rotation != HANDOFF_ROTATE_0;
    *width = turned ? fb->height : fb->width;
    *height = turned ? fb->width : fb->height;
}

// The framebuffer's rectangle the display fills, as the configuration asks,
// at an aspect ratio of aspect_w:aspect_h where it is to be kept.
static void place(uint32_t width, uint32_t height, uint32_t aspect_w, uint32_t aspect_h) {
    const struct handoff *h = core_handoff;
    uint32_t fw;
    uint32_t fh;
    vga_screen_size(&fw, &fh);
    uint64_t w = fw;
    uint64_t hgt = fh;
    switch (h->config.vga_scaling) {
        case HANDOFF_VGA_SCALING_ASPECT: {
            if ((uint64_t)fw * aspect_h >= (uint64_t)fh * aspect_w) {
                w = (uint64_t)fh * aspect_w / aspect_h;
            } else {
                hgt = (uint64_t)fw * aspect_h / aspect_w;
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

// Writes the band of a turned display to the framebuffer, a row of the
// framebuffer at a time, across the band's lines.
static void screen_flush(void) {
    const struct handoff_framebuffer *fb = &core_handoff->video.framebuffer;
    uint8_t *base = (uint8_t *)(uintptr_t)fb->base;
    for (uint32_t x = out.band_x0; x < out.band_x1 && out.band_count != 0; x++) {
        uint32_t column = out.x + x;
        const uint32_t *pixels = out.band + x;
        if (fb->rotation == HANDOFF_ROTATE_90) {
            uint8_t *row = base + (uint64_t)column * fb->pitch;
            for (uint32_t i = out.band_count; i-- > 0;) {
                uint32_t line = out.y + out.band_y + i;
                fb_put(row, fb->width - 1 - line, pixels[(uint64_t)i * out.w], fb->bpp);
            }
        } else {
            uint8_t *row = base + (uint64_t)(fb->height - 1 - column) * fb->pitch;
            for (uint32_t i = 0; i < out.band_count; i++) {
                uint32_t line = out.y + out.band_y + i;
                fb_put(row, line, pixels[(uint64_t)i * out.w], fb->bpp);
            }
        }
    }
    out.band_count = 0;
}

// Shows pixels x0 to x1 of out.fb_line as the display's line y. A turned
// display takes whole lines, as single pixels down the framebuffer's columns
// would keep its writes from combining, and writes them in bands.
static void screen_line(uint32_t y, uint32_t x0, uint32_t x1) {
    const struct handoff_framebuffer *fb = &core_handoff->video.framebuffer;
    if (fb->rotation == HANDOFF_ROTATE_0) {
        uint8_t *row = (uint8_t *)(uintptr_t)fb->base + (uint64_t)(out.y + y) * fb->pitch;
        for (uint32_t x = x0; x < x1; x++) {
            fb_put(row, out.x + x, out.fb_line[x], fb->bpp);
        }
        return;
    }
    if (out.band_count != 0
        && (y != out.band_y + out.band_count || out.band_count == BAND_LINES)) {
        screen_flush();
    }
    if (out.band_count == 0) {
        out.band_y = y;
        out.band_x0 = x0;
        out.band_x1 = x1;
    }
    if (x0 < out.band_x0) {
        out.band_x0 = x0;
    }
    if (x1 > out.band_x1) {
        out.band_x1 = x1;
    }
    memcpy(out.band + (uint64_t)out.band_count * out.w, out.fb_line, out.w * sizeof(uint32_t));
    out.band_count++;
}

// Starts showing a frame, all of it again where its size or border changed.
static void draw_start(uint32_t width, uint32_t height, uint32_t border) {
    bool all = !out.valid || width != out.width || height != out.height || border != out.border;
    if (all) {
        out.width = width;
        out.height = height;
        out.border = border;
        // As a VGA monitor shows every mode, at 4:3.
        place(width, height, 4, 3);
        fb_clear(border);
        out.valid = true;
        out.vbe_valid = false;
    }
    out.busy = true;
    out.busy_vbe = false;
    out.redraw_all = all;
    out.next_line = 0;
}

// Copies the frame's lines that changed since the last frame to the
// framebuffer, from out.next_line on, until done or the CPU's time slice is
// over.
static void draw_lines(void) {
    const struct handoff_framebuffer *fb = &core_handoff->video.framebuffer;
    uint32_t width = out.width;
    uint32_t height = out.height;
    bool all = out.redraw_all;
    for (uint32_t sy = out.next_line; sy < height; sy++) {
        if (sy != out.next_line && cpu_slice_over()) {
            out.next_line = sy;
            screen_flush();
            return;
        }
        if (!all && !out.made[sy]) {
            continue;
        }
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
        if (x1 > out.w) {
            x1 = out.w;
        }
        bool turned = fb->rotation != HANDOFF_ROTATE_0;
        uint32_t source = UINT32_MAX;
        uint32_t pixel = 0;
        for (uint32_t x = turned ? 0 : x0; x < (turned ? out.w : x1); x++) {
            uint32_t rgb = now[out.xmap[x]];
            if (rgb != source) {
                source = rgb;
                pixel = fb_pixel(rgb);
            }
            out.fb_line[x] = pixel;
        }
        for (uint32_t y = y0; y < y1 && y < out.h; y++) {
            screen_line(y, x0, x1);
        }
    }
    screen_flush();
    out.busy = false;
}

static uint32_t expand5(uint32_t v) {
    return (v << 3) | (v >> 2);
}

static void vbe_convert(const uint8_t *p, uint32_t width, unsigned bpp, uint32_t *rgb) {
    switch (bpp) {
        case 8: {
            for (uint32_t x = 0; x < width; x++) {
                rgb[x] = out.vbe_palette[p[x]];
            }
            break;
        }
        case 15: {
            for (uint32_t x = 0; x < width; x++) {
                uint32_t v = p[2 * x] | (uint32_t)p[2 * x + 1] << 8;
                rgb[x] = expand5((v >> 10) & 31) << 16 | expand5((v >> 5) & 31) << 8
                         | expand5(v & 31);
            }
            break;
        }
        case 16: {
            for (uint32_t x = 0; x < width; x++) {
                uint32_t v = p[2 * x] | (uint32_t)p[2 * x + 1] << 8;
                uint32_t g = (v >> 5) & 63;
                rgb[x] = expand5(v >> 11) << 16 | ((g << 2) | (g >> 4)) << 8 | expand5(v & 31);
            }
            break;
        }
        case 24: {
            for (uint32_t x = 0; x < width; x++) {
                const uint8_t *q = p + 3 * x;
                rgb[x] = (uint32_t)q[2] << 16 | (uint32_t)q[1] << 8 | q[0];
            }
            break;
        }
        case 32: {
            for (uint32_t x = 0; x < width; x++) {
                const uint8_t *q = p + 4 * x;
                rgb[x] = (uint32_t)q[2] << 16 | (uint32_t)q[1] << 8 | q[0];
            }
            break;
        }
        default: {
            memset(rgb, 0, width * sizeof(uint32_t));
            break;
        }
    }
}

static uint64_t line_hash(const uint8_t *p, uint32_t bytes) {
    uint64_t h = 0xcbf29ce484222325ull ^ bytes;
    uint32_t i = 0;
    for (; i + 8 <= bytes; i += 8) {
        uint64_t w;
        memcpy(&w, p + i, 8);
        h = (h ^ w) * 0x100000001b3ull;
        h ^= h >> 29;
    }
    for (; i < bytes; i++) {
        h = (h ^ p[i]) * 0x100000001b3ull;
    }
    return h;
}

// The Bochs interface's display, its lines redrawn where their bytes changed,
// as nothing traps the guest's writes to its memory.
static bool direct_wanted(void) {
    const struct handoff_framebuffer *fb = &core_handoff->video.framebuffer;
    return vbe.direct_size != 0 && vbe_on() && !vbe.panned && vbe.regs[DISPI_BPP] == 32
           && vbe.regs[DISPI_XRES] == fb->width && vbe.regs[DISPI_YRES] == fb->height
           && vbe.line_bytes == fb->pitch && vbe.start == 0;
}

// Moves the start of video memory into the firmware's framebuffer or back as
// the mode has it, with the other processors out of the guest, as what they
// write meanwhile would be lost.
static void direct_update(void) {
    spinlock_acquire(&vga.lock);
    bool want = direct_wanted();
    spinlock_release(&vga.lock);
    // A stepping vCPU keeps the others paused itself.
    if (want == vbe.direct || this_cpu()->vcpu->stepping) {
        return;
    }
    vcpu_pause_others();
    spinlock_acquire(&vga.lock);
    want = direct_wanted();
    if (want != vbe.direct) {
        uint8_t *fb = (uint8_t *)(uintptr_t)core_handoff->video.framebuffer.base;
        if (want) {
            memcpy(fb, vbe.vram, vbe.direct_size);
        } else {
            memcpy(vbe.vram, fb, vbe.direct_size);
        }
        vbe.direct = want;
        if (vbe.lfb != 0) {
            map_vram(vbe.lfb, 0, vbe.vram_size);
        }
        if (vbe.window_bank != NO_BANK) {
            map_vram(WINDOW_BASE, (uint64_t)vbe.window_bank << 16, PLANE_SIZE);
        }
        gpt_flush();
        out.vbe_valid = false;
    }
    spinlock_release(&vga.lock);
    vcpu_resume_others();
}

// Starts showing the Bochs interface's display: every line where its mode or
// 8-bit palette changed, else the lines whose bytes changed.
static void vbe_start(void) {
    if (vbe.direct) {
        return;
    }
    spinlock_acquire(&vga.lock);
    uint32_t width = vbe.regs[DISPI_XRES];
    uint32_t height = vbe.regs[DISPI_YRES];
    uint16_t bpp = vbe.regs[DISPI_BPP];
    uint32_t line_bytes = vbe.line_bytes;
    uint32_t start = vbe.start;
    bool palette = vga.palette_dirty;
    vga.palette_dirty = false;
    vga.dirty = false;
    if (bpp == 8 && (palette || !out.vbe_valid)) {
        for (unsigned i = 0; i < 256; i++) {
            out.vbe_palette[i] = dac_rgb((uint8_t)i);
        }
    }
    spinlock_release(&vga.lock);

    bool all = !out.vbe_valid || width != out.width || height != out.height || bpp != out.vbe_bpp
               || line_bytes != out.vbe_line_bytes || start != out.vbe_start
               || (bpp == 8 && palette);
    if (all) {
        out.width = width;
        out.height = height;
        out.vbe_bpp = bpp;
        out.vbe_line_bytes = line_bytes;
        out.vbe_start = start;
        // Its pixels are square.
        place(width, height, width, height);
        fb_clear(0);
        out.vbe_valid = true;
        out.valid = false;
    }
    out.busy = true;
    out.busy_vbe = true;
    out.redraw_all = all;
    out.next_line = 0;
}

// Whether the guest changed the Bochs interface's mode since the redraw under
// way started.
static bool vbe_mode_changed(void) {
    spinlock_acquire(&vga.lock);
    bool changed = vbe.regs[DISPI_XRES] != out.width || vbe.regs[DISPI_YRES] != out.height
                   || vbe.regs[DISPI_BPP] != out.vbe_bpp || vbe.line_bytes != out.vbe_line_bytes
                   || vbe.start != out.vbe_start;
    spinlock_release(&vga.lock);
    return changed;
}

// Shows the Bochs interface's lines that changed, from out.next_line on,
// until done or the CPU's time slice is over.
static void vbe_lines(void) {
    uint32_t width = out.width;
    uint32_t height = out.height;
    uint16_t bpp = out.vbe_bpp;
    bool all = out.redraw_all;
    uint32_t bytes = (width * (bpp == 15 ? 16 : bpp) + 7) / 8;
    for (uint32_t sy = out.next_line; sy < height; sy++) {
        if (sy != out.next_line && cpu_slice_over()) {
            out.next_line = sy;
            screen_flush();
            return;
        }
        uint64_t at = out.vbe_start + (uint64_t)sy * out.vbe_line_bytes;
        if (at + bytes > vbe.vram_size) {
            break;
        }
        const uint8_t *p = vbe.vram + at;
        uint64_t hash = line_hash(p, bytes);
        if (!all && hash == out.vbe_hashes[sy]) {
            continue;
        }
        out.vbe_hashes[sy] = hash;
        vbe_convert(p, width, bpp, out.vbe_line);

        uint32_t y0 = (uint32_t)(((uint64_t)sy * out.h + height - 1) / height);
        uint32_t y1 = (uint32_t)(((uint64_t)(sy + 1) * out.h + height - 1) / height);
        uint32_t source = UINT32_MAX;
        uint32_t pixel = 0;
        for (uint32_t x = 0; x < out.w; x++) {
            uint32_t rgb = out.vbe_line[out.xmap[x]];
            if (rgb != source) {
                source = rgb;
                pixel = fb_pixel(rgb);
            }
            out.fb_line[x] = pixel;
        }
        for (uint32_t y = y0; y < y1 && y < out.h; y++) {
            screen_line(y, 0, out.w);
        }
    }
    screen_flush();
    out.busy = false;
}

bool vga_redrawing(void) {
    return out.busy && this_cpu()->bsp;
}

void vga_refresh(void) {
    if (!vga.on || !this_cpu()->bsp) {
        return;
    }
    uint64_t now = time_ns();
    if (out.busy) {
        bool vbe_mode = vbe_on();
        if (out.busy_vbe == vbe_mode && !(vbe_mode && vbe_mode_changed())) {
            if (vbe_mode) {
                vbe_lines();
            } else {
                draw_lines();
            }
            return;
        }
        // The guest changed modes meanwhile: start again with the new one.
        out.busy = false;
    } else if (now - out.last < REFRESH_NS) {
        return;
    }
    out.last = now;
    if (vbe.present) {
        direct_update();
    }
    if (vbe_on()) {
        vbe_start();
        if (out.busy) {
            vbe_lines();
        }
        return;
    }
    uint64_t frames = now / frame_ns();
    bool cursor_on = !((frames >> CURSOR_FRAMES_SHIFT) & 1);
    bool blink_on = !((frames >> BLINK_FRAMES_SHIFT) & 1);
    uint32_t blink = (cursor_on ? 1u : 0u) | (blink_on ? 2u : 0u);
    if (!vga.dirty && blink == out.blink && out.valid) {
        return;
    }

    uint32_t width;
    uint32_t height;
    spinlock_acquire(&vga.lock);
    vga.dirty = false;
    struct display_state state;
    display_state(&state);
    // A text mode's font and its blinking show on every line, whatever bytes
    // it fetches.
    bool text = !(vga.gc[GC_MISC] & GC_MISC_GRAPHICS);
    bool all = !out.valid || memcmp(&state, &out.state, sizeof(state)) != 0
               || (text && (vga.font_changed || blink != out.blink));
    make_frame(&width, &height, cursor_on, blink_on, all);
    out.state = state;
    out.blink = blink;
    memset(vga.changed, 0, sizeof(vga.changed));
    vga.font_changed = false;
    uint32_t border = dac_rgb(ac_color_border());
    spinlock_release(&vga.lock);
    draw_start(width, height, border);
    draw_lines();
}

bool vga_emulated(void) {
    return vga.on;
}

uint64_t vga_lfb(void) {
    return vbe.present ? vbe.lfb : 0;
}

// The Bochs interface on the video memory the loader reserved, there for the
// guest where the layout found room.
static void vbe_init(void) {
    const struct handoff_video *video = &core_handoff->video;
    vbe.vram_phys = video->vram_base;
    vbe.vram = (uint8_t *)(uintptr_t)video->vram_base;
    vbe.vram_size = video->vram_size;
    vbe.regs[DISPI_ID] = DISPI_ID5;
    vbe.window_bank = NO_BANK;
    out.vbe_hashes = page_alloc((DISPI_MAX_YRES * sizeof(uint64_t) + PAGE_SIZE - 1) / PAGE_SIZE);
    out.vbe_line = page_alloc((DISPI_MAX_XRES * sizeof(uint32_t) + PAGE_SIZE - 1) / PAGE_SIZE);
    struct io_handler ports = {DISPI_PORT, DISPI_PORT_COUNT, dispi_ports, NULL};
    io_register(&ports);
    vbe.present = true;

    // Bochs' 32 bpp pixels hold blue, green, and red in their first three
    // bytes, and its scan lines are a multiple of 32 bytes. A turned picture
    // is never the framebuffer's own.
    const struct handoff_framebuffer *fb = &video->framebuffer;
    uint64_t frame = ((uint64_t)fb->pitch * fb->height + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    if (fb->present && fb->rotation == HANDOFF_ROTATE_0 && fb->bpp == 32 && fb->red_size == 8
        && fb->red_shift == 16
        && fb->green_size == 8 && fb->green_shift == 8 && fb->blue_size == 8
        && fb->blue_shift == 0 && fb->pitch % 32 == 0 && fb->base % PAGE_SIZE == 0
        && frame <= fb->size && frame <= vbe.vram_size) {
        vbe.direct_size = frame;
    }

    vbe.lfb = layout_lfb();
    if (vbe.lfb == 0) {
        printf("No room for the emulated VGA's linear framebuffer\n");
        return;
    }
    map_vram(vbe.lfb, 0, vbe.vram_size);
    gpt_flush();
    printf("Emulated VGA's linear framebuffer: %llu MiB at %#llx\n",
           (unsigned long long)(vbe.vram_size >> 20), (unsigned long long)vbe.lfb);
}

static const char *const scaling_names[] = {"stretched", "kept at 4:3", "centred", "scaled",
                                            "scaled sharply"};

static bool emulation_wanted(void) {
    const struct handoff *h = core_handoff;
    switch (h->config.vga_emulation) {
        case HANDOFF_VGA_EMULATION_NEVER: {
            return false;
        }
        case HANDOFF_VGA_EMULATION_ALWAYS: {
            return true;
        }
        default: {
            return !h->video.vbios_present;
        }
    }
}

static bool framebuffer_usable(const struct handoff_framebuffer *fb) {
    return fb->present && (fb->bpp == 32 || fb->bpp == 24 || fb->bpp == 16);
}

uint64_t vga_lfb_size(void) {
    const struct handoff *h = core_handoff;
    if (!emulation_wanted() || !framebuffer_usable(&h->video.framebuffer)) {
        return 0;
    }
    return h->video.vram_size;
}

bool vga_init(void) {
    const struct handoff *h = core_handoff;
    if (!emulation_wanted()) {
        return false;
    }
    const struct handoff_framebuffer *fb = &h->video.framebuffer;
    if (!framebuffer_usable(fb)) {
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
    uint32_t screen_width;
    uint32_t screen_height;
    vga_screen_size(&screen_width, &screen_height);
    size_t line_pages = (screen_width * sizeof(uint32_t) + PAGE_SIZE - 1) / PAGE_SIZE;
    out.xmap = page_alloc(line_pages);
    out.fb_line = page_alloc(line_pages);
    if (fb->rotation != HANDOFF_ROTATE_0) {
        out.band = page_alloc(BAND_LINES * line_pages);
    }
    mm_write_combining(fb->base, (uint64_t)fb->pitch * fb->height);
    vga.pel_mask = 0xff;
    vga.misc = MISC_COLOR;

    struct io_handler ports = {PORT_BASE, PORT_COUNT, ports_access, NULL};
    io_register(&ports);
    mmio_register(WINDOW_BASE, WINDOW_SIZE, &window_ops, NULL);
    gpt_map(WINDOW_BASE, 0, WINDOW_SIZE, 0, 0);
    gpt_flush();
    if (h->video.vram_size != 0) {
        vbe_init();
    }
    vga.on = true;

    unsigned scaling = h->config.vga_scaling < 5 ? h->config.vga_scaling : 0;
    printf("Emulating a VGA, %s on the %ux%u framebuffer at %#llx%s\n", scaling_names[scaling],
           fb->width, fb->height, (unsigned long long)fb->base,
           fb->rotation == HANDOFF_ROTATE_90    ? ", turned clockwise"
           : fb->rotation == HANDOFF_ROTATE_270 ? ", turned anticlockwise"
                                                : "");
    return true;
}
