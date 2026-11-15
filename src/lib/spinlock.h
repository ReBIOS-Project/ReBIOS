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

#ifndef LIB__SPINLOCK_H
#define LIB__SPINLOCK_H

#include <stdatomic.h>
#include <stdbool.h>
#include <sys/x86.h>

// A holder runs with interrupts off, so that nothing interrupts or preempts it
// on its own processor, as nothing could take the lock there meanwhile.
struct spinlock {
    _Atomic(bool) locked;
    // Whether interrupts were on where the lock was taken, which its release
    // turns back on.
    bool interrupts;
};

static inline void spinlock_acquire(struct spinlock *lock) {
    bool interrupts = (read_rflags() & RFLAGS_IF) != 0;
    cli();
    while (atomic_exchange_explicit(&lock->locked, true, memory_order_acquire)) {
        while (atomic_load_explicit(&lock->locked, memory_order_relaxed)) {
            cpu_relax();
        }
    }
    lock->interrupts = interrupts;
}

static inline bool spinlock_try(struct spinlock *lock) {
    bool interrupts = (read_rflags() & RFLAGS_IF) != 0;
    cli();
    if (atomic_exchange_explicit(&lock->locked, true, memory_order_acquire)) {
        if (interrupts) {
            sti();
        }
        return false;
    }
    lock->interrupts = interrupts;
    return true;
}

static inline void spinlock_release(struct spinlock *lock) {
    bool interrupts = lock->interrupts;
    atomic_store_explicit(&lock->locked, false, memory_order_release);
    if (interrupts) {
        sti();
    }
}

#endif
