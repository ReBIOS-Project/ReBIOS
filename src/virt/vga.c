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

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <virt/vga.h>
#include <core.h>
#include <handoff.h>
#include <lib/heap.h>
#include <lib/log.h>
#include <lib/spinlock.h>
#include <lib/time.h>
#include <lib/util.h>
#include <memory.h>
#include <sys/cpu.h>
#include <sys/mm.h>
#include <sys/simd.h>
#include <sys/smp.h>
#include <sys/x86.h>
#include <virt/dispi.h>
#include <virt/gpt.h>
#include <virt/io.h>
#include <virt/layout.h>
#include <virt/mmio.h>
#include <virt/nsvm.h>
#include <virt/nvmx.h>
#include <virt/vcpu.h>
#include <virt/vga_draw.h>
#include <virt/vgabios.h>

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

// The display runs at 60 Hz with 480 lines, else at 70 Hz, and is in vertical
// retrace for the first part of each frame.
#define FRAME_NS_70 ((uint32_t)14285714)
#define FRAME_NS_60 ((uint32_t)16683217)
#define RETRACE_NS ((uint32_t)600000)

// The cursor shows for 16 frames and hides for 16, characters with the blink
// attribute for 32 each.
#define CURSOR_FRAMES_SHIFT 4
#define BLINK_FRAMES_SHIFT 5

#define MAX_WIDTH 1024
#define MAX_HEIGHT 768
#define REFRESH_NS ((uint32_t)20000000)
// A frame begins no sooner than this many times as long as the processors took
// on the last one together, so that large pictures on slow processors leave
// the guest most of their time.
#define REDRAW_SHARE 4
// A frame compares every line of the Bochs interface's display this often, so
// that what the dirty flags do not show, devices' writes for instance, shows
// all the same.
#define COMPARE_ALL_NS ((uint64_t)1000000000)
// The longest the other processors wait out of the guest for the last pages of
// video memory to come back from the framebuffer.
#define LEAVE_PAUSE_NS ((uint64_t)250000)
// The lines of a frame that a processor takes at a time, an even number, as a
// double scan's second line is its first again.
#define SHARE_LINES 16
// A frame with fewer source lines to look at is the boot processor's alone.
#define SHARED_MIN 64

// The display tracks the guest's writes to the planes in chunks of plane
// address, 1 << CHUNK_SHIFT bytes each.
#define CHUNK_SHIFT 6
#define CHUNKS (PLANE_SIZE >> CHUNK_SHIFT)

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
    // showed them, and whether it wrote plane 2, which holds text modes'
    // fonts.
    uint64_t changed[CHUNKS / 64];
    bool font_changed;
    // The DAC takes 8 bits per primary, as the Bochs interface can select.
    bool dac8;
    bool palette_dirty;
} vga;

// The Bochs display interface, on video memory of its own, which the guest
// finds in the PCI memory of the bus that the display's VGA device is on.
static struct {
    bool present;
    uint8_t *vram;
    uintptr_t vram_phys;
    size_t vram_size;
    uint16_t index;
    uint16_t regs[DISPI_REG_COUNT];
    size_t line_bytes;
    uint32_t start;
    // The bank the window at A0000h shows, while the interface is enabled.
    uint32_t window_bank;
    // Where the guest finds the video memory as its linear framebuffer, 0 if
    // nowhere.
    uintptr_t lfb;
    // While the guest's mode is the firmware framebuffer's own, shown from the
    // start of video memory, the first direct_size bytes of video memory are
    // that framebuffer, which nothing then has to copy to. Panning or flipping
    // pages ends that until the next mode set.
    size_t direct_size;
    bool direct;
    bool panned;
    // While the guest's mode leaves the framebuffer's, the framebuffer stays
    // the start of video memory to the guest as that comes back from it: the
    // pages still to read back, a bit for each, and the next to look at.
    bool leaving;
    uint64_t *leave_pages;
    size_t leave_next;
    // Whether the processor's dirty flags tell which pages of video memory the
    // guest wrote to, and those pages: since the frame shown began, and
    // through banks the window showed before, a bit for each.
    bool track;
    uint64_t *written;
    uint64_t *written_before;
    _Atomic(bool) written_before_any;
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
    bool dac8;
    uint8_t dac[256][3];
};

// How a VGA scan line of the frame is fetched: from the character clock at ma,
// in row row_scan, shown shift dots further left, or as the line above it
// again, as double scanning shows it.
struct scan_plan {
    uint32_t ma;
    uint8_t row_scan;
    uint8_t shift;
    bool again;
};

// A rectangle of the framebuffer, empty where x0 is not below x1.
struct rect {
    uint32_t x0;
    uint32_t y0;
    uint32_t x1;
    uint32_t y1;
};

// The display, and how it is drawn: from the frame, which the VGA's planes
// make, or from the Bochs interface's video memory, scaled into the back
// buffer, as large as the framebuffer and in its format, of which what changed
// goes to the framebuffer once the frame is all drawn.
static struct {
    // The VGA's frame, as made from the planes, and as last drawn.
    uint32_t *frame;
    uint32_t *shown;
    // The source's size, and the rectangle of the screen the display fills.
    uint32_t width;
    uint32_t height;
    uint32_t x;
    uint32_t y;
    uint32_t w;
    uint32_t h;
    // Scaled across as scale has it, and filtered down where filter_y is set.
    struct vga_draw_scale scale;
    bool filter_y;
    bool valid;
    // When the frame drawn began, how long the processors took on it, and when
    // the next may begin.
    uint64_t frame_start;
    _Atomic(uint64_t) frame_cost;
    uint64_t next_frame;
    uint32_t blink;
    uint32_t border;
    // The registers and palette the frame was last made with, what its scan
    // lines are made of and how, which of them are made again, whether the
    // display is blank, and the scan lines' common shape.
    struct display_state state;
    struct vga_draw_frame draw;
    struct scan_plan plan[MAX_HEIGHT];
    bool made[MAX_HEIGHT];
    bool blank;
    bool graphics;
    struct vga_draw_scan scan;
    // What the Bochs interface last showed: its mode, and the palette.
    bool vbe_valid;
    uint16_t vbe_bpp;
    size_t vbe_line_bytes;
    uint32_t vbe_start;
    uint32_t vbe_palette[256];
    // Whether the frame compares every line, whether the last went by the
    // dirty flags, and when a frame next compares every line anyway.
    bool vbe_compare_all;
    bool vbe_tracked;
    uint64_t vbe_compare_due;
    // Video memory as last drawn, as large as it, which the lines are compared
    // with.
    uint8_t *shadow;
    // The pixels of each source line that changed since the line was last
    // drawn, from first to end.
    uint32_t *changed_first;
    uint32_t *changed_end;
    // The back buffer, and the framebuffer's pixel format.
    uint8_t *back;
    unsigned int format;
} out;

// What a processor draws with: a VGA scan line, a display row, a source line
// of the Bochs interface converted, two source lines scaled across, the
// display columns each holds, which was used last, and the frame they are of;
// and a band of SHARE_LINES display rows of a turned display.
struct vga_scratch {
    uint32_t *line;
    uint32_t *row;
    uint32_t *source;
    uint32_t *across[2];
    uint32_t across_line[2];
    uint32_t across_x0[2];
    uint32_t across_x1[2];
    size_t across_last;
    uint32_t generation;
    uint32_t *band;
};

// The frame being drawn, which every processor takes part in, SHARE_LINES
// lines at a time: first its source lines marked where they changed, then the
// display's rows drawn into the back buffer, then what changed copied to the
// framebuffer by one processor.
#define STEP_IDLE 0
#define STEP_DRAW 1
#define STEP_COPY 2

static struct {
    _Atomic(uint32_t) step;
    // Which frame it is, for what processors keep of one.
    _Atomic(uint32_t) generation;
    bool vbe;
    // Whether every line is drawn whatever changed.
    bool all;
    uint32_t mark_chunks;
    _Atomic(uint32_t) mark_next;
    _Atomic(uint32_t) mark_done;
    uint32_t draw_chunks;
    _Atomic(uint32_t) draw_next;
    _Atomic(uint32_t) draw_done;
    // The framebuffer's rectangle that each share of display rows changed,
    // which the copy goes through, and whether it copies the whole framebuffer
    // instead.
    struct rect *dirty;
    size_t dirty_count;
    bool whole;
    size_t copy_rect;
    uint32_t copy_row;
    // The processor that copies.
    struct cpu *copier;
    // The processors taking part.
    _Atomic(uint32_t) busy;
} job;

// Each byte of a plane as its 8 dots' bits, the leftmost's in the lowest byte.
static uint64_t plane_spread[256];

static uint8_t rotate_right(uint8_t value, size_t count) {
    count &= 7;
    return (uint8_t)((value >> count) | (value << ((8 - count) & 7)));
}

