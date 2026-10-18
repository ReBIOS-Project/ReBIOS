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

#ifndef VIRT__EDID_H
#define VIRT__EDID_H

#include <stdint.h>
#include <stdbool.h>

#define EDID_BLOCK_SIZE 128

// An EDID base block (E-EDID A2, EDID 1.4) for the emulated VGA's display,
// for when the firmware has none: width by height pixels at 60 Hz, as its
// preferred timing. Returns false if a detailed timing cannot describe that.
bool edid_synthesize(uint8_t *edid, uint32_t width, uint32_t height);

#endif
