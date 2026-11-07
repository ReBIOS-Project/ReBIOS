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

#ifndef VIRT__DISPI_H
#define VIRT__DISPI_H

// The Bochs display interface's registers, at an index port and a data port,
// with QEMU's include/hw/display/bochs-vbe.h values and semantics.
#define DISPI_PORT 0x1ce
#define DISPI_PORT_COUNT 2
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

#endif
