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
#include <stddef.h>
#include <stdint.h>
#include <virt/a20.h>
#include <lib/spinlock.h>
#include <sys/cpu.h>
#include <sys/smp.h>
#include <sys/x86.h>
#include <virt/gpt.h>
#include <virt/io.h>
#include <virt/vcpu.h>

#define SYSTEM_CONTROL_A 0x92

// In the output port and the system control port alike.
#define A20_BIT 0x02
// The system control port's INIT_NOW (ICH10 datasheet 319973-003, 13.7.3).
#define INIT_NOW 0x01

static struct spinlock lock;
// The guest's system control port, which never reaches the hardware, its
// reserved bits as the hardware's were when the hypervisor took over.
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

// The rest of an access wider than the port goes to the hardware as it is.
static void pass_rest(uint16_t port, size_t size, bool in, uint32_t *value) {
    for (size_t i = 1; i < size; i++) {
        if (in) {
            *value = (*value & ~((uint32_t)0xff << (8 * i)))
                   | (uint32_t)inb((uint16_t)(port + i)) << (8 * i);
        } else {
            outb((uint16_t)(port + i), (uint8_t)(*value >> (8 * i)));
        }
    }
}

// INIT# reaches every processor, as an INIT IPI does, which leaves one that
// waits for a start-up IPI waiting.
static void init_processors(void) {
    for (size_t i = 0; i < smp_cpu_count(); i++) {
        struct cpu *cpu = smp_cpu(i);
        struct vcpu *to = cpu->vcpu;
        if (to == NULL
         || atomic_load_explicit(&to->sipi, memory_order_acquire) != VCPU_SIPI_RUNNING) {
            continue;
        }
        atomic_store_explicit(&cpu->init_pending, true, memory_order_release);
        vcpu_kick(cpu);
    }
}

static void system_control_a(
    struct vcpu *v, void *ctx, uint16_t offset, size_t size, bool in, uint32_t *value
) {
    (void)v;
    (void)ctx;
    (void)offset;
    spinlock_acquire(&lock);
    if (in) {
        *value = system_control;
    } else {
        uint8_t old = system_control;
        uint8_t written = (uint8_t)*value & (INIT_NOW | A20_BIT);
        system_control = (uint8_t)((system_control & ~(INIT_NOW | A20_BIT)) | written);
        // INIT_NOW's rise asserts INIT#, which sets the A20 bit (ICH10
        // datasheet 319973-003, 13.7.3).
        if ((old & INIT_NOW) == 0 && (written & INIT_NOW) != 0) {
            system_control |= A20_BIT;
            init_processors();
        }
        update_gate();
    }
    pass_rest(SYSTEM_CONTROL_A, size, in, value);
    spinlock_release(&lock);
}

void a20_kbc_set(bool open) {
    spinlock_acquire(&lock);
    kbc_open = open;
    update_gate();
    spinlock_release(&lock);
}

void a20_init(void) {
    // The keyboard controller's A20 line comes up raised, and the system
    // control port as the hardware has it, whose own A20 bit then holds the
    // host's gate open, whatever the keyboard controller's line.
    system_control = inb(SYSTEM_CONTROL_A);
    outb(SYSTEM_CONTROL_A, (uint8_t)(system_control | A20_BIT));

    struct io_handler handler = { SYSTEM_CONTROL_A, 1, system_control_a, NULL };
    io_register(&handler);
}
