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

.text

// cpu_switch_stack(stack_top, fn, arg) calls fn(arg) on the stack that ends at
// stack_top, never to come back.
.globl cpu_switch_stack
.type cpu_switch_stack, %function
cpu_switch_stack:
    movq %rdi, %rsp
    xorl %ebp, %ebp
    movq %rdx, %rdi
    // Keep the ABI's stack alignment for fn, as if it had been called.
    pushq $0
    jmpq *%rsi
.size cpu_switch_stack, . - cpu_switch_stack

// cpu_resume_save(resume) saves where its caller goes on and returns 0; an
// exception the host recovers from by resuming there makes it return 1.
.globl cpu_resume_save
.type cpu_resume_save, %function
cpu_resume_save:
    movq %rbx, CPU_RESUME_RBX(%rdi)
    movq %rbp, CPU_RESUME_RBP(%rdi)
    movq %r12, CPU_RESUME_R12(%rdi)
    movq %r13, CPU_RESUME_R13(%rdi)
    movq %r14, CPU_RESUME_R14(%rdi)
    movq %r15, CPU_RESUME_R15(%rdi)
    leaq 8(%rsp), %rax
    movq %rax, CPU_RESUME_RSP(%rdi)
    movq (%rsp), %rax
    movq %rax, CPU_RESUME_RIP(%rdi)
    pushfq
    popq CPU_RESUME_RFLAGS(%rdi)
    xorl %eax, %eax
    retq
.size cpu_resume_save, . - cpu_resume_save

// cpu_halt() halts until an interrupt, unless the processor has been kicked.
// The host's NMI handler restarts an NMI that arrives before the HLT, as it
// would halt with the NMI's work left undone otherwise.
.globl cpu_halt
.globl cpu_halt_restart_end
.type cpu_halt, %function
cpu_halt:
    movq %rsp, %rax
    andq $-CPU_AREA_SIZE, %rax
    movq (%rax), %rax
    cmpb $0, CPU_KICKED(%rax)
    jne .Lkicked
    sti
    hlt
cpu_halt_restart_end:
.Lkicked:
    cli
    retq
.size cpu_halt, . - cpu_halt

// Exception entry stubs: a dummy error code where the CPU pushes none.
.macro exc_stub vector, has_error
.balign 16
exc_stub_\vector:
.if \has_error == 0
    pushq $0
.endif
    pushq $\vector
    jmp exception_common
.endm

    exc_stub 0, 0
    exc_stub 1, 0
    exc_stub 2, 0
    exc_stub 3, 0
    exc_stub 4, 0
    exc_stub 5, 0
    exc_stub 6, 0
    exc_stub 7, 0
    exc_stub 8, 1
    exc_stub 9, 0
    exc_stub 10, 1
    exc_stub 11, 1
    exc_stub 12, 1
    exc_stub 13, 1
    exc_stub 14, 1
    exc_stub 15, 0
    exc_stub 16, 0
    // #AC, #CP, and #VC never happen in the host: these are interrupts.
    exc_stub 17, 0
    exc_stub 18, 0
    exc_stub 19, 0
    exc_stub 20, 0
    exc_stub 21, 0
    exc_stub 22, 0
    exc_stub 23, 0
    exc_stub 24, 0
    exc_stub 25, 0
    exc_stub 26, 0
    exc_stub 27, 0
    exc_stub 28, 0
    exc_stub 29, 0
    exc_stub 30, 1
    exc_stub 31, 0

// Interrupt stubs for vectors 32 to 255, 16 bytes apart.
.balign 16
.globl interrupt_stubs
.type interrupt_stubs, %function
interrupt_stubs:
.set vector, 32
.rept 224
.balign 16
    pushq $0
    pushq $vector
    jmp exception_common
.set vector, vector + 1
.endr
.size interrupt_stubs, . - interrupt_stubs

.type exception_common, %function
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
    // The frame is not 16-byte aligned in general.
    movq %rsp, %rbx
    andq $-16, %rsp
    callq cpu_exception_handler
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
.size exception_common, . - exception_common

.section .rodata
.balign 4
.globl exception_stubs
.type exception_stubs, %object
exception_stubs:
.irp vector, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
.long exc_stub_\vector - exception_stubs
.endr
.size exception_stubs, . - exception_stubs
