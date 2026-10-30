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

/* Offsets into struct gprs. RAX and RSP live in the VMCB. */
#define GPR_RCX 8
#define GPR_RDX 16
#define GPR_RBX 24
#define GPR_RBP 40
#define GPR_RSI 48
#define GPR_RDI 56
#define GPR_R8 64
#define GPR_R9 72
#define GPR_R10 80
#define GPR_R11 88
#define GPR_R12 96
#define GPR_R13 104
#define GPR_R14 112
#define GPR_R15 120

    .text

    /*
     * void svm_enter(uintptr_t vmcb_pa, struct gprs *gprs)
     *
     * Runs the guest until its next #VMEXIT, with the state VMLOAD and VMSAVE
     * cover loaded from and saved to the VMCB, as nothing the host does relies
     * on that state.
     */
    .globl svm_enter
    .type svm_enter, @function
svm_enter:
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    pushq %rsi

    movq %rdi, %rax
    movq GPR_RCX(%rsi), %rcx
    movq GPR_RDX(%rsi), %rdx
    movq GPR_RBX(%rsi), %rbx
    movq GPR_RBP(%rsi), %rbp
    movq GPR_RDI(%rsi), %rdi
    movq GPR_R8(%rsi), %r8
    movq GPR_R9(%rsi), %r9
    movq GPR_R10(%rsi), %r10
    movq GPR_R11(%rsi), %r11
    movq GPR_R12(%rsi), %r12
    movq GPR_R13(%rsi), %r13
    movq GPR_R14(%rsi), %r14
    movq GPR_R15(%rsi), %r15
    movq GPR_RSI(%rsi), %rsi

    /*
     * With V_INTR_MASKING, the host's IF at VMRUN gates physical interrupts in
     * the guest; GIF holds them off in the host meanwhile.
     */
    clgi
    sti
    vmload %rax
    vmrun %rax
    vmsave %rax
    cli

    /* RAX holds the VMCB address again, so it is free to address the gprs. */
    movq (%rsp), %rax
    movq %rcx, GPR_RCX(%rax)
    movq %rdx, GPR_RDX(%rax)
    movq %rbx, GPR_RBX(%rax)
    movq %rbp, GPR_RBP(%rax)
    movq %rsi, GPR_RSI(%rax)
    movq %rdi, GPR_RDI(%rax)
    movq %r8, GPR_R8(%rax)
    movq %r9, GPR_R9(%rax)
    movq %r10, GPR_R10(%rax)
    movq %r11, GPR_R11(%rax)
    movq %r12, GPR_R12(%rax)
    movq %r13, GPR_R13(%rax)
    movq %r14, GPR_R14(%rax)
    movq %r15, GPR_R15(%rax)

    addq $8, %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp
    ret
    .size svm_enter, . - svm_enter
