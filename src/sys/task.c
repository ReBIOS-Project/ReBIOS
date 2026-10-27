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

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <lib/log.h>
#include <lib/time.h>
#include <sys/cpu.h>
#include <sys/mm.h>
#include <sys/task.h>

void task_swap(uint64_t *save_rsp, uint64_t rsp);
void task_entry(void);
void task_main(struct task *t);

void task_main(struct task *t) {
    t->fn(t->arg);
    t->done = true;
    for (;;) {
        task_yield();
    }
}

void task_init(struct task *t, void (*fn)(void *arg), void *arg) {
    uint8_t *area = page_alloc_aligned(CPU_AREA_SIZE / PAGE_SIZE, CPU_AREA_SIZE);
    t->area = (struct cpu **)area;
    t->fn = fn;
    t->arg = arg;
    t->wake = 0;
    t->done = false;
    // What task_swap() takes off the stack: the callee-saved registers, %rbx
    // the task, then where to return to, task_entry() with the stack aligned
    // as a call's target has it.
    uint64_t *top = (uint64_t *)(area + CPU_AREA_SIZE);
    *--top = (uint64_t)(uintptr_t)task_entry;
    *--top = 0;
    *--top = (uint64_t)(uintptr_t)t;
    for (int i = 0; i < 4; i++) {
        *--top = 0;
    }
    t->rsp = (uint64_t)(uintptr_t)top;
}

bool task_run(struct task *t) {
    if (t->done) {
        return false;
    }
    if (time_ns() < t->wake) {
        return true;
    }
    struct cpu *cpu = this_cpu();
    *t->area = cpu;
    cpu->task = t;
    task_swap(&t->runner_rsp, t->rsp);
    cpu->task = NULL;
    return !t->done;
}

struct task *task_current(void) {
    return this_cpu()->task;
}

void task_yield(void) {
    struct task *t = this_cpu()->task;
    if (t == NULL) {
        panic("A task's yield outside a task\n");
    }
    task_swap(&t->rsp, t->runner_rsp);
}

void task_sleep(uint64_t until) {
    struct task *t = this_cpu()->task;
    if (t == NULL) {
        panic("A task's sleep outside a task\n");
    }
    t->wake = until;
    task_swap(&t->rsp, t->runner_rsp);
}

bool task_yield_if_over(void) {
    if (this_cpu()->task == NULL || !cpu_slice_over()) {
        return false;
    }
    task_yield();
    return true;
}
