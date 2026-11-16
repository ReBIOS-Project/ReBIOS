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
// You should have received a copy of the GNU General Public License along with
// ReBIOS. If not, see <https://www.gnu.org/licenses/>.

.section .text

// thread_swap(save_rsp, rsp) leaves the callee-saved registers on this stack,
// and its pointer in *save_rsp, then carries on from rsp, as another
// thread_swap() left it. A call keeps nothing else: the hypervisor has no x87
// or vector state.
.globl thread_swap
.type thread_swap, %function
thread_swap:
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq %rsp, (%rdi)
    movq %rsi, %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp
    retq
.size thread_swap, . - thread_swap

// Where a thread starts, with itself in %rbx.
.globl thread_entry
.type thread_entry, %function
thread_entry:
    movq %rbx, %rdi
    callq thread_main
    ud2
.size thread_entry, . - thread_entry
