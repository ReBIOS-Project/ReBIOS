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

#ifndef VIRT__VGA_H
#define VIRT__VGA_H

#include <stdbool.h>

// Gives the guest a VGA of its own when it has no usable video BIOS, or the
// configuration asks for one: its registers, its memory window, and its
// display, drawn on the firmware's framebuffer. Needs layout_init(). Returns
// whether the VGA is the hypervisor's.
bool vga_init(void);
bool vga_emulated(void);

// Redraws what the guest changed, at most as often as a display refreshes.
// Only the boot processor draws.
void vga_refresh(void);

#endif
