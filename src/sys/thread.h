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

#ifndef SYS__THREAD_H
#define SYS__THREAD_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/cpu.h>

// Work on a stack of its own, which a CPU runs until the work yields, and any
// CPU then carries on with.
struct thread {
    // The thread's stack pointer while it waits, and its runner's while it
    // runs.
    uintptr_t rsp;
    uintptr_t runner_rsp;
    // Its stack's area, which begins with the CPU running it.
    struct cpu **area;
    void (*fn)(void *arg);
    void *arg;
    // The time a sleep of the thread lasts until.
    _Atomic(uint64_t) wake;
    bool done;
    // Whether its runner has interrupts on, which the thread then has too.
    bool interrupts;
    // While an interrupt has stopped it, the RFLAGS in the interrupt's frame,
    // which the thread goes on with from the handler, else NULL.
    uint64_t *preempted;
};

void thread_init(struct thread *t, void (*fn)(void *arg), void *arg);
// Has the thread start over, whatever it was doing.
void thread_restart(struct thread *t);
// Runs the thread on this CPU until it yields, an interrupt preempts it, or
// unless it sleeps: false once its function returned.
bool thread_run(struct thread *t);
// The thread this CPU runs, if any.
struct thread *thread_current(void);
// From the thread, gives the CPU back to whatever runs it, until it runs the
// thread again, or, for a sleep, until it does so once time_ns() reaches
// until.
void thread_yield(void);
void thread_sleep(uint64_t until);
// Ends its sleep, from anywhere.
void thread_wake(struct thread *t);
// From a host interrupt's handler, with the RFLAGS in its frame: the thread
// that the interrupt came in, if any, gives the CPU back to what runs it, and
// in the threads' turn the CPU's slice ends, as what the interrupt brought is
// the guest's to see to first.
void thread_preempt(uint64_t *rflags);

#endif
