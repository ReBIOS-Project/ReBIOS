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

// The status flags: CF, PF, AF, ZF, SF, and OF.
#define STATUS 0x8d5

.text

// alu_run(stub, dst, src, rflags) runs a stub on dst in RAX and src in RDX and
// CL, with the status flags of *rflags, which it updates. The host's other
// flags stay its own.
.globl alu_run
.type alu_run, %function
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
    callq *%r11
    pushfq
    popq %r9
    andq $STATUS, %r9
    movq (%r8), %r10
    andq $~STATUS, %r10
    orq %r9, %r10
    movq %r10, (%r8)
    retq
.size alu_run, . - alu_run

// Each stub takes 16 bytes, as alu_run() finds them by index.
.macro stub insn
.Lstub\@:
    \insn
    retq
.org .Lstub\@ + 16
.endm

.macro binary op
    stub "\op\()b %dl, %al"
    stub "\op\()w %dx, %ax"
    stub "\op\()l %edx, %eax"
    stub "\op\()q %rdx, %rax"
.endm

.macro shift op
    stub "\op\()b %cl, %al"
    stub "\op\()w %cl, %ax"
    stub "\op\()l %cl, %eax"
    stub "\op\()q %cl, %rax"
.endm

.macro unary op
    stub "\op\()b %al"
    stub "\op\()w %ax"
    stub "\op\()l %eax"
    stub "\op\()q %rax"
.endm

// Bit tests have no byte form.
.macro bittest op
    stub "ud2"
    stub "\op\()w %dx, %ax"
    stub "\op\()l %edx, %eax"
    stub "\op\()q %rdx, %rax"
.endm

// In alu.h's order, four sizes each.
.balign 16
.type alu_stubs, %function
alu_stubs:
    binary add
    binary or
    binary adc
    binary sbb
    binary and
    binary sub
    binary xor
    binary cmp
    shift rol
    shift ror
    shift rcl
    shift rcr
    shift shl
    shift shr
    shift sal
    shift sar
    binary test
    unary not
    unary neg
    unary inc
    unary dec
    bittest bt
    bittest bts
    bittest btr
    bittest btc
.size alu_stubs, . - alu_stubs
