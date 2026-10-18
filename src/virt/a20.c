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

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <lib/spinlock.h>
#include <sys/x86.h>
#include <virt/a20.h>
#include <virt/gpt.h>
#include <virt/io.h>

#define KBC_DATA 0x60
#define KBC_COMMAND 0x64
#define SYSTEM_CONTROL_A 0x92

#define KBC_READ_OUTPUT 0xd0
#define KBC_WRITE_OUTPUT 0xd1
#define KBC_DISABLE_A20 0xdd
#define KBC_ENABLE_A20 0xdf
// 0xf0-0xff pulse the output port bits whose command bits are clear.
#define KBC_PULSE 0xf0

// In the output port and the system control port alike.
#define A20_BIT 0x02

enum pending {
    PENDING_NONE,
    PENDING_READ_OUTPUT,
    PENDING_WRITE_OUTPUT,
};

static struct spinlock lock;
// The chipset ORs the two into the gate (PIIX4 datasheet 290562-001, Port 92h
// register): each reads back as the guest set it.
static bool port92_open;
static bool kbc_open = true;
static bool gate_open = true;
static enum pending pending;

static void update_gate(void) {
    bool want_open = port92_open || kbc_open;
    if (want_open == gate_open) {
        return;
    }
    gate_open = want_open;
    gpt_mask_a20(!gate_open);
}

// The rest of an access wider than the port goes to the hardware as it is.
static void pass_rest(uint16_t port, unsigned size, bool in, uint32_t *value) {
    for (unsigned i = 1; i < size; i++) {
        if (in) {
            *value = (*value & ~(0xffu << (8 * i))) | (uint32_t)inb((uint16_t)(port + i)) << (8 * i);
        } else {
            outb((uint16_t)(port + i), (uint8_t)(*value >> (8 * i)));
        }
    }
}

static uint8_t with_a20(uint8_t value, bool open) {
    return (uint8_t)((value & ~A20_BIT) | (open ? A20_BIT : 0));
}

static void system_control_a(struct vcpu *v, void *ctx, uint16_t offset, unsigned size, bool in,
                             uint32_t *value) {
    (void)v;
    (void)ctx;
    (void)offset;
    spinlock_acquire(&lock);
    if (in) {
        *value = with_a20(inb(SYSTEM_CONTROL_A), port92_open);
    } else {
        port92_open = (*value & A20_BIT) != 0;
        update_gate();
        outb(SYSTEM_CONTROL_A, (uint8_t)(*value | A20_BIT));
    }
    pass_rest(SYSTEM_CONTROL_A, size, in, value);
    spinlock_release(&lock);
}

static void kbc_command(struct vcpu *v, void *ctx, uint16_t offset, unsigned size, bool in,
                        uint32_t *value) {
    (void)v;
    (void)ctx;
    (void)offset;
    spinlock_acquire(&lock);
    if (in) {
        *value = inb(KBC_COMMAND);
    } else {
        uint8_t command = (uint8_t)*value;
        pending = PENDING_NONE;
        if (command == KBC_DISABLE_A20 || command == KBC_ENABLE_A20) {
            kbc_open = command == KBC_ENABLE_A20;
            update_gate();
        } else {
            if (command == KBC_READ_OUTPUT) {
                pending = PENDING_READ_OUTPUT;
            } else if (command == KBC_WRITE_OUTPUT) {
                pending = PENDING_WRITE_OUTPUT;
            } else if (command >= KBC_PULSE) {
                command |= A20_BIT;
            }
            outb(KBC_COMMAND, command);
        }
    }
    pass_rest(KBC_COMMAND, size, in, value);
    spinlock_release(&lock);
}

static void kbc_data(struct vcpu *v, void *ctx, uint16_t offset, unsigned size, bool in,
                     uint32_t *value) {
    (void)v;
    (void)ctx;
    (void)offset;
    spinlock_acquire(&lock);
    if (in) {
        uint8_t data = inb(KBC_DATA);
        if (pending == PENDING_READ_OUTPUT) {
            data = with_a20(data, kbc_open);
            pending = PENDING_NONE;
        }
        *value = data;
    } else {
        uint8_t data = (uint8_t)*value;
        if (pending == PENDING_WRITE_OUTPUT) {
            kbc_open = (data & A20_BIT) != 0;
            update_gate();
            data |= A20_BIT;
            pending = PENDING_NONE;
        }
        outb(KBC_DATA, data);
    }
    pass_rest(KBC_DATA, size, in, value);
    spinlock_release(&lock);
}

void a20_init(void) {
    // The keyboard controller's A20 line comes up raised, port 92h's bit as
    // the hardware has it.
    port92_open = (inb(SYSTEM_CONTROL_A) & A20_BIT) != 0;

    struct io_handler system_control = {SYSTEM_CONTROL_A, 1, system_control_a, NULL};
    io_register(&system_control);
    struct io_handler command = {KBC_COMMAND, 1, kbc_command, NULL};
    io_register(&command);
    struct io_handler data = {KBC_DATA, 1, kbc_data, NULL};
    io_register(&data);
}
