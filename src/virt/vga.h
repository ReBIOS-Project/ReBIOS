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

// The registers of the Bochs display interface, for the video BIOS.
uint16_t vga_dispi_read(uint16_t index);
void vga_dispi_write(uint16_t index, uint16_t value);

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