// The graphics controller's function on data and the latches, for every plane.
static uint32_t logic(uint32_t data, uint32_t latches) {
    switch ((vga.gc[GC_ROTATE] >> 3) & 3) {
        case 1: {
            return data & latches;
        }
        case 2: {
            return data | latches;
        }
        case 3: {
            return data ^ latches;
        }
        default: {
            return data;
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

// A byte for each plane, plane p's in bits 8p to 8p + 7: all ones in those of
// the planes a 4-bit mask selects.
static uint32_t plane_bytes(uint8_t planes) {
    return ((planes & 1) != 0 ? (uint32_t)0x000000ff : 0)
         | ((planes & 2) != 0 ? (uint32_t)0x0000ff00 : 0)
         | ((planes & 4) != 0 ? (uint32_t)0x00ff0000 : 0)
         | ((planes & 8) != 0 ? (uint32_t)0xff000000 : 0);
}

// What the graphics controller makes of a CPU write's byte for each plane,
// from the latches as they are, as plane_bytes() lays them out.
static uint32_t write_results(uint8_t value) {
    uint32_t latches = read_le32(vga.latch);
    uint8_t mode = vga.gc[GC_MODE] & 3;
    if (mode == 1) {
        return latches;
    }
    uint8_t rotated = rotate_right(value, vga.gc[GC_ROTATE]);
    uint32_t set_reset = plane_bytes(vga.gc[GC_SET_RESET]);
    uint32_t mask = vga.gc[GC_BIT_MASK] * (uint32_t)0x01010101;
    uint32_t data;
    if (mode == 0) {
        uint32_t enable = plane_bytes(vga.gc[GC_ENABLE_SET_RESET]);
        data = (set_reset & enable) | (rotated * (uint32_t)0x01010101 & ~enable);
    } else if (mode == 2) {
        data = plane_bytes(value);
    } else {
        data = set_reset;
        mask &= rotated * (uint32_t)0x01010101;
    }
    return (logic(data, latches) & mask) | (latches & ~mask);
}

// A plane address the guest wrote to, in the planes of a 4-bit mask, for the
// display to show.
static void planes_changed(uint32_t address, uint8_t planes) {
    if (planes != 0) {
        uint32_t chunk = address >> CHUNK_SHIFT;
        vga.changed[chunk / 64] |= (uint64_t)1 << (chunk % 64);
        vga.font_changed |= (planes & 4) != 0;
    }
}

// CPU writes of count elements of size bytes, the first at offset into the
// window and each next one step bytes on, all within a page, which the memory
// map decodes all of or none of: element n's byte i is data[n * size + i], or
// data[i] for every element where same is true. Each byte reaches the planes
// that the map mask and the addressing mode select, with what the graphics
// controller makes of it from the latches, which writes leave as they are.
static void planes_write(
    uint32_t offset,
    int64_t step,
    size_t size,
    size_t count,
    const uint8_t *data,
    bool same
) {
    uint32_t first;
    if (!window_address(offset, &first)) {
        return;
    }
    uint8_t map = vga.seq[SEQ_MAP_MASK] & 0x0f;
    bool chain4 = (vga.seq[SEQ_MEMORY_MODE] & SEQ_MEMORY_CHAIN4) != 0;
    bool odd_even = !chain4 && (vga.seq[SEQ_MEMORY_MODE] & SEQ_MEMORY_SEQUENTIAL) == 0;
    uint32_t results[8];
    for (size_t n = 0; n < count; n++) {
        uint32_t at = first + (uint32_t)((uint64_t)step * n);
        for (size_t i = 0; i < size; i++) {
            if (n == 0 || !same) {
                results[i] = write_results(data[same ? i : n * size + i]);
            }
            uint32_t address = at + (uint32_t)i;
            uint8_t planes = map;
            if (chain4) {
                planes &= (uint8_t)((uint32_t)1 << (address & 3));
                address &= ~(uint32_t)3;
            } else if (odd_even) {
                planes &= (address & 1) != 0 ? 0x0a : 0x05;
                address &= ~(uint32_t)1;
            }
            address &= PLANE_SIZE - 1;
            planes_changed(address, planes);
            for (size_t p = 0; p < 4; p++) {
                if ((planes & ((uint32_t)1 << p)) != 0) {
                    vga.planes[p][address] = (uint8_t)(results[i] >> (8 * p));
                }
            }
        }
    }
    vga.dirty = true;
}

static void mem_write(uint32_t offset, uint8_t value) {
    planes_write(offset, 1, 1, 1, &value, true);
}

static uint8_t mem_read(uint32_t offset) {
    uint32_t address;
    if (!window_address(offset, &address)) {
        return 0xff;
    }
    unsigned int plane;
    if ((vga.seq[SEQ_MEMORY_MODE] & SEQ_MEMORY_CHAIN4) != 0) {
        plane = address & 3;
        address &= ~(uint32_t)3;
    } else if ((vga.seq[SEQ_MEMORY_MODE] & SEQ_MEMORY_SEQUENTIAL) == 0) {
        plane = (vga.gc[GC_READ_MAP] & 2) | (address & 1);
        address &= ~(uint32_t)1;
    } else {
        plane = vga.gc[GC_READ_MAP] & 3;
    }
    address &= PLANE_SIZE - 1;

    for (size_t p = 0; p < 4; p++) {
        vga.latch[p] = vga.planes[p][address];
    }
    if ((vga.gc[GC_MODE] & GC_MODE_READ1) == 0) {
        return vga.latch[plane];
    }
    // Colour compare: ones where every plane that counts matches.
    uint8_t differ = 0;
    for (size_t p = 0; p < 4; p++) {
        if ((vga.gc[GC_DONT_CARE] & ((uint32_t)1 << p)) != 0) {
            differ |= vga.latch[p]
                    ^ ((vga.gc[GC_COLOR_COMPARE] & ((uint32_t)1 << p)) != 0 ? 0xff : 0x00);
        }
    }
    return (uint8_t)~differ;
}

static bool vbe_on(void) {
    return vbe.present && (vbe.regs[DISPI_ENABLE] & DISPI_ENABLED) != 0;
}

// Video memory as the guest reaches it now.
static uint8_t *vram_at(size_t offset) {
    if (vbe.direct && offset < vbe.direct_size) {
        return (uint8_t *)(uintptr_t)core_handoff->video.framebuffer.base + offset;
    }
    return vbe.vram + offset;
}

static void vram_clear(size_t size) {
    uint64_t done = 0;
    if (vbe.direct) {
        done = size < vbe.direct_size ? size : vbe.direct_size;
        memset(vram_at(0), 0, done);
    }
    // What comes back from the framebuffer meanwhile is cleared too.
    if (vbe.leaving) {
        done = 0;
    }
    memset(vbe.vram + done, 0, size - done);
}

// Video memory's byte at offset as the guest reaches it now, and while the
// framebuffer's part comes back, there too.
static void vram_write(size_t offset, uint8_t byte) {
    *vram_at(offset) = byte;
    if (vbe.leaving && offset < vbe.direct_size) {
        vbe.vram[offset] = byte;
    }
}

// Maps size bytes of video memory from offset on at gpa, as the guest reaches
// them now.
static void map_vram(uintptr_t gpa, size_t offset, size_t size) {
    if (vbe.direct && offset < vbe.direct_size) {
        size_t n = vbe.direct_size - offset < size ? vbe.direct_size - offset : size;
        gpt_map(
            gpa, core_handoff->video.framebuffer.base + offset, n,
            GPT_RWX | (vbe.track ? GPT_DIRTY : 0), MT_WC
        );
        gpa += n;
        offset += n;
        size -= n;
    }
    if (size != 0) {
        gpt_map(gpa, vbe.vram_phys + offset, size, GPT_RWX | (vbe.track ? GPT_DIRTY : 0), MT_WB);
    }
}

// While the Bochs interface is on, the window's first 64 KiB show a bank of
// its video memory, normally mapped there directly, and the rest nothing:
// where in video memory offset is, false for nothing.
static bool vbe_window_at(uint32_t offset, size_t *at) {
    *at = ((size_t)vbe.regs[DISPI_BANK] << 16) + offset;
    return offset < PLANE_SIZE && *at < vbe.vram_size;
}

static uint8_t window_byte_read(bool vbe_mode, uint32_t offset) {
    if (vbe_mode) {
        size_t at;
        return vbe_window_at(offset, &at) ? *vram_at(at) : 0xff;
    }
    return mem_read(offset);
}

static void window_byte_write(bool vbe_mode, uint32_t offset, uint8_t byte) {
    size_t at;
    if (!vbe_mode) {
        mem_write(offset, byte);
    } else if (vbe_window_at(offset, &at)) {
        vram_write(at, byte);
    }
}

static uint64_t window_read(struct vcpu *v, void *ctx, uint64_t offset, size_t size) {
    (void)v;
    (void)ctx;
    uint64_t value = 0;
    spinlock_acquire(&vga.lock);
    bool vbe_mode = vbe_on();
    for (size_t i = 0; i < size; i++) {
        value |= (uint64_t)window_byte_read(vbe_mode, (uint32_t)(offset + i)) << (8 * i);
    }
    spinlock_release(&vga.lock);
    return value;
}

static void window_write(struct vcpu *v, void *ctx, uint64_t offset, size_t size, uint64_t value) {
    (void)v;
    (void)ctx;
    spinlock_acquire(&vga.lock);
    bool vbe_mode = vbe_on();
    for (size_t i = 0; i < size; i++) {
        window_byte_write(vbe_mode, (uint32_t)(offset + i), (uint8_t)(value >> (8 * i)));
    }
    spinlock_release(&vga.lock);
}

static void window_read_string(
    struct vcpu *v,
    void *ctx,
    uint64_t offset,
    size_t size,
    int64_t step,
    uint8_t *data,
    size_t count
) {
    (void)v;
    (void)ctx;
    spinlock_acquire(&vga.lock);
    bool vbe_mode = vbe_on();
    for (size_t n = 0; n < count; n++) {
        uint32_t at = (uint32_t)(offset + (uint64_t)step * n);
        for (size_t i = 0; i < size; i++) {
            data[n * size + i] = window_byte_read(vbe_mode, at + (uint32_t)i);
        }
    }
    spinlock_release(&vga.lock);
}

static void window_write_string(
    struct vcpu *v,
    void *ctx,
    uint64_t offset,
    size_t size,
    int64_t step,
    const uint8_t *data,
    bool same,
    size_t count
) {
    (void)v;
    (void)ctx;
    spinlock_acquire(&vga.lock);
    if (!vbe_on()) {
        planes_write((uint32_t)offset, step, size, count, data, same);
    } else {
        for (size_t n = 0; n < count; n++) {
            uint32_t at = (uint32_t)(offset + (uint64_t)step * n);
            const uint8_t *element = same ? data : data + n * size;
            for (size_t i = 0; i < size; i++) {
                window_byte_write(true, at + (uint32_t)i, element[i]);
            }
        }
    }
    spinlock_release(&vga.lock);
}

// MOVSB through write mode 1 with planar addressing, as screen-to-screen
// copies use it: each byte read loads the latches from its address in every
// plane, and each write stores them in the planes the map mask enables, the
// data the processor moves going nowhere.
static bool latch_copies(size_t size) {
    uint8_t addressing = vga.seq[SEQ_MEMORY_MODE] & (SEQ_MEMORY_CHAIN4 | SEQ_MEMORY_SEQUENTIAL);
    return size == 1 && (vga.gc[GC_MODE] & 3) == 1 && addressing == SEQ_MEMORY_SEQUENTIAL;
}

static void latches_move(uint32_t source, uint32_t destination, int64_t step, size_t count) {
    uint32_t from;
    uint32_t to;
    bool read = window_address(source, &from);
    bool written = window_address(destination, &to);
    uint8_t planes = vga.seq[SEQ_MAP_MASK] & 0x0f;
    for (size_t n = 0; n < count; n++) {
        uint32_t delta = (uint32_t)((uint64_t)step * n);
        if (read) {
            uint32_t address = (from + delta) & (PLANE_SIZE - 1);
            for (size_t p = 0; p < 4; p++) {
                vga.latch[p] = vga.planes[p][address];
            }
        }
        if (written) {
            uint32_t address = (to + delta) & (PLANE_SIZE - 1);
            planes_changed(address, planes);
            for (size_t p = 0; p < 4; p++) {
                if ((planes & ((uint32_t)1 << p)) != 0) {
                    vga.planes[p][address] = vga.latch[p];
                }
            }
        }
    }
    if (written) {
        vga.dirty = true;
    }
}

static void window_move_string(
    struct vcpu *v,
    void *ctx,
    uint64_t source,
    uint64_t destination,
    size_t size,
    int64_t step,
    size_t count
) {
    (void)v;
    (void)ctx;
    spinlock_acquire(&vga.lock);
    bool vbe_mode = vbe_on();
    if (!vbe_mode && latch_copies(size)) {
        latches_move((uint32_t)source, (uint32_t)destination, step, count);
        spinlock_release(&vga.lock);
        return;
    }
    for (size_t n = 0; n < count; n++) {
        uint32_t from = (uint32_t)(source + (uint64_t)step * n);
        uint32_t to = (uint32_t)(destination + (uint64_t)step * n);
        uint8_t element[8];
        for (size_t i = 0; i < size; i++) {
            element[i] = window_byte_read(vbe_mode, from + (uint32_t)i);
        }
        if (!vbe_mode) {
            planes_write(to, 1, size, 1, element, false);
            continue;
        }
        for (size_t i = 0; i < size; i++) {
            window_byte_write(true, to + (uint32_t)i, element[i]);
        }
    }
    spinlock_release(&vga.lock);
}

static const struct mmio_ops window_ops = {
    .read = window_read,
    .write = window_write,
    .read_string = window_read_string,
    .write_string = window_write_string,
    .move_string = window_move_string
};

// The CRT controller's registers answer at 3Bxh or 3Dxh, as the miscellaneous
// output register selects.
static bool port_decoded(uint16_t port) {
    bool colour = (vga.misc & MISC_COLOR) != 0;
    if (port < 0x3c0) {
        return !colour;
    }
    if (port >= 0x3d0) {
        return colour;
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
                if (index < AC_COUNT && (index >= 0x10 || (vga.ac_index & AC_INDEX_PAS) == 0)) {
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
            // Protection leaves registers 0-7 alone, but the line compare bit
            // of the overflow register.
            if ((vga.crtc[CRTC_VRE] & CRTC_VRE_PROTECT) != 0 && index <= CRTC_OVERFLOW) {
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

static void ports_access(
    struct vcpu *v,
    void *ctx,
    uint16_t offset,
    size_t size,
    bool in,
    uint32_t *value
) {
    (void)v;
    (void)ctx;
    spinlock_acquire(&vga.lock);
    uint16_t port = (uint16_t)(PORT_BASE + offset);
    if (in) {
        uint32_t result = 0;
        for (size_t i = 0; i < size; i++) {
            uint16_t p = (uint16_t)(port + i);
            bool here = p < PORT_BASE + PORT_COUNT && port_decoded(p);
            result |= (uint32_t)(here ? port_read(p) : 0xff) << (8 * i);
        }
        *value = result;
    } else {
        for (size_t i = 0; i < size; i++) {
            uint16_t p = (uint16_t)(port + i);
            if (p < PORT_BASE + PORT_COUNT && port_decoded(p)) {
                port_write(p, (uint8_t)(*value >> (8 * i)));
            }
        }
    }
    spinlock_release(&vga.lock);
}

static unsigned int dispi_bits(void) {
    return vbe.regs[DISPI_BPP] == 15 ? 16 : vbe.regs[DISPI_BPP];
}

// The registers brought back in range, and the scan line and display start
// that follow from them, as QEMU's vbe_fixup_regs() does.
static void dispi_fixup(void) {
    uint16_t *r = vbe.regs;
    if ((r[DISPI_ENABLE] & DISPI_ENABLED) == 0) {
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
    unsigned int bits = dispi_bits();
    r[DISPI_XRES] &= (uint16_t)~(uint32_t)7;
    if (r[DISPI_XRES] == 0) {
        r[DISPI_XRES] = 8;
    }
    if (r[DISPI_XRES] > DISPI_MAX_XRES) {
        r[DISPI_XRES] = DISPI_MAX_XRES;
    }
    r[DISPI_VIRT_WIDTH] &= (uint16_t)~(uint32_t)7;
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
    size_t start = (uint64_t)r[DISPI_X_OFFSET] * bits / 8 + (uint64_t)r[DISPI_Y_OFFSET] * line;
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
    bool written = vbe.window_bank != NO_BANK
                && gpt_dirty(
                       WINDOW_BASE, PLANE_SIZE, vbe.written_before,
                       ((uint64_t)vbe.window_bank << 16) / PAGE_SIZE, true
                   );
    if (written) {
        atomic_store_explicit(&vbe.written_before_any, true, memory_order_relaxed);
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
        if ((vbe.regs[DISPI_ENABLE] & DISPI_GETCAPS) != 0) {
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
            if ((value & DISPI_ENABLED) != 0 && (r[DISPI_ENABLE] & DISPI_ENABLED) == 0) {
                r[DISPI_VIRT_WIDTH] = 0;
                r[DISPI_X_OFFSET] = 0;
                r[DISPI_Y_OFFSET] = 0;
                r[DISPI_ENABLE] |= DISPI_ENABLED;
                dispi_fixup();
                vbe.panned = false;
                if ((value & DISPI_NOCLEARMEM) == 0) {
                    vram_clear((uint64_t)r[DISPI_YRES] * vbe.line_bytes);
                }
            } else {
                r[DISPI_BANK] = 0;
            }
            vga.dac8 = (value & DISPI_8BIT_DAC) != 0;
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

uint16_t vga_dispi_read(uint16_t index) {
    spinlock_acquire(&vga.lock);
    uint16_t value = dispi_read(index);
    spinlock_release(&vga.lock);
    return value;
}

void vga_dispi_write(uint16_t index, uint16_t value) {
    spinlock_acquire(&vga.lock);
    dispi_write(index, value);
    spinlock_release(&vga.lock);
}

static void dispi_ports(
    struct vcpu *v,
    void *ctx,
    uint16_t offset,
    size_t size,
    bool in,
    uint32_t *value
) {
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
static uint8_t ac_colour(uint8_t colour) {
    uint8_t p = vga.ac[colour & 0x0f];
    uint8_t select = vga.ac[AC_COLOR_SELECT];
    if ((vga.ac[AC_MODE] & AC_MODE_P54) != 0) {
        p = (uint8_t)((p & 0x0f) | ((select & 0x03) << 4));
    }
    return (uint8_t)((p & 0x3f) | ((select & 0x0c) << 4));
}

// The overscan colour register's DAC index, as the colour select register
// extends it.
static uint8_t ac_colour_border(void) {
    return (uint8_t)((vga.ac[0x11] & 0x3f) | ((vga.ac[AC_COLOR_SELECT] & 0x0c) << 4));
}

// How many dots the attribute controller's pel panning shifts the display.
static unsigned int pel_panning(unsigned int dots) {
    unsigned int pan = vga.ac[AC_PANNING] & 0x0f;
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

// The frame's registers and colours, as its scan lines are made with them.
static void draw_frame_set(void) {
    struct vga_draw_frame *f = &out.draw;
    for (size_t p = 0; p < 4; p++) {
        f->planes[p] = vga.planes[p];
    }
    f->dword = (vga.crtc[CRTC_UNDERLINE] & CRTC_DWORD) != 0;
    f->byte = (vga.crtc[CRTC_MODE] & CRTC_MODE_BYTE) != 0;
    f->wrap15 = (vga.crtc[CRTC_MODE] & CRTC_MODE_WRAP15) != 0;
    f->cms = (vga.crtc[CRTC_MODE] & CRTC_MODE_CMS) == 0;
    f->srs = (vga.crtc[CRTC_MODE] & CRTC_MODE_SRS) == 0;
    f->cursor = (uint32_t)vga.crtc[CRTC_CURSOR_HIGH] << 8 | vga.crtc[CRTC_CURSOR_LOW];
    f->cursor_start = vga.crtc[CRTC_CURSOR_START] & 0x1f;
    f->cursor_end = vga.crtc[CRTC_CURSOR_END] & 0x1f;
    f->cursor_shown = (vga.crtc[CRTC_CURSOR_START] & CRTC_CURSOR_OFF) == 0;
    f->underline = vga.crtc[CRTC_UNDERLINE] & 0x1f;
    f->blink = (vga.ac[AC_MODE] & AC_MODE_BLINK) != 0;
    f->line_graphics = (vga.ac[AC_MODE] & AC_MODE_LINE_GRAPHICS) != 0;
    f->char_map = vga.seq[SEQ_CHAR_MAP];
    f->gc_mode = vga.gc[GC_MODE] & (GC_MODE_256 | GC_MODE_INTERLEAVE);
    f->plane_enable = vga.ac[AC_PLANE_ENABLE] & 0x0f;
    f->spread = plane_spread;
    for (size_t i = 0; i < 256; i++) {
        f->palette[i] = dac_rgb((uint8_t)i);
    }
    for (size_t i = 0; i < 16; i++) {
        f->ac_palette[i] = dac_rgb(ac_colour((uint8_t)i));
    }
}

// Whether the guest wrote any of the bytes a scan line fetches since the
// display last showed them.
static bool line_changed(uint32_t columns, uint32_t ma, unsigned int row_scan) {
    for (size_t c = 0; c < columns; c++) {
        uint32_t address = vga_draw_crtc_address(&out.draw, ma + (uint32_t)c, row_scan);
        uint32_t chunk = address >> CHUNK_SHIFT;
        if ((vga.changed[chunk / 64] & ((uint64_t)1 << (chunk % 64))) != 0) {
            return true;
        }
    }
    return false;
}

// Plans the frame as the CRT controller would scan it out: every scan line, or
// those that fetch what the guest wrote since the last time, the others
// staying as they were made then. Returns how many it makes.
static size_t frame_plan(
    uint32_t *width_out,
    uint32_t *height_out,
    bool cursor_on,
    bool blink_on,
    bool all
) {
    bool graphics = (vga.gc[GC_MISC] & GC_MISC_GRAPHICS) != 0;
    unsigned int dots = (graphics || (vga.seq[SEQ_CLOCKING] & SEQ_CLOCKING_8DOT) != 0) ? 8 : 9;
    unsigned int repeat = (vga.seq[SEQ_CLOCKING] & SEQ_CLOCKING_HALF) != 0 ? 2 : 1;
    uint32_t width = (vga.crtc[CRTC_HDE] + (uint32_t)1) * dots * repeat;
    uint32_t overflow = vga.crtc[CRTC_OVERFLOW];
    uint32_t vde = vga.crtc[CRTC_VDE] | ((overflow & 0x02) << 7) | ((overflow & 0x40) << 3);
    uint32_t height = vde + 1;
    if (width > MAX_WIDTH) {
        width = MAX_WIDTH;
    }
    if (height > MAX_HEIGHT) {
        height = MAX_HEIGHT;
    }
    *width_out = width;
    *height_out = height;

    out.graphics = graphics;
    out.blank = (vga.ac_index & AC_INDEX_PAS) == 0
             || (vga.seq[SEQ_CLOCKING] & SEQ_CLOCKING_OFF) != 0;
    if (out.blank) {
        memset(out.made, all, height);
        return all ? height : 0;
    }
    draw_frame_set();

    unsigned int max_scan = vga.crtc[CRTC_MAX_SCAN] & 0x1f;
    bool double_scan = (vga.crtc[CRTC_MAX_SCAN] & CRTC_DOUBLE_SCAN) != 0;
    uint32_t line_compare = vga.crtc[CRTC_LINE_COMPARE]
                          | ((vga.crtc[CRTC_OVERFLOW] & (uint32_t)0x10) << 4)
                          | ((vga.crtc[CRTC_MAX_SCAN] & (uint32_t)0x40) << 3);
    // The start address, with the CRT controller's byte panning, and the first
    // row's first scan line.
    uint32_t ma = ((uint32_t)vga.crtc[CRTC_START_HIGH] << 8 | vga.crtc[CRTC_START_LOW])
                + ((vga.crtc[CRTC_PRESET] >> 5) & (uint32_t)3);
    uint32_t row_step = vga.crtc[CRTC_OFFSET] * (uint32_t)2;
    unsigned int row_scan = vga.crtc[CRTC_PRESET] & (uint32_t)0x1f;
    uint32_t columns = vga.crtc[CRTC_HDE] + (uint32_t)1;
    unsigned int pan = pel_panning(dots) * repeat;
    out.scan = (struct vga_draw_scan){
        width + 2 * dots * repeat, columns + 1, 0, 0, dots, repeat, cursor_on, blink_on
    };
    bool split = false;
    size_t made = 0;
    for (size_t y = 0; y < height; y++) {
        if (y == line_compare) {
            ma = 0;
            row_scan = 0;
            split = true;
        }
        // A double scan's second line is its first again, unless the split
        // begins there.
        bool again = double_scan && (y & 1) != 0 && y != line_compare;
        out.made[y] = again ? out.made[y - 1] : all || line_changed(columns + 1, ma, row_scan);
        // The screen below a split can be kept from panning.
        unsigned int shift = split && (vga.ac[AC_MODE] & AC_MODE_PAN_SPLIT) != 0 ? 0 : pan;
        out.plan[y] = (struct scan_plan){ ma, (uint8_t)row_scan, (uint8_t)shift, again };
        made += out.made[y] ? 1 : 0;
        if (!double_scan || (y & 1) != 0) {
            if (row_scan >= max_scan) {
                row_scan = 0;
                ma += row_step;
            } else {
                row_scan++;
            }
        }
    }
    return made;
}

// The framebuffer's pixel formats that kernels draw in, and any other.
#define FORMAT_RGB 0
#define FORMAT_XBGR 1
#define FORMAT_RGB565 2
#define FORMAT_RGB555 3
#define FORMAT_BGR24 4
#define FORMAT_OTHER 5

static uint32_t channel(uint32_t value, size_t size, uint8_t shift) {
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

static unsigned int fb_format(const struct handoff_framebuffer *fb) {
    bool bytes = fb->red_size == 8 && fb->green_size == 8 && fb->blue_size == 8
              && fb->green_shift == 8;
    bool fives = fb->red_size == 5 && fb->blue_size == 5 && fb->blue_shift == 0
              && fb->green_shift == 5;
    if (fb->bpp == 32 && bytes && fb->red_shift == 16 && fb->blue_shift == 0) {
        return FORMAT_RGB;
    }
    if (fb->bpp == 32 && bytes && fb->red_shift == 0 && fb->blue_shift == 16) {
        return FORMAT_XBGR;
    }
    if (fb->bpp == 24 && bytes && fb->red_shift == 16 && fb->blue_shift == 0) {
        return FORMAT_BGR24;
    }
    if (fb->bpp == 16 && fives && fb->green_size == 6 && fb->red_shift == 11) {
        return FORMAT_RGB565;
    }
    if (fb->bpp == 16 && fives && fb->green_size == 5 && fb->red_shift == 10) {
        return FORMAT_RGB555;
    }
    return FORMAT_OTHER;
}

static void back_put(uint8_t *row, uint32_t x, uint32_t pixel, uint8_t bpp) {
    switch (bpp) {
        case 32: {
            ((uint32_t *)row)[x] = pixel;
            break;
        }
        case 24: {
            uint8_t *p = row + x * 3;
            p[0] = (uint8_t)pixel;
            p[1] = (uint8_t)(pixel >> 8);
            p[2] = (uint8_t)(pixel >> 16);
            break;
        }
        default: {
            ((uint16_t *)row)[x] = (uint16_t)pixel;
            break;
        }
    }
}

// 0RGB pixels into a row of the back buffer from column x on, in the
// framebuffer's format, which may be the pixels' own.
static void back_row(
    const struct vga_draw_ops *ops,
    uint8_t *row,
    uint32_t x,
    const uint32_t *pixels,
    size_t count
) {
    const struct handoff_framebuffer *fb = &core_handoff->video.framebuffer;
    switch (out.format) {
        case FORMAT_RGB: {
            ops->copy((uint32_t *)row + x, pixels, count * sizeof(uint32_t));
            break;
        }
        case FORMAT_XBGR: {
            ops->to_xbgr((uint32_t *)row + x, pixels, count);
            break;
        }
        case FORMAT_RGB565: {
            ops->to_rgb565((uint16_t *)row + x, pixels, count);
            break;
        }
        case FORMAT_RGB555: {
            ops->to_rgb555((uint16_t *)row + x, pixels, count);
            break;
        }
        case FORMAT_BGR24: {
            ops->to_bgr24(row + (size_t)x * 3, pixels, count);
            break;
        }
        default: {
            for (size_t i = 0; i < count; i++) {
                back_put(row, x + (uint32_t)i, fb_pixel(pixels[i]), fb->bpp);
            }
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

// The framebuffer's rectangle that display columns x0 to x1 of display rows y0
// to y1 cover.
static struct rect screen_rect(uint32_t x0, uint32_t x1, uint32_t y0, uint32_t y1) {
    const struct handoff_framebuffer *fb = &core_handoff->video.framebuffer;
    switch (fb->rotation) {
        case HANDOFF_ROTATE_90: {
            return (struct rect){
                fb->width - (out.y + y1), out.x + x0, fb->width - (out.y + y0), out.x + x1
            };
        }
        case HANDOFF_ROTATE_270: {
            return (struct rect){
                out.y + y0, fb->height - (out.x + x1), out.y + y1, fb->height - (out.x + x0)
            };
        }
        default: {
            return (struct rect){ out.x + x0, out.y + y0, out.x + x1, out.y + y1 };
        }
    }
}

static void rect_add(struct rect *to, struct rect r) {
    if (r.x0 >= r.x1) {
        return;
    }
    if (to->x0 >= to->x1) {
        *to = r;
        return;
    }
    to->x0 = r.x0 < to->x0 ? r.x0 : to->x0;
    to->y0 = r.y0 < to->y0 ? r.y0 : to->y0;
    to->x1 = r.x1 > to->x1 ? r.x1 : to->x1;
    to->y1 = r.y1 > to->y1 ? r.y1 : to->y1;
}

// The framebuffer's rectangle the display fills, as the configuration asks, at
// an aspect ratio of aspect_w:aspect_h where it is to be kept.
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
            unsigned int scale = h->config.vga_scale != 0 ? h->config.vga_scale : 1;
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
    // A whole number of display pixels per pixel needs no filtering.
    bool filter = h->config.vga_filter == HANDOFF_VGA_FILTER_BILINEAR;
    vga_draw_scale_set(&out.scale, width, out.w, filter && width != 0 && out.w % width != 0);
    out.filter_y = filter && height != 0 && out.h % height != 0;
}

// Around the display, the overscan colour, as a monitor shows it: the back
// buffer's pixels outside the display's rectangle, which the whole framebuffer
// gets once the frame is drawn.
static void border_fill(const struct vga_draw_ops *ops, uint32_t rgb) {
    const struct handoff_framebuffer *fb = &core_handoff->video.framebuffer;
    uint32_t pixel = fb_pixel(rgb);
    struct rect display = screen_rect(0, out.w, 0, out.h);
    for (size_t y = 0; y < fb->height; y++) {
        uint8_t *row = out.back + y * fb->pitch;
        bool inside = y >= display.y0 && y < display.y1;
        uint32_t spans[2][2] = {
            { 0, inside ? display.x0 : fb->width }, { inside ? display.x1 : fb->width, fb->width }
        };
        for (size_t k = 0; k < 2; k++) {
            uint32_t from = spans[k][0];
            uint32_t to = spans[k][1];
            if (from >= to) {
                continue;
            }
            if (fb->bpp == 32) {
                ops->fill((uint32_t *)row + from, pixel, to - from);
                continue;
            }
            for (size_t x = from; x < to; x++) {
                back_put(row, (uint32_t)x, pixel, fb->bpp);
            }
        }
    }
    job.whole = true;
}

// The Bochs interface's display, its lines redrawn where their bytes changed,
// as nothing traps the guest's writes to its memory: on the pages that the
// dirty flags show written, where the processor keeps them.
static bool direct_wanted(void) {
    const struct handoff_framebuffer *fb = &core_handoff->video.framebuffer;
    return vbe.direct_size != 0 && vbe_on() && !vbe.panned && vbe.regs[DISPI_BPP] == 32
        && vbe.regs[DISPI_XRES] == fb->width && vbe.regs[DISPI_YRES] == fb->height
        && vbe.line_bytes == fb->pitch && vbe.start == 0;
}

// The guest's linear framebuffer and window on video memory, its start in the
// firmware's framebuffer or not, the rest its own either way.
static void direct_map(void) {
    if (vbe.lfb != 0) {
        map_vram(vbe.lfb, 0, vbe.direct_size);
    }
    if (vbe.window_bank != NO_BANK) {
        map_vram(WINDOW_BASE, (uint64_t)vbe.window_bank << 16, PLANE_SIZE);
    }
    gpt_flush();
    out.vbe_valid = false;
}

// When the guest's mode leaves the framebuffer's, the start of video memory
// comes back from the framebuffer, with the streaming loads that read its
// write-combining memory at speed, a slice at a time while the guest goes on
// writing to it there. The pages it writes to meanwhile, which the dirty flags
// tell, come back again, until those left take at most LEAVE_PAUSE_NS with the
// other processors out of the guest, and the guest's mode then has video
// memory of its own. Without dirty flags, it all comes back at once.

// Adds the pages of the start of video memory that the guest wrote to since
// the last call, through its linear framebuffer and the window, now and
// before, to those to read back, their dirty flags cleared: with the other
// processors out of the guest, back in after gpt_flush().
static void leave_take_written(void) {
    size_t pages = vbe.direct_size / PAGE_SIZE;
    size_t words = vbe.vram_size / PAGE_SIZE / 64;
    if (vbe.lfb != 0) {
        gpt_dirty(vbe.lfb, vbe.direct_size, vbe.leave_pages, 0, true);
    }
    if (vbe.window_bank != NO_BANK) {
        gpt_dirty(
            WINDOW_BASE, PLANE_SIZE, vbe.leave_pages,
            ((uint64_t)vbe.window_bank << 16) / PAGE_SIZE, true
        );
    }
    for (size_t i = 0; i < words; i++) {
        vbe.leave_pages[i] |= vbe.written_before[i];
        vbe.written_before[i] = 0;
    }
    atomic_store_explicit(&vbe.written_before_any, false, memory_order_relaxed);
    // The pages after the framebuffer's are video memory's own.
    if (pages % 64 != 0) {
        vbe.leave_pages[pages / 64] &= ((uint64_t)1 << (pages % 64)) - 1;
    }
    for (size_t i = (pages + 63) / 64; i < words; i++) {
        vbe.leave_pages[i] = 0;
    }
}

// The first page from page on still to read back, the framebuffer's page count
// where none is.
static size_t leave_find(size_t page) {
    size_t pages = vbe.direct_size / PAGE_SIZE;
    for (; page < pages; page++) {
        if ((vbe.leave_pages[page / 64] & ((uint64_t)1 << (page % 64))) != 0) {
            break;
        }
    }
    return page;
}

static void leave_read(const struct vga_draw_ops *ops, size_t page) {
    const uint8_t *fb = (const uint8_t *)(uintptr_t)core_handoff->video.framebuffer.base;
    ops->read_fb(vbe.vram + page * PAGE_SIZE, fb + page * PAGE_SIZE, PAGE_SIZE);
    vbe.leave_pages[page / 64] &= ~((uint64_t)1 << (page % 64));
}

// With the other processors out of the guest: every page to read back, none
// written to since.
static void leave_start(void) {
    leave_take_written();
    for (size_t page = 0; page < vbe.direct_size / PAGE_SIZE; page++) {
        vbe.leave_pages[page / 64] |= (uint64_t)1 << (page % 64);
    }
    vbe.leave_next = 0;
    vbe.leaving = true;
    gpt_flush();
}

// The pages written to since the last look, and those left, with the other
// processors out of the guest for at most LEAVE_PAUSE_NS.
static void leave_finish(const struct vga_draw_ops *ops) {
    vcpu_pause_others();
    spinlock_acquire(&vga.lock);
    size_t pages = vbe.direct_size / PAGE_SIZE;
    size_t page = pages;
    // The guest's mode may be the framebuffer's again.
    if (!direct_wanted()) {
        leave_take_written();
        uint64_t until = time_ns() + LEAVE_PAUSE_NS;
        page = leave_find(0);
        while (page < pages && time_ns() < until) {
            leave_read(ops, page);
            page = leave_find(page + 1);
        }
    }
    vbe.leaving = page < pages;
    vbe.leave_next = 0;
    if (!vbe.leaving && !direct_wanted()) {
        vbe.direct = false;
        direct_map();
    } else {
        gpt_flush();
    }
    spinlock_release(&vga.lock);
    vcpu_resume_others();
}

// A slice of reading video memory back, and after a pass over it, the end
// where it is near. The guest's processor comes back after a turn as long.
static void direct_leave(void) {
    struct vcpu *v = this_cpu()->vcpu;
    const struct vga_draw_ops *ops = vga_draw_ops();
    size_t pages = vbe.direct_size / PAGE_SIZE;
    bool passed = false;
    simd_begin(vga_draw_level());
    for (bool first = true; ; first = false) {
        if (!first && cpu_slice_over()) {
            break;
        }
        spinlock_acquire(&vga.lock);
        // The guest's mode may be the framebuffer's again.
        if (direct_wanted()) {
            vbe.leaving = false;
        }
        size_t page = vbe.leaving ? leave_find(vbe.leave_next) : pages;
        if (page < pages) {
            leave_read(ops, page);
            vbe.leave_next = page + 1;
        }
        spinlock_release(&vga.lock);
        if (page == pages) {
            passed = vbe.leaving;
            break;
        }
    }
    // A stepping vCPU keeps the others paused itself.
    if (passed && !v->stepping) {
        leave_finish(ops);
    }
    simd_end();
    if (vbe.leaving) {
        vcpu_exit_after_turn(v);
    }
}

// Moves the start of video memory into the firmware's framebuffer as the mode
// has it, with the other processors out of the guest, as what they write
// meanwhile would be lost, or starts it coming back.
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
    uint8_t *fb = (uint8_t *)(uintptr_t)core_handoff->video.framebuffer.base;
    if (want && !vbe.direct) {
        memcpy(fb, vbe.vram, vbe.direct_size);
        vbe.direct = true;
        direct_map();
    } else if (!want && vbe.direct && vbe.track) {
        leave_start();
    } else if (!want && vbe.direct) {
        simd_begin(vga_draw_level());
        vga_draw_ops()->read_fb(vbe.vram, fb, vbe.direct_size);
        simd_end();
        vbe.direct = false;
        direct_map();
    }
    spinlock_release(&vga.lock);
    vcpu_resume_others();
}

// Takes which pages of video memory, size bytes from start on, the guest wrote
// to since the last frame, through its linear framebuffer or the window, as
// the dirty flags have them, and through banks the window showed before.
// Clearing the flags keeps the other vCPUs out of the guest until they flush
// their TLBs, which may keep translations that are dirty already.
static void vbe_take_written(uintptr_t start, size_t size) {
    memset(vbe.written, 0, vbe.vram_size / PAGE_SIZE / 8);
    size_t first = start / PAGE_SIZE;
    size_t end = page_count(start + size);
    if (end > vbe.vram_size / PAGE_SIZE) {
        end = vbe.vram_size / PAGE_SIZE;
    }
    uintptr_t lfb = vbe.lfb + first * PAGE_SIZE;
    size_t lfb_size = first < end ? (end - first) * PAGE_SIZE : 0;
    bool dirty = (vbe.lfb != 0 && gpt_dirty(lfb, lfb_size, NULL, 0, false))
              || gpt_dirty(WINDOW_BASE, PLANE_SIZE, NULL, 0, false);
    if (!dirty && !atomic_load_explicit(&vbe.written_before_any, memory_order_relaxed)) {
        return;
    }
    // A stepping vCPU keeps the others paused itself: the next frame sees to
    // it.
    bool take = dirty && !this_cpu()->vcpu->stepping;
    if (take) {
        vcpu_pause_others();
    }
    spinlock_acquire(&vga.lock);
    if (atomic_load_explicit(&vbe.written_before_any, memory_order_relaxed)) {
        for (size_t i = 0; i < vbe.vram_size / PAGE_SIZE / 64; i++) {
            vbe.written[i] |= vbe.written_before[i];
            vbe.written_before[i] = 0;
        }
        atomic_store_explicit(&vbe.written_before_any, false, memory_order_relaxed);
    }
    if (take && vbe.lfb != 0) {
        gpt_dirty(lfb, lfb_size, vbe.written, first, true);
    }
    if (take && vbe.window_bank != NO_BANK) {
        gpt_dirty(
            WINDOW_BASE, PLANE_SIZE, vbe.written, ((uint64_t)vbe.window_bank << 16) / PAGE_SIZE,
            true
        );
    }
    spinlock_release(&vga.lock);
    if (take) {
        gpt_flush();
        vcpu_resume_others();
    }
}

static bool vbe_written(uint64_t at, size_t bytes) {
    for (size_t page = at / PAGE_SIZE; page <= (at + bytes - 1) / PAGE_SIZE; page++) {
        if ((vbe.written[page / 64] & ((uint64_t)1 << (page % 64))) != 0) {
            return true;
        }
    }
    return false;
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

// The bytes of video memory that a line of the Bochs interface's display
// shows.
static size_t vbe_bytes(void) {
    return (out.width * (out.vbe_bpp == 15 ? 16 : out.vbe_bpp) + 7) / 8;
}

// The source lines that display row y is made of, and the weight of the
// second; the same line twice where there is no second.
static void row_sources(uint32_t y, uint32_t *top, uint32_t *bottom, uint32_t *weight) {
    if (out.filter_y) {
        vga_draw_filter_position(y, out.h, out.height, top, weight);
        *bottom = *weight != 0 ? *top + 1 : *top;
        return;
    }
    *top = (uint32_t)((uint64_t)y * out.height / out.h);
    *bottom = *top;
    *weight = 0;
}

// The first display column whose source pixel is pixel or after it.
static uint32_t column_of(uint32_t pixel) {
    uint32_t low = 0;
    uint32_t high = out.w;
    while (low < high) {
        uint32_t middle = low + (high - low) / 2;
        if (out.scale.xmap[middle] < pixel) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return low;
}

// The Bochs interface's source pixels from to end of line sy as 0RGB, from
// video memory as last drawn.
static void vbe_convert(
    const struct vga_draw_ops *ops,
    uint32_t sy,
    uint32_t from,
    uint32_t end,
    uint32_t *rgb
) {
    const uint8_t *p = out.shadow + out.vbe_start + (uint64_t)sy * out.vbe_line_bytes;
    size_t count = end - from;
    switch (out.vbe_bpp) {
        case 8: {
            ops->convert8(rgb + from, p + from, count, out.vbe_palette);
            break;
        }
        case 15: {
            ops->convert15(rgb + from, p + (size_t)from * 2, count);
            break;
        }
        case 16: {
            ops->convert16(rgb + from, p + (size_t)from * 2, count);
            break;
        }
        case 24: {
            ops->convert24(rgb + from, p + (size_t)from * 3, count);
            break;
        }
        case 32: {
            ops->convert32(rgb + from, p + (size_t)from * 4, count);
            break;
        }
        default: {
            ops->fill(rgb + from, 0, count);
            break;
        }
    }
}

// Source line sy scaled across, display columns x0 to x1 of it at least, in
// one of the processor's two such lines, the one not used last making room.
static const uint32_t *across(
    const struct vga_draw_ops *ops,
    struct vga_scratch *s,
    uint32_t sy,
    uint32_t x0,
    uint32_t x1
) {
    for (size_t k = 0; k < 2; k++) {
        if (s->across_line[k] == sy && s->across_x0[k] <= x0 && s->across_x1[k] >= x1) {
            s->across_last = k;
            return s->across[k];
        }
    }
    size_t k = s->across_last ^ 1;
    const uint32_t *line = out.frame + (size_t)sy * out.width;
    if (job.vbe) {
        // The source pixels the columns take, the one after each included
        // where filtering.
        uint32_t from = out.scale.xmap[x0];
        uint32_t end = out.scale.xmap[x1 - 1] + (out.scale.filter ? 2 : 1);
        vbe_convert(ops, sy, from, end < out.width ? end : out.width, s->source);
        line = s->source;
    }
    ops->scale(s->across[k], line, &out.scale, x0, x1);
    s->across_line[k] = sy;
    s->across_x0[k] = x0;
    s->across_x1[k] = x1;
    s->across_last = k;
    return s->across[k];
}

// Marks which pixels of the Bochs interface's line sy changed since the line
// was last drawn: all of a line that the frame draws again, else those that
// differ from video memory as last drawn, on pages the guest wrote to where
// the dirty flags tell, which then join it.
static void mark_vbe(const struct vga_draw_ops *ops, uint32_t sy) {
    size_t bytes = vbe_bytes();
    uint64_t at = out.vbe_start + (uint64_t)sy * out.vbe_line_bytes;
    if (at + bytes > vbe.vram_size
     || (!job.all && !out.vbe_compare_all && !vbe_written(at, bytes))) {
        return;
    }
    size_t first = 0;
    size_t end = bytes;
    if (!job.all) {
        ops->differ(vbe.vram + at, out.shadow + at, bytes, &first, &end);
        if (first >= end) {
            return;
        }
    }
    ops->copy(out.shadow + at + first, vbe.vram + at + first, end - first);
    unsigned int bits = out.vbe_bpp == 15 ? 16 : out.vbe_bpp;
    uint32_t pixel_first = 0;
    uint32_t pixel_end = out.width;
    if (bits >= 8) {
        size_t size = bits / 8;
        pixel_first = (uint32_t)(first / size);
        pixel_end = (uint32_t)((end + size - 1) / size);
    }
    out.changed_first[sy] = pixel_first;
    out.changed_end[sy] = pixel_end < out.width ? pixel_end : out.width;
}

// Makes the frame's scan line y again where the plan has it, and marks which
// of its pixels changed since it was last drawn.
static void mark_vga(const struct vga_draw_ops *ops, struct vga_scratch *s, uint32_t y) {
    if (!out.made[y]) {
        return;
    }
    uint32_t width = out.width;
    uint32_t *now = out.frame + (size_t)y * width;
    const struct scan_plan *plan = &out.plan[y];
    if (out.blank) {
        ops->fill(now, 0, width);
    } else if (plan->again && y % SHARE_LINES != 0) {
        ops->copy(now, now - width, width * sizeof(uint32_t));
    } else {
        struct vga_draw_scan scan = out.scan;
        scan.ma = plan->ma;
        scan.row_scan = plan->row_scan;
        if (out.graphics) {
            ops->graphics_line(s->line, &out.draw, &scan);
        } else {
            ops->text_line(s->line, &out.draw, &scan);
        }
        ops->copy(now, s->line + plan->shift, width * sizeof(uint32_t));
    }
    uint32_t *was = out.shown + (size_t)y * width;
    size_t first = 0;
    size_t end = width * sizeof(uint32_t);
    if (!job.all) {
        ops->differ(now, was, width * sizeof(uint32_t), &first, &end);
        if (first >= end) {
            return;
        }
    }
    uint32_t pixel_first = (uint32_t)(first / sizeof(uint32_t));
    uint32_t pixel_end = (uint32_t)((end + sizeof(uint32_t) - 1) / sizeof(uint32_t));
    ops->copy(was + pixel_first, now + pixel_first, (pixel_end - pixel_first) * sizeof(uint32_t));
    out.changed_first[y] = pixel_first;
    out.changed_end[y] = pixel_end;
}

static void mark_share(const struct vga_draw_ops *ops, struct vga_scratch *s, uint32_t share) {
    uint32_t first = share * SHARE_LINES;
    uint32_t end = first + SHARE_LINES < out.height ? first + SHARE_LINES : out.height;
    for (size_t y = first; y < end; y++) {
        out.changed_first[y] = 0;
        out.changed_end[y] = 0;
        if (job.vbe) {
            mark_vbe(ops, (uint32_t)y);
        } else {
            mark_vga(ops, s, (uint32_t)y);
        }
    }
}

// The display columns of row y that its source lines changed, if any, and the
// lines and the weight of the second.
static bool row_span(
    uint32_t y,
    uint32_t *top,
    uint32_t *bottom,
    uint32_t *weight,
    uint32_t *x0,
    uint32_t *x1
) {
    row_sources(y, top, bottom, weight);
    uint32_t first = out.changed_first[*top];
    uint32_t end = out.changed_end[*top];
    if (*bottom != *top && out.changed_first[*bottom] < out.changed_end[*bottom]) {
        if (first >= end || out.changed_first[*bottom] < first) {
            first = out.changed_first[*bottom];
        }
        if (out.changed_end[*bottom] > end) {
            end = out.changed_end[*bottom];
        }
    }
    if (first >= end) {
        return false;
    }
    // A column filtered across takes the pixel after its own too.
    *x0 = column_of(out.scale.filter && first != 0 ? first - 1 : first);
    *x1 = column_of(end);
    return *x0 < *x1;
}

// Display row y's columns x0 to x1, as 0RGB, in a line of the processor's or
// straight into the back buffer's row where its format is the pixels' own.
static const uint32_t *row_pixels(
    const struct vga_draw_ops *ops,
    struct vga_scratch *s,
    uint32_t top,
    uint32_t bottom,
    uint32_t weight,
    uint32_t x0,
    uint32_t x1,
    uint32_t *into
) {
    const uint32_t *a = across(ops, s, top, x0, x1);
    if (bottom == top) {
        if (into != NULL) {
            ops->copy(into + x0, a + x0, (x1 - x0) * sizeof(uint32_t));
        }
        return a;
    }
    const uint32_t *b = across(ops, s, bottom, x0, x1);
    uint32_t *to = into != NULL ? into : s->row;
    ops->blend(to + x0, a + x0, b + x0, weight, x1 - x0);
    return to;
}

static struct rect draw_rows(
    const struct vga_draw_ops *ops,
    struct vga_scratch *s,
    uint32_t r0,
    uint32_t r1
) {
    const struct handoff_framebuffer *fb = &core_handoff->video.framebuffer;
    struct rect dirty = { 0, 0, 0, 0 };
    for (size_t y = r0; y < r1; y++) {
        uint32_t top;
        uint32_t bottom;
        uint32_t weight;
        uint32_t x0;
        uint32_t x1;
        if (!row_span((uint32_t)y, &top, &bottom, &weight, &x0, &x1)) {
            continue;
        }
        uint8_t *row = out.back + (out.y + y) * fb->pitch;
        if (out.format == FORMAT_RGB) {
            row_pixels(ops, s, top, bottom, weight, x0, x1, (uint32_t *)row + out.x);
        } else {
            const uint32_t *pixels = row_pixels(ops, s, top, bottom, weight, x0, x1, NULL);
            back_row(ops, row, out.x + x0, pixels + x0, x1 - x0);
        }
        rect_add(&dirty, screen_rect(x0, x1, (uint32_t)y, (uint32_t)y + 1));
    }
    return dirty;
}

// The rows of a turned display, which run down the framebuffer's columns:
// every row of the share, over the columns any of them changed, in the
// processor's band, then turned into the back buffer.
static struct rect draw_band(
    const struct vga_draw_ops *ops,
    struct vga_scratch *s,
    uint32_t r0,
    uint32_t r1
) {
    const struct handoff_framebuffer *fb = &core_handoff->video.framebuffer;
    uint32_t from = UINT32_MAX;
    uint32_t to = 0;
    for (size_t y = r0; y < r1; y++) {
        uint32_t top;
        uint32_t bottom;
        uint32_t weight;
        uint32_t x0;
        uint32_t x1;
        if (row_span((uint32_t)y, &top, &bottom, &weight, &x0, &x1)) {
            from = x0 < from ? x0 : from;
            to = x1 > to ? x1 : to;
        }
    }
    struct rect dirty = { 0, 0, 0, 0 };
    if (from >= to) {
        return dirty;
    }
    size_t columns = to - from;
    for (size_t y = r0; y < r1; y++) {
        uint32_t top;
        uint32_t bottom;
        uint32_t weight;
        row_sources((uint32_t)y, &top, &bottom, &weight);
        uint32_t *band_row = s->band + (y - r0) * columns;
        const uint32_t *pixels = row_pixels(ops, s, top, bottom, weight, from, to, NULL);
        if (out.format == FORMAT_XBGR) {
            ops->to_xbgr(band_row, pixels + from, columns);
        } else if (out.format == FORMAT_RGB) {
            ops->copy(band_row, pixels + from, columns * sizeof(uint32_t));
        } else {
            for (size_t i = 0; i < columns; i++) {
                band_row[i] = fb_pixel(pixels[from + i]);
            }
        }
    }
    dirty = screen_rect(from, to, r0, r1);
    if (fb->bpp != 32) {
        // Pixels of other sizes one at a time.
        for (size_t y = r0; y < r1; y++) {
            for (size_t x = from; x < to; x++) {
                struct rect at = screen_rect(
                    (uint32_t)x, (uint32_t)x + 1, (uint32_t)y, (uint32_t)y + 1
                );
                back_put(
                    out.back + (size_t)at.y0 * fb->pitch, at.x0,
                    s->band[(y - r0) * columns + (x - from)], fb->bpp
                );
            }
        }
        return dirty;
    }
    // Column x of the display is a row of the framebuffer, and its rows run
    // along it: right to left turned clockwise, down the framebuffer's rows
    // and left to right turned anticlockwise, up them.
    ptrdiff_t pitch = (ptrdiff_t)(fb->pitch / sizeof(uint32_t));
    uint32_t *back = (uint32_t *)out.back;
    uint32_t rows = r1 - r0;
    if (fb->rotation == HANDOFF_ROTATE_90) {
        ops->transpose(
            back + (ptrdiff_t)dirty.y0 * pitch + dirty.x0, pitch,
            s->band + (size_t)(rows - 1) * columns, -(ptrdiff_t)columns, rows, columns
        );
    } else {
        ops->transpose(
            back + (ptrdiff_t)(dirty.y1 - 1) * pitch + dirty.x0, -pitch, s->band,
            (ptrdiff_t)columns, rows, columns
        );
    }
    return dirty;
}

static void draw_share(const struct vga_draw_ops *ops, struct vga_scratch *s, uint32_t share) {
    uint32_t r0 = share * SHARE_LINES;
    uint32_t r1 = r0 + SHARE_LINES < out.h ? r0 + SHARE_LINES : out.h;
    bool turned = core_handoff->video.framebuffer.rotation != HANDOFF_ROTATE_0;
    job.dirty[share] = turned ? draw_band(ops, s, r0, r1) : draw_rows(ops, s, r0, r1);
}

// Gets the copy ready once the last display row is drawn: of the whole
// framebuffer, of each share's rectangle, or of one around them all for a
// turned display, whose shares are the framebuffer's columns.
static void copy_ready(void) {
    const struct handoff_framebuffer *fb = &core_handoff->video.framebuffer;
    if (job.whole) {
        job.dirty[0] = (struct rect){ 0, 0, fb->width, fb->height };
        job.dirty_count = 1;
    } else if (fb->rotation != HANDOFF_ROTATE_0) {
        struct rect all = { 0, 0, 0, 0 };
        for (size_t i = 0; i < job.draw_chunks; i++) {
            rect_add(&all, job.dirty[i]);
        }
        job.dirty[0] = all;
        job.dirty_count = 1;
    } else {
        job.dirty_count = job.draw_chunks;
    }
    job.copy_rect = 0;
    job.copy_row = 0;
}

// The processor to copy the frame: the one that copies fastest of those whose
// vCPUs wait, as the processors that an OS for one processor never starts
// always do, else the one that copies fastest of all.
static struct cpu *copier_pick(void) {
    struct cpu *best = NULL;
    for (size_t i = 0; i < smp_cpu_count(); i++) {
        struct cpu *cpu = smp_cpu(i);
        if (cpu->vga_scratch == NULL || !atomic_load_explicit(&cpu->idle, memory_order_relaxed)) {
            continue;
        }
        if (best == NULL || cpu->draw_copy_ticks < best->draw_copy_ticks) {
            best = cpu;
        }
    }
    return best != NULL ? best : vga_draw_copier();
}

// Copies what the frame changed to the framebuffer from where the copy got to,
// a row at a time, until done or the processor's slice is over. Returns
// whether done.
static bool copy_on(const struct vga_draw_ops *ops) {
    const struct handoff_framebuffer *fb = &core_handoff->video.framebuffer;
    uint8_t *screen = (uint8_t *)(uintptr_t)fb->base;
    size_t size = fb->bpp / 8;
    bool copied = false;
    for (; job.copy_rect < job.dirty_count; job.copy_rect++) {
        struct rect r = job.dirty[job.copy_rect];
        if (r.x0 >= r.x1) {
            continue;
        }
        if (job.copy_row < r.y0) {
            job.copy_row = r.y0;
        }
        for (; job.copy_row < r.y1; job.copy_row++) {
            if (copied && cpu_slice_over()) {
                return false;
            }
            size_t offset = (size_t)job.copy_row * fb->pitch + r.x0 * size;
            ops->copy_fb(screen + offset, out.back + offset, (r.x1 - r.x0) * size);
            copied = true;
        }
        job.copy_row = 0;
    }
    return true;
}

// Whether this processor has a part in the frame left to take.
static bool work_left(void) {
    switch (atomic_load_explicit(&job.step, memory_order_acquire)) {
        case STEP_DRAW: {
            return atomic_load_explicit(&job.mark_next, memory_order_relaxed) < job.mark_chunks
                || atomic_load_explicit(&job.draw_next, memory_order_relaxed) < job.draw_chunks;
        }
        case STEP_COPY: {
            return this_cpu() == job.copier;
        }
        default: {
            return false;
        }
    }
}

// Takes part in the frame being drawn until no part is left for this processor
// or its slice is over. The processor that draws the last display row hands
// the copy to the one that copies fastest, which ends the frame.
static void work(void) {
    if (!work_left()) {
        return;
    }
    struct cpu *cpu = this_cpu();
    struct vga_scratch *s = cpu->vga_scratch;
    atomic_fetch_add_explicit(&job.busy, 1, memory_order_acq_rel);
    uint64_t start = time_ns();
    uint32_t generation = atomic_load_explicit(&job.generation, memory_order_acquire);
    if (s->generation != generation) {
        s->generation = generation;
        s->across_line[0] = UINT32_MAX;
        s->across_line[1] = UINT32_MAX;
    }
    const struct vga_draw_ops *ops = vga_draw_ops();
    simd_begin(vga_draw_level());
    bool done = false;
    for (bool first = true; ; first = false) {
        if (!first && cpu_slice_over()) {
            break;
        }
        vcpu_serve_pause();
        uint32_t step = atomic_load_explicit(&job.step, memory_order_acquire);
        if (step == STEP_COPY) {
            if (cpu == job.copier) {
                done = copy_on(ops);
            }
            break;
        }
        if (step != STEP_DRAW) {
            break;
        }
        uint32_t share = atomic_fetch_add_explicit(&job.mark_next, 1, memory_order_acq_rel);
        if (share < job.mark_chunks) {
            mark_share(ops, s, share);
            atomic_fetch_add_explicit(&job.mark_done, 1, memory_order_acq_rel);
            continue;
        }
        // The display rows wait for every source line, which others may still
        // be marking.
        if (atomic_load_explicit(&job.mark_done, memory_order_acquire) < job.mark_chunks) {
            cpu_relax();
            continue;
        }
        share = atomic_fetch_add_explicit(&job.draw_next, 1, memory_order_acq_rel);
        if (share >= job.draw_chunks) {
            break;
        }
        draw_share(ops, s, share);
        uint32_t drawn = atomic_fetch_add_explicit(&job.draw_done, 1, memory_order_acq_rel) + 1;
        if (drawn == job.draw_chunks) {
            copy_ready();
            job.copier = copier_pick();
            atomic_store_explicit(&job.step, STEP_COPY, memory_order_release);
            vcpu_wake(job.copier);
        }
    }
    simd_end();
    atomic_fetch_add_explicit(&out.frame_cost, time_ns() - start, memory_order_relaxed);
    if (done) {
        // A display's refresh at least, and REDRAW_SHARE times what the
        // processors took on it.
        uint64_t wait = atomic_load_explicit(&out.frame_cost, memory_order_relaxed) * REDRAW_SHARE;
        out.next_frame = out.frame_start + (wait > REFRESH_NS ? wait : REFRESH_NS);
        atomic_store_explicit(&job.step, STEP_IDLE, memory_order_release);
    } else if (work_left()) {
        // Back for the rest once the guest has had its turn.
        vcpu_exit_after_turn(cpu->vcpu);
    }
    atomic_fetch_sub_explicit(&job.busy, 1, memory_order_acq_rel);
}

// Starts the frame's steps, with the number of source lines to look at, every
// other processor kicked to take part where there are enough.
static void job_start(bool vbe_mode, bool all, uint32_t lines) {
    job.vbe = vbe_mode;
    job.all = all;
    job.mark_chunks = (out.height + SHARE_LINES - 1) / SHARE_LINES;
    job.draw_chunks = (out.h + SHARE_LINES - 1) / SHARE_LINES;
    atomic_store_explicit(&job.mark_next, 0, memory_order_relaxed);
    atomic_store_explicit(&job.mark_done, 0, memory_order_relaxed);
    atomic_store_explicit(&job.draw_next, 0, memory_order_relaxed);
    atomic_store_explicit(&job.draw_done, 0, memory_order_relaxed);
    atomic_fetch_add_explicit(&job.generation, 1, memory_order_acq_rel);
    atomic_store_explicit(&job.step, STEP_DRAW, memory_order_release);
    if (lines < SHARED_MIN) {
        return;
    }
    for (size_t i = 0; i < smp_cpu_count(); i++) {
        struct cpu *cpu = smp_cpu(i);
        if (cpu != this_cpu() && cpu->vga_scratch != NULL) {
            vcpu_wake(cpu);
        }
    }
}

// Abandons the frame being drawn, once no processor takes part in it any more.
static void job_abandon(void) {
    atomic_store_explicit(&job.step, STEP_IDLE, memory_order_release);
    atomic_fetch_add_explicit(&job.generation, 1, memory_order_acq_rel);
    while (atomic_load_explicit(&job.busy, memory_order_acquire) != 0) {
        vcpu_serve_pause();
        cpu_relax();
    }
}

// Starts showing the Bochs interface's display: every line where its mode or
// 8-bit palette changed, else the lines whose bytes changed, of those on pages
// written to where the dirty flags tell. False where no line can have.
static bool vbe_frame(uint64_t now) {
    if (vbe.direct) {
        return false;
    }
    spinlock_acquire(&vga.lock);
    uint32_t width = vbe.regs[DISPI_XRES];
    uint32_t height = vbe.regs[DISPI_YRES];
    uint16_t bpp = vbe.regs[DISPI_BPP];
    size_t line_bytes = vbe.line_bytes;
    uint32_t start = vbe.start;
    bool palette = vga.palette_dirty;
    vga.palette_dirty = false;
    vga.dirty = false;
    if (bpp == 8 && (palette || !out.vbe_valid)) {
        for (size_t i = 0; i < 256; i++) {
            out.vbe_palette[i] = dac_rgb((uint8_t)i);
        }
    }
    spinlock_release(&vga.lock);

    bool all = !out.vbe_valid || width != out.width || height != out.height || bpp != out.vbe_bpp
            || line_bytes != out.vbe_line_bytes || start != out.vbe_start || (bpp == 8 && palette);
    job.whole = false;
    if (all) {
        out.width = width;
        out.height = height;
        out.vbe_bpp = bpp;
        out.vbe_line_bytes = line_bytes;
        out.vbe_start = start;
        // Its pixels are square.
        place(width, height, width, height);
        simd_begin(vga_draw_level());
        border_fill(vga_draw_ops(), 0);
        simd_end();
        out.vbe_valid = true;
        out.valid = false;
    }
    // The guest's guests write to memory past the dirty flags of the
    // hypervisor's EPT or nested page tables, and after them every line needs
    // comparing once.
    bool track = vbe.track && !nvmx_any_vmx_operation() && !nsvm_any_svm();
    out.vbe_compare_all = !track || !out.vbe_tracked || now >= out.vbe_compare_due;
    if (out.vbe_compare_all) {
        out.vbe_compare_due = now + COMPARE_ALL_NS;
    }
    out.vbe_tracked = track;
    if (track) {
        vbe_take_written(start, (uint64_t)height * line_bytes);
    }
    uint32_t lines = height;
    if (!all && !out.vbe_compare_all) {
        lines = 0;
        size_t bytes = vbe_bytes();
        for (size_t y = 0; y < height; y++) {
            if (vbe_written(start + (uint64_t)y * line_bytes, bytes)) {
                lines++;
            }
        }
        if (lines == 0) {
            return false;
        }
    }
    job_start(true, all, lines);
    return true;
}

// Starts showing the VGA's display where anything changed. False where nothing
// did.
static bool vga_frame(uint64_t now) {
    uint64_t frames = now / frame_ns();
    bool cursor_on = ((frames >> CURSOR_FRAMES_SHIFT) & 1) == 0;
    bool blink_on = ((frames >> BLINK_FRAMES_SHIFT) & 1) == 0;
    uint32_t blink = (cursor_on ? (uint32_t)1 : (uint32_t)0)
                   | (blink_on ? (uint32_t)2 : (uint32_t)0);
    if (!vga.dirty && blink == out.blink && out.valid) {
        return false;
    }

    uint32_t width;
    uint32_t height;
    spinlock_acquire(&vga.lock);
    vga.dirty = false;
    struct display_state state;
    display_state(&state);
    // A text mode's font and its blinking show on every line, whatever bytes
    // it fetches.
    bool text = (vga.gc[GC_MISC] & GC_MISC_GRAPHICS) == 0;
    bool remake = !out.valid || memcmp(&state, &out.state, sizeof(struct display_state)) != 0
               || (text && (vga.font_changed || blink != out.blink));
    size_t made = frame_plan(&width, &height, cursor_on, blink_on, remake);
    out.state = state;
    out.blink = blink;
    memset(vga.changed, 0, sizeof(vga.changed));
    vga.font_changed = false;
    uint32_t border = dac_rgb(ac_colour_border());
    spinlock_release(&vga.lock);

    // All of it again where its size or border changed.
    bool all = !out.valid || width != out.width || height != out.height || border != out.border;
    job.whole = false;
    if (all) {
        out.width = width;
        out.height = height;
        out.border = border;
        // As a VGA monitor shows every mode, at 4:3.
        place(width, height, 4, 3);
        simd_begin(vga_draw_level());
        border_fill(vga_draw_ops(), border);
        simd_end();
        out.valid = true;
        out.vbe_valid = false;
    }
    if (made == 0 && !all) {
        return false;
    }
    job_start(false, all, (uint32_t)made);
    return true;
}

// Whether the frame under way shows what the display no longer is.
static bool job_stale(void) {
    bool vbe_mode = vbe_on();
    return job.vbe != vbe_mode || (vbe_mode && vbe_mode_changed());
}

// Abandons the frame under way where it no longer shows what the display is,
// and starts the next one once it is due.
static void frame_next(void) {
    if (vbe.leaving) {
        direct_leave();
        return;
    }
    uint64_t now = time_ns();
    uint32_t step = atomic_load_explicit(&job.step, memory_order_acquire);
    if (step != STEP_IDLE && job_stale()) {
        job_abandon();
        step = STEP_IDLE;
        out.next_frame = 0;
    }
    if (step != STEP_IDLE || now < out.next_frame) {
        return;
    }
    if (vbe.present) {
        direct_update();
    }
    atomic_store_explicit(&out.frame_cost, 0, memory_order_relaxed);
    out.frame_start = now;
    bool started = vbe_on() ? vbe_frame(now) : vga_frame(now);
    uint64_t setup = time_ns() - now;
    atomic_fetch_add_explicit(&out.frame_cost, setup, memory_order_relaxed);
    if (!started) {
        out.next_frame = now + REFRESH_NS;
    }
}

void vga_refresh(void) {
    // Processors this one paused may be in the middle of the frame, which
    // abandoning it would wait out.
    if (!vga.on || vcpu_others_paused()) {
        return;
    }
    if (this_cpu()->bsp) {
        frame_next();
    }
    work();
}

bool vga_redrawing(void) {
    return vga.on && work_left();
}

bool vga_emulated(void) {
    return vga.on;
}

uintptr_t vga_lfb(void) {
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
    vbe.track = gpt_track_dirty();
    if (vbe.track) {
        size_t bitmap_pages = page_count(vbe.vram_size / PAGE_SIZE / 8);
        vbe.written = page_alloc(bitmap_pages);
        vbe.written_before = page_alloc(bitmap_pages);
        vbe.leave_pages = page_alloc(bitmap_pages);
    }
    out.shadow = vbe.vram + vbe.vram_size;
    struct io_handler ports = { DISPI_PORT, DISPI_PORT_COUNT, dispi_ports, NULL, false };
    io_register(&ports);
    vbe.present = true;

    // Bochs' 32 bpp pixels hold blue, green, and red in their first three
    // bytes, and its scan lines are a multiple of 32 bytes. A turned picture
    // is never the framebuffer's own.
    const struct handoff_framebuffer *fb = &video->framebuffer;
    uint64_t frame = page_up((uint64_t)fb->pitch * fb->height);
    if (fb->present && fb->rotation == HANDOFF_ROTATE_0 && fb->bpp == 32 && fb->red_size == 8
     && fb->red_shift == 16 && fb->green_size == 8 && fb->green_shift == 8 && fb->blue_size == 8
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
    printf(
        "Emulated VGA's linear framebuffer: %llu MiB at %#llx, writes found by %s\n",
        (unsigned long long)(vbe.vram_size >> 20), (unsigned long long)vbe.lfb,
        vbe.track ? "dirty flags" : "comparing lines"
    );
}

static const char *const scaling_names[] = {
    "stretched", "kept at 4:3", "centered", "scaled", "scaled sharply"
};

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

size_t vga_lfb_size(void) {
    const struct handoff *h = core_handoff;
    if (!emulation_wanted() || !framebuffer_usable(&h->video.framebuffer)) {
        return 0;
    }
    return h->video.vram_size;
}

// Where nothing stands in for it, the card that has the legacy VGA's resources
// decodes its ports, which the guest then reaches.
#define REAL_MONOCHROME_BASE 0x3b0
#define REAL_MONOCHROME_COUNT 0x0c
#define REAL_COLOUR_BASE 0x3c0
#define REAL_COLOUR_COUNT 0x20
// QEMU's VGAs decode the Bochs display interface too, through which their
// Video BIOSes leave and set graphics modes, and its 16-bit data port reaches
// 1D0h.
#define REAL_DISPI_COUNT (DISPI_PORT_COUNT + 1)

struct pci_id {
    uint16_t vendor;
    uint16_t device;
};

static const struct pci_id real_dispi_vgas[] = {
    // QEMU standard VGA
    { 0x1234, 0x1111 },
    // virtio-vga
    { 0x1af4, 0x1050 },
    // QXL
    { 0x1b36, 0x0100 },
    // VMware SVGA II
    { 0x15ad, 0x0405 }
};

static bool decodes_dispi(uint16_t vendor, uint16_t device) {
    for (size_t i = 0; i < ARRAY_COUNT(real_dispi_vgas); i++) {
        if (real_dispi_vgas[i].vendor == vendor && real_dispi_vgas[i].device == device) {
            return true;
        }
    }
    return false;
}

static void give_real_vga(void) {
    const struct handoff_video *video = &core_handoff->video;
    if (video->vga_present) {
        io_pass(REAL_MONOCHROME_BASE, REAL_MONOCHROME_COUNT);
        io_pass(REAL_COLOUR_BASE, REAL_COLOUR_COUNT);
        if (decodes_dispi(video->vga_vendor_id, video->vga_device_id)) {
            io_pass(DISPI_PORT, REAL_DISPI_COUNT);
        }
    }
}

static struct vga_scratch *scratch_alloc(void) {
    uint32_t screen_width;
    uint32_t screen_height;
    vga_screen_size(&screen_width, &screen_height);
    size_t line_pages = page_count(screen_width * sizeof(uint32_t));
    struct vga_scratch *s = kmalloc(sizeof(struct vga_scratch));
    s->line = page_alloc(page_count((MAX_WIDTH + 2 * 9 * 2) * sizeof(uint32_t)));
    s->row = page_alloc(line_pages);
    s->source = page_alloc(page_count(DISPI_MAX_XRES * sizeof(uint32_t)));
    s->across[0] = page_alloc(line_pages);
    s->across[1] = page_alloc(line_pages);
    s->across_line[0] = UINT32_MAX;
    s->across_line[1] = UINT32_MAX;
    if (core_handoff->video.framebuffer.rotation != HANDOFF_ROTATE_0) {
        s->band = page_alloc(SHARE_LINES * line_pages);
    }
    return s;
}

bool vga_init(void) {
    const struct handoff *h = core_handoff;
    if (!emulation_wanted()) {
        give_real_vga();
        return false;
    }
    const struct handoff_framebuffer *fb = &h->video.framebuffer;
    if (!framebuffer_usable(fb)) {
        printf("No framebuffer to show an emulated VGA on\n");
        give_real_vga();
        return false;
    }

    if (h->video.back_base == 0) {
        printf("No memory for the emulated VGA's back buffer\n");
        give_real_vga();
        return false;
    }
    for (size_t p = 0; p < 4; p++) {
        vga.planes[p] = page_alloc(PLANE_SIZE / PAGE_SIZE);
        memset(vga.planes[p], 0, PLANE_SIZE);
    }
    size_t frame_pages = page_count(MAX_WIDTH * MAX_HEIGHT * sizeof(uint32_t));
    out.frame = page_alloc(frame_pages);
    out.shown = page_alloc(frame_pages);
    uint32_t screen_width;
    uint32_t screen_height;
    vga_screen_size(&screen_width, &screen_height);
    vga_draw_scale_alloc(&out.scale, screen_width);
    size_t lines = DISPI_MAX_YRES > MAX_HEIGHT ? DISPI_MAX_YRES : MAX_HEIGHT;
    out.changed_first = page_alloc(page_count(lines * sizeof(uint32_t)));
    out.changed_end = page_alloc(page_count(lines * sizeof(uint32_t)));
    job.dirty = page_alloc(page_count((screen_height / SHARE_LINES + 1) * sizeof(struct rect)));
    out.back = (uint8_t *)(uintptr_t)h->video.back_base;
    out.format = fb_format(fb);
    mm_write_combining(fb->base, (uint64_t)fb->pitch * fb->height);
    for (size_t b = 0; b < 256; b++) {
        uint64_t spread = 0;
        for (size_t k = 0; k < 8; k++) {
            spread |= (uint64_t)((b >> (7 - k)) & 1) << (8 * k);
        }
        plane_spread[b] = spread;
    }
    // The framebuffer's bottom rows time the copies to it.
    size_t rows = fb->height < SHARE_LINES ? fb->height : SHARE_LINES;
    uint8_t *bottom = (uint8_t *)(uintptr_t)fb->base + (uint64_t)(fb->height - rows) * fb->pitch;
    vga_draw_init(bottom, fb->pitch, rows);
    vga.pel_mask = 0xff;
    vga.misc = MISC_COLOR;

    // The video BIOS has ports among the VGA's, which its come before.
    vgabios_init();
    struct io_handler ports = { PORT_BASE, PORT_COUNT, ports_access, NULL, false };
    io_register(&ports);
    mmio_register(WINDOW_BASE, WINDOW_SIZE, &window_ops, NULL);
    gpt_map(WINDOW_BASE, 0, WINDOW_SIZE, 0, 0);
    gpt_flush();
    if (h->video.vram_size != 0) {
        vbe_init();
    }
    vga.on = true;
    this_cpu()->vga_scratch = scratch_alloc();

    unsigned int scaling = h->config.vga_scaling < 5 ? h->config.vga_scaling : 0;
    const char *orientation = "";
    switch (fb->rotation) {
        case HANDOFF_ROTATE_90: {
            orientation = ", turned clockwise";
            break;
        }
        case HANDOFF_ROTATE_270: {
            orientation = ", turned anticlockwise";
            break;
        }
    }
    printf(
        "Emulating a VGA, %s on the %ux%u framebuffer at %#llx%s%s\n", scaling_names[scaling],
        fb->width, fb->height, (unsigned long long)fb->base, orientation,
        h->config.vga_filter == HANDOFF_VGA_FILTER_BILINEAR ? ", with bilinear filtering" : ""
    );
    return true;
}

void vga_cpu_init(void) {
    if (!vga.on) {
        return;
    }
    vga_draw_cpu_init();
    this_cpu()->vga_scratch = scratch_alloc();
}
