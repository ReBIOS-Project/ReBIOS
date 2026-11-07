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
// What the firmware's AML last wrote to the standard index.
static uint8_t host_index;

void cmos_host_io(uint16_t port, bool in, uint8_t *value) {
    spinlock_acquire(&lock);
    if (in) {
        *value = inb(port);
    } else {
        outb(port, *value);
        if ((port & ~PORT_ALIAS) == PORT_INDEX) {
            host_index = *value;
        }
    }
    spinlock_release(&lock);
}

uint8_t cmos_read(uint8_t reg) {
    spinlock_acquire(&lock);
    outb(PORT_INDEX, (uint8_t)((host_index & INDEX_NMI_MASK) | reg));
    uint8_t value = inb(PORT_TARGET);
    outb(PORT_INDEX, host_index);
    spinlock_release(&lock);
    return value;
}

void cmos_write(uint8_t reg, uint8_t value) {
    spinlock_acquire(&lock);
    outb(PORT_INDEX, (uint8_t)((host_index & INDEX_NMI_MASK) | reg));
    outb(PORT_TARGET, value);
    outb(PORT_INDEX, host_index);
    spinlock_release(&lock);
}
