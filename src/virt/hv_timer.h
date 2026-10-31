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

#ifndef VIRT__HV_TIMER_H
#define VIRT__HV_TIMER_H

#include <stdint.h>

// A timer of the hypervisor's own, which the boot processor runs between the
// guest's work, whatever the guest does with its local APIC timers.
struct hv_timer {
    // By the processor's TSC, or 0.
    uint64_t deadline;
    void (*fired)(void *ctx);
    void *ctx;
};

// Before the guest starts.
void hv_timer_add(struct hv_timer *timer);
// From any processor. 0 stops the timer.
void hv_timer_set(struct hv_timer *timer, uint64_t tsc);
// The earliest deadline, or 0.
uint64_t hv_timer_next(void);
// On the boot processor, calls those that are due.
void hv_timer_run(void);

#endif
