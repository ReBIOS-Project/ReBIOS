// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of ReBIOS.
// Copyright (C) 2026 Mintsuki and contributors
//
// ReBIOS is free software: you can redistribute it and/or modify it under the
// terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version.
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

#include <stdbool.h>
#include <sys/x86.h>

struct spinlock {
    bool locked;
};

static inline void spinlock_acquire(struct spinlock *lock) {
    while (__atomic_exchange_n(&lock->locked, true, __ATOMIC_ACQUIRE)) {
        while (__atomic_load_n(&lock->locked, __ATOMIC_RELAXED)) {
            cpu_relax();
        }
    }
}

static inline void spinlock_release(struct spinlock *lock) {
    __atomic_store_n(&lock->locked, false, __ATOMIC_RELEASE);
}

#endif
