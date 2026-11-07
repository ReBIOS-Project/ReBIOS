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

#ifndef VIRT__BDA_H
#define VIRT__BDA_H

// The BIOS data area's segment, and the fields the video BIOS keeps there, as
// plain numbers, which vgabios_stub.S takes too.
#define BDA 0x40
#define BDA_EQUIPMENT 0x10
#define BDA_MODE 0x49
#define BDA_COLUMNS 0x4a
#define BDA_PAGE_SIZE 0x4c
#define BDA_PAGE_START 0x4e
#define BDA_CURSOR 0x50
#define BDA_CURSOR_TYPE 0x60
#define BDA_PAGE 0x62
#define BDA_CRTC 0x63
#define BDA_MODE_CONTROL 0x65
#define BDA_PALETTE 0x66
#define BDA_ROWS 0x84
#define BDA_CHAR_HEIGHT 0x85
#define BDA_VIDEO_CONTROL 0x87
#define BDA_SWITCHES 0x88
#define BDA_VGA_FLAGS 0x89
#define BDA_DCC 0x8a

#define BDA_VIDEO_CONTROL_NO_EMULATION 0x01
#define BDA_VIDEO_CONTROL_KEPT 0x80

#define BDA_VGA_FLAGS_GREY 0x02
#define BDA_VGA_FLAGS_NO_PALETTE 0x08
#define BDA_VGA_FLAGS_400 0x10
#define BDA_VGA_FLAGS_200 0x80

#endif
