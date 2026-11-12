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

#ifndef VIRT__VGA_H
#define VIRT__VGA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Gives the guest a VGA of its own when it has no usable video BIOS, or the
// configuration asks for one: its registers, its memory window, and its
// display, drawn on the firmware's framebuffer. Needs layout_init(). Returns
// whether the VGA is the hypervisor's.
bool vga_init(void);
// On each other processor as it starts, after vga_init(): what it draws with.
void vga_cpu_init(void);
bool vga_emulated(void);
// Whether size bytes of ports from port on are all the emulated VGA's.
bool vga_port_emulated(uint16_t port, size_t size);

// The largest VBE mode the emulated VGA shows.
#define VGA_VBE_MAX_WIDTH 16000
#define VGA_VBE_MAX_HEIGHT 12000

// The emulated VGA's VBE mode, as the video BIOS sets it.
struct vga_vbe {
    // Whether a VBE mode is on, rather than one of the VGA's own.
    bool on;
    // Whether its mode set asked for the linear framebuffer, and to keep video
    // memory.
    bool lfb;
    bool keep;
    uint16_t width;
    uint16_t height;
    uint16_t bpp;
    // The scan line in pixels, how many of them video memory holds, and the
    // display start.
    uint16_t virt_width;
    uint16_t virt_height;
    uint16_t x_offset;
    uint16_t y_offset;
    // The 64 KiB bank of video memory that the window at A0000h shows.
    uint16_t bank;
};

// The video BIOS's way to the VBE modes, each value brought back in range.
void vga_vbe_get(struct vga_vbe *mode);
// Sets a mode, its scan line as wide as it and its display start at the top,
// with the DAC back to 6 bits per primary, clearing video memory unless keep.
void vga_vbe_set_mode(uint16_t width, uint16_t height, uint16_t bpp, bool lfb, bool keep);
// Leaves the VBE modes for the VGA's own, with the DAC back to 6 bits.
void vga_vbe_set_off(void);
void vga_vbe_set_bank(uint16_t bank);
void vga_vbe_set_virt_width(uint16_t width);
void vga_vbe_set_start(uint16_t x, uint16_t y);
// Back to a mode that vga_vbe_get() gave, video memory as it is.
void vga_vbe_restore(const struct vga_vbe *mode);
// The size of the VBE modes' video memory, 0 where there is none.
size_t vga_vbe_vram_size(void);

// Whether the DAC takes 8 bits per primary rather than 6.
bool vga_dac8(void);
void vga_set_dac8(bool dac8);

// The size of the linear framebuffer vga_init() is to give the guest, 0 if it
// is to give none.
size_t vga_lfb_size(void);

// Where the guest finds the emulated VGA's linear framebuffer, or 0 if it has
// none.
uintptr_t vga_lfb(void);

// The framebuffer's size as the emulated VGA's picture stands on it.
void vga_screen_size(uint32_t *width, uint32_t *height);

// Redraws what the guest changed, at most as often as a display refreshes, a
// slice at a time, each processor taking part.
void vga_refresh(void);
// Whether a redraw has a part left for this processor, which vga_refresh()
// carries on with.
bool vga_redrawing(void);

#endif
