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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <virt/hv_timer.h>
#include <lib/log.h>
#include <sys/smp.h>
#include <sys/x86.h>
#include <virt/vcpu.h>

#define MAX_TIMERS 8

static struct hv_timer *timers[MAX_TIMERS];
static size_t timer_count;

void hv_timer_add(struct hv_timer *timer) {
    if (timer_count == MAX_TIMERS) {
        panic("Too many hypervisor timers\n");
    }
    timers[timer_count++] = timer;
}

void hv_timer_set(struct hv_timer *timer, uint64_t tsc) {
    __atomic_store_n(&timer->deadline, tsc, __ATOMIC_RELEASE);
    vcpu_kick(smp_cpu(0));
}

uint64_t hv_timer_next(void) {
    uint64_t next = 0;
    for (size_t i = 0; i < timer_count; i++) {
        uint64_t deadline = __atomic_load_n(
            &timers[i]->deadline, __ATOMIC_ACQUIRE
        );
        if (deadline != 0 && (next == 0 || deadline < next)) {
            next = deadline;
        }
    }
    return next;
}

void hv_timer_run(void) {
    uint64_t now = rdtsc();
    for (size_t i = 0; i < timer_count; i++) {
        struct hv_timer *timer = timers[i];
        uint64_t deadline = __atomic_load_n(
            &timer->deadline, __ATOMIC_ACQUIRE
        );
        if (deadline == 0 || deadline > now) {
            continue;
        }
        // One set meanwhile, from another processor, stands.
        bool due = __atomic_compare_exchange_n(
            &timer->deadline, &deadline, 0, false, __ATOMIC_ACQ_REL,
            __ATOMIC_RELAXED
        );
        if (due) {
            timer->fired(timer->ctx);
        }
    }
}
