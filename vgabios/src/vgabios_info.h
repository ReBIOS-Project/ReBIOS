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

#ifndef VGABIOS_INFO_H
#define VGABIOS_INFO_H

#include <stdint.h>

// What the hypervisor fills in before the BIOS runs the video BIOS, at the
// offset the 16-bit word at offset VGABIOS_INFO_POINTER of the ROM gives.
#define VGABIOS_INFO_POINTER 0x08

// The Bochs display interface is there.
#define VGABIOS_INFO_DISPI 0x0001

struct vgabios_info {
    uint16_t flags;
    // The emulated display controller, as PCI bus and device-function in its
    // high and low bytes, or 0xffff if there is none.
    uint16_t pci_bdf;
    // The largest mode the display shows unscaled, the framebuffer's.
    uint16_t max_width;
    uint16_t max_height;
    uint16_t edid_size;
    uint8_t edid[256];
};

#endif
