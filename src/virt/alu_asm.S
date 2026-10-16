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

/* The status flags: CF, PF, AF, ZF, SF, and OF. */
#define STATUS 0x8d5

    .text

    /*
     * uint64_t alu_run(unsigned stub, uint64_t dst, uint64_t src,
     *                  uint64_t *rflags)
     * Runs a stub on dst in RAX and src in RDX and CL, with the status flags
     * of *rflags, which it updates. The host's other flags stay its own.
     */
    .globl alu_run
    .type alu_run, @function
alu_run:
    movq %rcx, %r8
    leaq alu_stubs(%rip), %r11
    movl %edi, %eax
    shlq $4, %rax
    addq %rax, %r11
    movq %rsi, %rax
    movq %rdx, %rcx
    pushfq
    popq %r9
    andq $~STATUS, %r9
    movq (%r8), %r10
    andq $STATUS, %r10
    orq %r10, %r9
    pushq %r9
    popfq
    call *%r11
    pushfq
    popq %r9
    andq $STATUS, %r9
    movq (%r8), %r10
    andq $~STATUS, %r10
    orq %r9, %r10
    movq %r10, (%r8)
    ret
    .size alu_run, . - alu_run

/* Each stub takes 16 bytes, as alu_run() finds them by index. */
.macro STUB insn
0:
    \insn
    ret
    .org 0b + 16
.endm

.macro BINARY op
    STUB "\op\()b %dl, %al"
    STUB "\op\()w %dx, %ax"
    STUB "\op\()l %edx, %eax"
    STUB "\op\()q %rdx, %rax"
.endm

.macro SHIFT op
    STUB "\op\()b %cl, %al"
    STUB "\op\()w %cl, %ax"
    STUB "\op\()l %cl, %eax"
    STUB "\op\()q %cl, %rax"
.endm

.macro UNARY op
    STUB "\op\()b %al"
    STUB "\op\()w %ax"
    STUB "\op\()l %eax"
    STUB "\op\()q %rax"
.endm

/* Bit tests have no byte form. */
.macro BITTEST op
    STUB "ud2"
    STUB "\op\()w %dx, %ax"
    STUB "\op\()l %edx, %eax"
    STUB "\op\()q %rdx, %rax"
.endm

    /* In alu.h's order, four sizes each. */
    .balign 16
alu_stubs:
    BINARY add
    BINARY or
    BINARY adc
    BINARY sbb
    BINARY and
    BINARY sub
    BINARY xor
    BINARY cmp
    SHIFT rol
    SHIFT ror
    SHIFT rcl
    SHIFT rcr
    SHIFT shl
    SHIFT shr
    SHIFT sal
    SHIFT sar
    BINARY test
    UNARY not
    UNARY neg
    UNARY inc
    UNARY dec
    BITTEST bt
    BITTEST bts
    BITTEST btr
    BITTEST btc
