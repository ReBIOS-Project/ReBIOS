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

#ifndef SYS__TASK_H
#define SYS__TASK_H

#include <stdint.h>
#include <stdbool.h>
#include <sys/cpu.h>

// Work on a stack of its own, which a CPU runs until the work yields, and any
// CPU then carries on with.
struct task {
    // The task's stack pointer while it waits, and its runner's while it runs.
    uint64_t rsp;
    uint64_t runner_rsp;
    // Its stack's area, which begins with the CPU running it.
    struct cpu **area;
    void (*fn)(void *arg);
    void *arg;
    // The time a sleep of the task lasts until.
    uint64_t wake;
    bool done;
};

void task_init(struct task *t, void (*fn)(void *arg), void *arg);
// Runs the task on this CPU until it yields, unless it sleeps: false once its
// function returned.
bool task_run(struct task *t);
// The task this CPU runs, if any.
struct task *task_current(void);
// From the task, gives the CPU back to whatever runs it, until it runs the
// task again, or, for a sleep, until it does so once time_ns() reaches until.
void task_yield(void);
void task_sleep(uint64_t until);
// From a task, yields where this CPU's time slice is over, returning whether
// it did; elsewhere, returns false.
bool task_yield_if_over(void);

#endif
