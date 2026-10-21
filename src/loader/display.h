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

#ifndef LOADER__DISPLAY_H
#define LOADER__DISPLAY_H

#include <stdbool.h>

// Whether the core will emulate the VGA, as its vga_init() decides. Needs
// video_init().
bool display_vga_emulated(void);

// Reserves the emulated VGA's video memory, if the VGA is to be emulated, and
// records the display's EDID. Needs video_init().
void display_collect(void);

#endif
