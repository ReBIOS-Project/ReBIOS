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

#define HYPERCALL_TEST_DONE 0x5242fffe

    /*
     * A real-mode program, run at a segment boundary with CS at its start, that
     * reports through the serial port and a hypercall. On SVM the hypercall is
     * patched to VMMCALL.
     */
    .section .rodata
    .globl test_guest_start
    .globl test_guest_hypercall
    .globl test_guest_end
    .code16
test_guest_start:
    cli
    pushw %cs
    popw %ds
    movw $0x3f8, %dx
    movw $(.Lmsg - test_guest_start), %si
1:
    lodsb
    testb %al, %al
    jz 2f
    outb %al, %dx
    jmp 1b
2:
    movl $HYPERCALL_TEST_DONE, %eax
    movl $0x600d, %ebx
test_guest_hypercall:
    vmcall
3:
    hlt
    jmp 3b
.Lmsg:
    .asciz "Hello from the guest\r\n"
test_guest_end:
    .code64
