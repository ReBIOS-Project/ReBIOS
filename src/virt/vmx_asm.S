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

#include <sys/cpu.h>

/* Offsets into struct gprs. */
#define GPR_RAX 0
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

#define VMCS_HOST_RSP 0x6c14

    .text

    /*
     * int vmx_enter(struct gprs *gprs, int launched)
     *
     * Returns 0 after a VM exit, with the guest's registers in *gprs, 1 if the
     * VM entry failed, or 2 if an NMI reached the host before it.
     */
    .globl vmx_enter
    .type vmx_enter, @function
vmx_enter:
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    pushq %rsi
    /* Where vmx_exit_entry finds it, at the host RSP. */
    pushq %rdi
    movq $VMCS_HOST_RSP, %rax
    vmwrite %rsp, %rax

    /*
     * The host's NMI handler restarts an NMI that arrives from here to the
     * VM entry here, as the kick it may be would be left unseen otherwise.
     */
    .globl vmx_enter_restart
vmx_enter_restart:
    movq %rsp, %rax
    andq $-CPU_AREA_SIZE, %rax
    movq (%rax), %rax
    cmpb $0, CPU_KICKED(%rax)
    jne 3f
    movq (%rsp), %rdi
    /* The moves below leave the flags alone. */
    cmpl $0, 8(%rsp)

    movq GPR_RAX(%rdi), %rax
    movq GPR_RCX(%rdi), %rcx
    movq GPR_RDX(%rdi), %rdx
    movq GPR_RBX(%rdi), %rbx
    movq GPR_RBP(%rdi), %rbp
    movq GPR_RSI(%rdi), %rsi
    movq GPR_R8(%rdi), %r8
    movq GPR_R9(%rdi), %r9
    movq GPR_R10(%rdi), %r10
    movq GPR_R11(%rdi), %r11
    movq GPR_R12(%rdi), %r12
    movq GPR_R13(%rdi), %r13
    movq GPR_R14(%rdi), %r14
    movq GPR_R15(%rdi), %r15
    movq GPR_RDI(%rdi), %rdi

    jne 1f
    vmlaunch
    jmp 2f
1:
    vmresume
    .globl vmx_enter_restart_end
vmx_enter_restart_end:
2:
    movl $1, %eax
    jmp 4f
3:
    movl $2, %eax
4:
    addq $16, %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp
    ret
    .size vmx_enter, . - vmx_enter

    .globl vmx_exit_entry
    .type vmx_exit_entry, @function
vmx_exit_entry:
    pushq %rdi
    movq 8(%rsp), %rdi
    movq %rax, GPR_RAX(%rdi)
    movq %rcx, GPR_RCX(%rdi)
    movq %rdx, GPR_RDX(%rdi)
    movq %rbx, GPR_RBX(%rdi)
    movq %rbp, GPR_RBP(%rdi)
    movq %rsi, GPR_RSI(%rdi)
    movq %r8, GPR_R8(%rdi)
    movq %r9, GPR_R9(%rdi)
    movq %r10, GPR_R10(%rdi)
    movq %r11, GPR_R11(%rdi)
    movq %r12, GPR_R12(%rdi)
    movq %r13, GPR_R13(%rdi)
    movq %r14, GPR_R14(%rdi)
    movq %r15, GPR_R15(%rdi)
    popq %rax
    movq %rax, GPR_RDI(%rdi)
    addq $16, %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp
    xorl %eax, %eax
    ret
    .size vmx_exit_entry, . - vmx_exit_entry
