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

#ifndef LIB__SPINLOCK_H
#define LIB__SPINLOCK_H

#include <stdatomic.h>
#include <stdbool.h>
#include <sys/x86.h>

struct spinlock {
    _Atomic(bool) locked;
};

static inline void spinlock_acquire(struct spinlock *lock) {
    while (atomic_exchange_explicit(&lock->locked, true, memory_order_acquire)) {
        while (atomic_load_explicit(&lock->locked, memory_order_relaxed)) {
            cpu_relax();
        }
    }
}

static inline bool spinlock_try(struct spinlock *lock) {
    return !atomic_exchange_explicit(&lock->locked, true, memory_order_acquire);
}

static inline void spinlock_release(struct spinlock *lock) {
    atomic_store_explicit(&lock->locked, false, memory_order_release);
}

#endif
