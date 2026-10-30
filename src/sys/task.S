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

    .text

    /*
     * void task_swap(uintptr_t *save_rsp, uintptr_t rsp)
     * Leaves the callee-saved registers on this stack, and its pointer in
     * *save_rsp, then carries on from rsp, as another task_swap() left it. A
     * call keeps nothing else: the hypervisor has no x87 or vector state.
     */
    .globl task_swap
    .type task_swap, @function
task_swap:
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
    ret
    .size task_swap, . - task_swap

    /* Where a task starts, with itself in %rbx. */
    .globl task_entry
    .type task_entry, @function
task_entry:
    movq %rbx, %rdi
    call task_main
    ud2
    .size task_entry, . - task_entry
