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
#include <stdint.h>
#include <hw/cmos.h>
#include <lib/spinlock.h>
#include <sys/x86.h>

// ICH10 datasheet 319973-003, 13.6.1: 74h-77h alias 70h-73h, and bit 7 of the
// standard index, the NMI mask, reads back from neither, so its value is the
// last one written.
#define PORT_INDEX 0x70
#define PORT_TARGET 0x71
#define PORT_ALIAS 0x04
#define INDEX_NMI_MASK 0x80

static struct spinlock lock;
// What the firmware's AML last wrote to the standard index, and what the index
// holds.
static uint8_t host_index;
static uint8_t index;
// Whether the index holds a register that the guest's own accesses to the data
// port reach, and how many times that ended.
static bool lent;
static _Atomic(uint64_t) reclaims;
static cmos_reclaimer reclaimer;

static void select_index(uint8_t value) {
    outb(PORT_INDEX, value);
    index = value;
}

static uint8_t register_index(uint8_t reg) {
    return (uint8_t)((host_index & INDEX_NMI_MASK) | reg);
}

static void reclaim(void) {
    if (lent) {
        lent = false;
        atomic_fetch_add_explicit(&reclaims, 1, memory_order_seq_cst);
        reclaimer();
    }
}

void cmos_host_io(uint16_t port, bool in, uint8_t *value) {
    spinlock_acquire(&lock);
    reclaim();
    if (!in && (port & ~PORT_ALIAS) == PORT_INDEX) {
        outb(port, *value);
        host_index = index = *value;
    } else {
        if (index != host_index) {
            select_index(host_index);
        }
        if (in) {
            *value = inb(port);
        } else {
            outb(port, *value);
        }
    }
    spinlock_release(&lock);
}

uint8_t cmos_read(uint8_t reg) {
    spinlock_acquire(&lock);
    reclaim();
    select_index(register_index(reg));
    uint8_t value = inb(PORT_TARGET);
    spinlock_release(&lock);
    return value;
}

void cmos_write(uint8_t reg, uint8_t value) {
    spinlock_acquire(&lock);
    reclaim();
    select_index(register_index(reg));
    outb(PORT_TARGET, value);
    spinlock_release(&lock);
}

void cmos_set_reclaimer(cmos_reclaimer function) {
    reclaimer = function;
}

uint64_t cmos_lend(uint8_t reg) {
    spinlock_acquire(&lock);
    select_index(register_index(reg));
    lent = true;
    uint64_t count = atomic_load_explicit(&reclaims, memory_order_relaxed);
    spinlock_release(&lock);
    return count;
}

uint64_t cmos_lent_io(uint8_t reg, bool in, uint8_t *value) {
    spinlock_acquire(&lock);
    if (!lent || index != register_index(reg)) {
        select_index(register_index(reg));
        lent = true;
    }
    if (in) {
        *value = inb(PORT_TARGET);
    } else {
        outb(PORT_TARGET, *value);
    }
    uint64_t count = atomic_load_explicit(&reclaims, memory_order_relaxed);
    spinlock_release(&lock);
    return count;
}

void cmos_reclaim(void) {
    spinlock_acquire(&lock);
    reclaim();
    spinlock_release(&lock);
}

uint64_t cmos_reclaims(void) {
    return atomic_load_explicit(&reclaims, memory_order_seq_cst);
}
