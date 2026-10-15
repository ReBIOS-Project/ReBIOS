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

    .text

    /* void switch_stack(uint64_t stack_top, void (*fn)(void *), void *arg) */
    .globl switch_stack
    .type switch_stack, @function
switch_stack:
    movq %rdi, %rsp
    xorl %ebp, %ebp
    movq %rdx, %rdi
    /* Keep the ABI's stack alignment for fn, as if it had been called. */
    pushq $0
    jmpq *%rsi
    .size switch_stack, . - switch_stack

    /* Exception entry stubs: a dummy error code where the CPU pushes none. */
.macro EXC_STUB vector, has_error
    .balign 16
exc_stub_\vector:
    .if \has_error == 0
    pushq $0
    .endif
    pushq $\vector
    jmp exception_common
.endm

EXC_STUB 0, 0
EXC_STUB 1, 0
EXC_STUB 2, 0
EXC_STUB 3, 0
EXC_STUB 4, 0
EXC_STUB 5, 0
EXC_STUB 6, 0
EXC_STUB 7, 0
EXC_STUB 8, 1
EXC_STUB 9, 0
EXC_STUB 10, 1
EXC_STUB 11, 1
EXC_STUB 12, 1
EXC_STUB 13, 1
EXC_STUB 14, 1
EXC_STUB 15, 0
EXC_STUB 16, 0
/* #AC, #CP, and #VC never happen in the host: these are interrupts. */
EXC_STUB 17, 0
EXC_STUB 18, 0
EXC_STUB 19, 0
EXC_STUB 20, 0
EXC_STUB 21, 0
EXC_STUB 22, 0
EXC_STUB 23, 0
EXC_STUB 24, 0
EXC_STUB 25, 0
EXC_STUB 26, 0
EXC_STUB 27, 0
EXC_STUB 28, 0
EXC_STUB 29, 0
EXC_STUB 30, 1
EXC_STUB 31, 0

    /* Interrupt stubs for vectors 32 to 255, 16 bytes apart. */
    .balign 16
    .globl interrupt_stubs
interrupt_stubs:
    .set vector, 32
    .rept 224
    .balign 16
    pushq $0
    pushq $vector
    jmp exception_common
    .set vector, vector + 1
    .endr

exception_common:
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    cld
    movq %rsp, %rdi
    /* The frame is not 16-byte aligned in general. */
    movq %rsp, %rbx
    andq $-16, %rsp
    call exception_handler
    movq %rbx, %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax
    addq $16, %rsp
    iretq

    .section .rodata
    .balign 4
    .globl exception_stubs
exception_stubs:
    .irp vector, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
    .long exc_stub_\vector - exception_stubs
    .endr
