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

// The ROM's header and its entry points, which set up what the C code
// expects (vgabios.h) and call it with 32-bit returns, as -m16 code has.
    .code16

    .section .header, "ax"
    .globl vgabios_header
vgabios_header:
    .byte 0x55, 0xaa
    .byte vgabios_blocks
    // The initialisation entry point, which the BIOS calls far at offset 3.
    .byte 0xe9
    .word init_entry - (. + 2)
    // Software looks for an IBM video BIOS by this signature.
    .org 0x1e
    .ascii "IBM"

    .section .text.entry, "ax"
init_entry:
    pushal
    pushw %ds
    pushw %es
    pushw %fs
    pushw %gs
    movw %ss, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %cs, %ax
    movw %ax, %fs
    movzwl %sp, %esp
    cld
    calll vgabios_init
    popw %gs
    popw %fs
    popw %es
    popw %ds
    popal
    lretw

    .globl vgabios_int10_entry
vgabios_int10_entry:
    pushal
    pushw %ds
    pushw %es
    pushw %fs
    pushw %gs
    movw %ss, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %cs, %ax
    movw %ax, %fs
    movzwl %sp, %esp
    movl %esp, %eax
    cld
    calll vgabios_int10
    popw %gs
    popw %fs
    popw %es
    popw %ds
    popal
    iretw
