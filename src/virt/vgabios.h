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

#ifndef VIRT__VGABIOS_H
#define VIRT__VGABIOS_H

// The emulated VGA's video BIOS: a small program in a ROM (vgabios_stub.S) and
// the rest here in the hypervisor, with each of its port and memory accesses
// carried out in the guest by that program, or in true real mode, where
// nothing can be virtualising the VGA, here at once.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Traps the ports the ROM asks through.
void vgabios_init(void);

// The ROM's image, with what is left of it to write being its checksum, and
// what the video BIOS knows of the display.
#define VGABIOS_ROM_SIZE 0x3400
void vgabios_rom_fill(uint8_t *image);

#define VGABIOS_SEGMENT 0xc000

// The BIOS data area, and what more than one part of the video BIOS reads.
#define BDA 0x0040
#define BDA_MODE 0x49
#define BDA_VIDEO_CONTROL 0x87
#define VIDEO_CONTROL_KEPT 0x80

// What the ROM holds besides the program, for callers that get far pointers.
#define ROM_FONT8 0x0800
#define ROM_FONT14 0x1000
#define ROM_FONT16 0x1e00
#define ROM_FUNCTIONALITY 0x2e00
#define ROM_OEM_STRING 0x2e10
#define ROM_VENDOR_NAME 0x2e20
#define ROM_PRODUCT_NAME 0x2e28
#define ROM_PRODUCT_REVISION 0x2e38
#define ROM_PALETTES 0x2e40

#define MODE_TEXT 0
#define MODE_CGA 1
#define MODE_CGA1 2
#define MODE_PLANAR 3
#define MODE_LINEAR 4

struct vgabios_mode {
    uint8_t mode;
    uint8_t kind;
    uint8_t columns;
    uint8_t rows;
    uint8_t char_height;
    uint8_t palette;
    uint16_t width;
    uint16_t height;
    uint16_t segment;
    uint16_t page_size;
    // Text modes come at 200, 350, and 400 lines.
    uint16_t scan_lines;
    uint8_t misc;
    // Sequencer registers 1 to 4.
    uint8_t seq[4];
    uint8_t crtc[25];
    // Attribute controller registers 0 to 13h.
    uint8_t ac[20];
    uint8_t gc[9];
};

struct vgabios_palette {
    const uint8_t *colours;
    size_t count;
};

extern const uint8_t vgabios_font8[256 * 8];
extern const uint8_t vgabios_font14[256 * 14];
extern const uint8_t vgabios_font16[256 * 16];
extern const struct vgabios_palette vgabios_palettes[];
extern const size_t vgabios_palette_count;
extern const size_t vgabios_mode_count;
extern const struct vgabios_mode vgabios_modes[];

// The caller's registers, of those the video BIOS takes or gives.
struct vgabios_regs {
    union {
        uint16_t ax;
        struct {
            uint8_t al;
            uint8_t ah;
        };
    };
    union {
        uint16_t bx;
        struct {
            uint8_t bl;
            uint8_t bh;
        };
    };
    union {
        uint16_t cx;
        struct {
            uint8_t cl;
            uint8_t ch;
        };
    };
    union {
        uint16_t dx;
        struct {
            uint8_t dl;
            uint8_t dh;
        };
    };
    uint16_t di;
    uint16_t bp;
    uint16_t es;
};

// What the video BIOS knows of the display it drives.
struct vgabios_display {
    // Whether the Bochs display interface is there.
    bool dispi;
    // The largest mode the display shows unscaled, the framebuffer's.
    uint16_t max_width;
    uint16_t max_height;
    uint16_t edid_size;
    // The linear framebuffer's physical address, or 0 if there is none.
    uint32_t lfb;
    uint8_t edid[256];
};

extern struct vgabios_display vgabios_display;

// Where the ROM keeps a palette, and where its entry points are.
uint16_t vgabios_palette_offset(size_t index);
uint16_t vgabios_int10_offset(void);
uint16_t vgabios_window_offset(void);

void vgabios_init_call(void);
void vgabios_int10(struct vgabios_regs *r);

void vgabios_set_mode(uint8_t mode);
// The DAC's colours of the standard 256-colour mode.
void vgabios_load_palette_256(void);

// The VBE functions, INT 10h AH=4Fh, and the window function they give a far
// pointer to, which far calls reach with the registers of AX=4F05h.
void vbe_int10(struct vgabios_regs *r);
// INT 10h AH=1Ch, the VGA's own form of VBE's state functions.
void vbe_vga_state(struct vgabios_regs *r);
// Turns the Bochs display interface off, for a VGA mode.
void vbe_disable(void);

// What the running call does in the guest. VGA ports only.
uint8_t bios_in(uint16_t port);
void bios_out(uint16_t port, uint8_t value);
// To the CRTC, sequencer, or graphics controller at their index ports.
void bios_out_indexed(uint16_t port, uint8_t index, uint8_t value);
// Bytes to one port.
void bios_out_block(uint16_t port, const uint8_t *bytes, size_t count);
// The DAC's colours from a place in the ROM, and black for those past count
// bytes.
void bios_dac_from_rom(uint16_t offset, size_t count);
uint8_t peekb(uint16_t segment, uint16_t offset);
uint16_t peekw(uint16_t segment, uint16_t offset);
void pokeb(uint16_t segment, uint16_t offset, uint8_t value);
void pokew(uint16_t segment, uint16_t offset, uint16_t value);
// An even number of bytes to memory.
void poke_block(uint16_t segment, uint16_t offset, const void *bytes, size_t count);
// Count bytes or words from segment:offset.
void far_fill8(uint16_t segment, uint16_t offset, uint8_t value, size_t count);
void far_fill16(uint16_t segment, uint16_t offset, uint16_t value, size_t count);
// Moves count bytes within a segment, overlapping or not.
void far_move(uint16_t segment, uint16_t to, uint16_t from, size_t count);
// A character map's height bytes per character to count cells of 32 bytes.
void far_font(
    uint16_t to_segment, uint16_t to, uint16_t from_segment, uint16_t from, uint8_t height,
    size_t count
);
// The caller's ECX.
uint32_t bios_ecx(const struct vgabios_regs *r);
// Lets the guest run if this call has had its time, as a wait is to.
void bios_relax(void);

#endif
