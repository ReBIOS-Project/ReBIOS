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
#include <virt/a20.h>
#include <lib/spinlock.h>
#include <virt/gpt.h>
#include <virt/io.h>
#include <virt/vcpu.h>

#define SYSTEM_CONTROL_A 0x92

// In the output port and the system control port alike.
#define A20_BIT 0x02
// The system control port's INIT_NOW (ICH10 datasheet 319973-003, 13.7.3).
#define INIT_NOW 0x01

static struct spinlock lock;
// The guest's system control port, which never reaches the hardware, from its
// reset value of 00h (ICH10 datasheet 319973-003, 13.7.3).
static uint8_t system_control;
// The keyboard controller's A20 line. The chipset ORs it with the system
// control port's A20 bit into the gate (PIIX4 datasheet 290562-001, Port 92h
// register): each reads back as the guest set it.
static bool kbc_open = true;
static bool gate_open = true;

static void update_gate(void) {
    bool want_open = (system_control & A20_BIT) != 0 || kbc_open;
    if (want_open == gate_open) {
        return;
    }
    gate_open = want_open;
    gpt_mask_a20(!gate_open);
}

static void system_control_a(
    struct vcpu *v, void *ctx, uint16_t offset, size_t size, bool in, uint32_t *value
) {
    (void)v;
    (void)ctx;
    (void)offset;
    (void)size;
    spinlock_acquire(&lock);
    if (in) {
        *value = (*value & ~(uint32_t)0xff) | system_control;
    } else {
        uint8_t old = system_control;
        uint8_t written = (uint8_t)*value & (INIT_NOW | A20_BIT);
        system_control = (uint8_t)((system_control & ~(INIT_NOW | A20_BIT)) | written);
        // INIT_NOW's rise asserts INIT#, which sets the A20 bit (ICH10
        // datasheet 319973-003, 13.7.3).
        if ((old & INIT_NOW) == 0 && (written & INIT_NOW) != 0) {
            system_control |= A20_BIT;
            vcpu_init_all();
        }
        update_gate();
    }
    spinlock_release(&lock);
}

void a20_kbc_set(bool open) {
    spinlock_acquire(&lock);
    kbc_open = open;
    update_gate();
    spinlock_release(&lock);
}

void a20_init(void) {
    struct io_handler handler = { SYSTEM_CONTROL_A, 1, system_control_a, NULL, true };
    io_register(&handler);
}
