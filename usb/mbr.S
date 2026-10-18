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

// The ReBIOS USB stick's master boot record. UEFI firmware boots ReBIOS from
// the stick's FAT partition; a legacy BIOS, or ReBIOS's own legacy BIOS when
// asked to, runs this, which says what the stick is and has the BIOS try its
// next drive, through INT 18h (BIOS Boot Specification 1.01, 6.5.2).

    .code16
    .text
    .globl _start
_start:
    // Some BIOSes boot a USB stick only with a jump and a BPB in front, and
    // some write into that BPB in memory, so the code starts past it.
    .byte 0xeb, skip_bpb - _start - 2
    nop
    .ascii "REBIOS  "
    .word 512
    .byte 0
    .word 0
    .byte 0
    .word 0
    .word 0
    .byte 0
    .word 0
    .word 18
    .word 2
    .long 0
    .long 0
    .byte 0
    .byte 0
    .byte 0
    .long 0
    .ascii "REBIOS     "
    .fill 8, 1, 0

skip_bpb:
    cli
    cld
    // Some BIOSes jump to 07C0:0000 rather than 0000:7C00.
    ljmp $0, $(1f - _start + 0x7c00)
1:
    xor %ax, %ax
    mov %ax, %ds
    mov %ax, %ss
    mov $0x7c00, %sp
    sti
    mov $(message - _start + 0x7c00), %si
1:
    lodsb
    test %al, %al
    jz 2f
    mov $0x0e, %ah
    mov $0x0007, %bx
    int $0x10
    jmp 1b
2:
    int $0x18
3:
    hlt
    jmp 3b

    // Windows 95B, 98, and ME keep a disk timestamp here.
    .org 0xda
    .fill 6, 1, 0

message:
    .ascii "This is a ReBIOS USB stick: boot it as a UEFI device to run ReBIOS,\r\n"
    .asciz "which then boots the computer's other drives.\r\n"

    // One partition, from 1 MiB to the end of the 8 MiB image, with its CHS
    // addresses at 255 heads and 63 sectors. It is an EFI system partition
    // (EFh, UEFI 2.11, 5.2.2) rather than a FAT one, so that DOS and Windows
    // give it no drive letter, which would move theirs.
    .org 446
    .byte 0x00
    .byte 0x20, 0x21, 0x00
    .byte 0xef
    .byte 0x05, 0x04, 0x01
    .long 2048
    .long 14336

    .org 510
    .byte 0x55, 0xaa
