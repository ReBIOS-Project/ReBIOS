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

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/thread.h>
#include <lib/log.h>
#include <lib/time.h>
#include <sys/cpu.h>
#include <sys/mm.h>

void thread_swap(uintptr_t *save_rsp, uintptr_t rsp);
void thread_entry(void);
void thread_main(struct thread *t);

void thread_main(struct thread *t) {
    t->fn(t->arg);
    t->done = true;
    for (;;) {
        thread_yield();
    }
}

void thread_restart(struct thread *t) {
    uint8_t *area = (uint8_t *)t->area;
    atomic_init(&t->wake, 0);
    t->done = false;
    // What thread_swap() takes off the stack: the callee-saved registers, %rbx
    // the thread, then where to return to, thread_entry() with the stack
    // aligned as a call's target has it.
    uint64_t *top = (uint64_t *)(area + CPU_AREA_SIZE);
    *--top = (uint64_t)(uintptr_t)thread_entry;
    *--top = 0;
    *--top = (uint64_t)(uintptr_t)t;
    for (size_t i = 0; i < 4; i++) {
        *--top = 0;
    }
    t->rsp = (uintptr_t)top;
}

void thread_init(struct thread *t, void (*fn)(void *arg), void *arg) {
    t->area = page_alloc_aligned(CPU_AREA_SIZE / PAGE_SIZE, CPU_AREA_SIZE);
    t->fn = fn;
    t->arg = arg;
    thread_restart(t);
}

bool thread_run(struct thread *t) {
    if (t->done) {
        return false;
    }
    if (time_ns() < atomic_load_explicit(&t->wake, memory_order_relaxed)) {
        return true;
    }
    struct cpu *cpu = this_cpu();
    *t->area = cpu;
    cpu->thread = t;
    thread_swap(&t->runner_rsp, t->rsp);
    cpu->thread = NULL;
    return !t->done;
}

struct thread *thread_current(void) {
    return this_cpu()->thread;
}

void thread_yield(void) {
    struct thread *t = this_cpu()->thread;
    if (t == NULL) {
        panic("A thread's yield outside a thread\n");
    }
    thread_swap(&t->rsp, t->runner_rsp);
}

void thread_sleep(uint64_t until) {
    struct thread *t = this_cpu()->thread;
    if (t == NULL) {
        panic("A thread's sleep outside a thread\n");
    }
    atomic_store_explicit(&t->wake, until, memory_order_relaxed);
    thread_swap(&t->rsp, t->runner_rsp);
}

void thread_wake(struct thread *t) {
    atomic_store_explicit(&t->wake, 0, memory_order_release);
}

bool thread_yield_if_over(void) {
    if (this_cpu()->thread == NULL || !cpu_slice_over()) {
        return false;
    }
    thread_yield();
    return true;
}
