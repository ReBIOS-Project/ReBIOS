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

#ifndef VIRT__VGABIOS_STUB_H
#define VIRT__VGABIOS_STUB_H

// What vgabios.c and the code of the video BIOS in vgabios_stub.S agree on, as
// plain numbers, which the stub takes too.

// A word read from the first port starts a call, and tells it its own port
// among the next ones, if the ROM's word came first: other software that reads
// ports to see what is there finds nothing.
#define VGABIOS_STUB_PORT_ALLOCATE 0x1d0
#define VGABIOS_STUB_KNOCK 0x5242
#define VGABIOS_STUB_KNOCK_INIT 0x5243

// Where those do not reach the hypervisor, as from Windows NT's DOS boxes,
// which let a program have only the VGA's ports, two that no VGA has a use
// for: one to start with, and one more call's own.
#define VGABIOS_STUB_PORT_VGA 0x3dc
#define VGABIOS_STUB_PORT_VGA_COUNT 4
#define VGABIOS_STUB_PORT_VGA_CALL_OFFSET 0
#define VGABIOS_STUB_PORT_VGA_ALLOCATE_OFFSET 2

// Where the stub saves the registers: from the frame BP gives.
#define VGABIOS_STUB_FRAME_AX 0
#define VGABIOS_STUB_FRAME_BX 2
#define VGABIOS_STUB_FRAME_BH 3
#define VGABIOS_STUB_FRAME_CX 4
#define VGABIOS_STUB_FRAME_DX 6
#define VGABIOS_STUB_FRAME_DI 10
#define VGABIOS_STUB_FRAME_BP 12
#define VGABIOS_STUB_FRAME_ES 16

// The operations that the hypervisor's words name: by number below the indexed
// registers', which the stub's table of them follows, and by class from them
// on, a register's index or a port's offset from VGABIOS_STUB_VGA_PORTS in the
// low bits.
#define VGABIOS_STUB_OP_DONE 0
#define VGABIOS_STUB_OP_NOTHING 1
#define VGABIOS_STUB_OP_REGISTERS 2
#define VGABIOS_STUB_OP_SET_REGISTER 3
#define VGABIOS_STUB_OP_BDA_READ8 4
#define VGABIOS_STUB_OP_BDA_READ16 5
#define VGABIOS_STUB_OP_BDA_WRITE8 6
#define VGABIOS_STUB_OP_BDA_WRITE16 7
#define VGABIOS_STUB_OP_READ8 8
#define VGABIOS_STUB_OP_READ16 9
#define VGABIOS_STUB_OP_WRITE8 10
#define VGABIOS_STUB_OP_WRITE16 11
#define VGABIOS_STUB_OP_FILL8 12
#define VGABIOS_STUB_OP_FILL16 13
#define VGABIOS_STUB_OP_COPY 14
#define VGABIOS_STUB_OP_FONT 15
#define VGABIOS_STUB_OP_OUT_BLOCK 16
#define VGABIOS_STUB_OP_WRITE_BLOCK 17
#define VGABIOS_STUB_OP_READ_BLOCK 18
#define VGABIOS_STUB_OP_ECX_HIGH 19
#define VGABIOS_STUB_OP_DAC_ROM 20
#define VGABIOS_STUB_OP_INDEXED_CRTC 0x20
#define VGABIOS_STUB_OP_INDEXED_CRTC_MONO 0x40
#define VGABIOS_STUB_OP_INDEXED_SEQ 0x60
#define VGABIOS_STUB_OP_INDEXED_GC 0x70
#define VGABIOS_STUB_OP_VGA_IN 0x80
#define VGABIOS_STUB_OP_VGA_OUT 0xc0
#define VGABIOS_STUB_VGA_PORTS 0x3b0

#endif
