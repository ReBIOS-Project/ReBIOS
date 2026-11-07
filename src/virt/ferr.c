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
#include <virt/ferr.h>
#include <hw/isa.h>
#include <lib/spinlock.h>
#include <lib/util.h>
#include <sys/x86.h>
#include <virt/event.h>
#include <virt/io.h>
#include <virt/irq.h>
#include <virt/vcpu.h>

// SDM 325462-093 Vol. 1, 8.1.3 and 8.1.5: the x87 status word's exception
// flags with the stack fault one, which FNCLEX clears with the summary ES and
// the busy flag B that mirrors it, and the control word's masks of those
// exceptions. The environment FNSTENV stores in protected mode, 28 bytes, has
// the control word at 0 and the status word at 4 (8.1.10).
#define FSW_FLAGS 0x7f
#define FSW_ES 0x80
#define FSW_B 0x8000
#define FCW_MASKS 0x3f
#define ENV_SIZE 28
#define ENV_CONTROL 0
#define ENV_STATUS 4

#define PORT_COPROC_ERR 0xf0

static struct spinlock lock;
// The vCPUs whose error FERR# reports, IGNNE#, and the latch on IRQ13.
static size_t errors;
static bool ignne;
static bool irq_up;

// The guest's x87 state is the processor's while the hypervisor runs, which
// leaves it alone.
static uint16_t status_word(void) {
    uint16_t status;
    asm volatile ("fnstsw %0" : "=m"(status));
    return status;
}

// Under the lock.
static void latch(bool up) {
    if (up != irq_up) {
        irq_up = up;
        irq_isa_set(ISA_IRQ_FPU, up);
    }
}

// COPROC_ERR: any write lowers IRQ13 and asserts IGNNE#, while FERR# is up;
// reads find nothing, as of a register that is written only.
static void coproc_err(
    struct vcpu *v, void *ctx, uint16_t offset, size_t size, bool in, uint32_t *value
) {
    (void)v;
    (void)ctx;
    (void)offset;
    (void)size;
    if (in) {
        *value = 0xff;
        return;
    }
    spinlock_acquire(&lock);
    if (errors != 0) {
        ignne = true;
        latch(false);
    }
    spinlock_release(&lock);
}

void ferr_init(void) {
    struct io_handler port = {
        .base = PORT_COPROC_ERR, .count = 1, .access = coproc_err, .bytes = true
    };
    io_register(&port);
}

bool ferr_catches(struct vcpu *v) {
    return (vcpu_cr0(v) & CR0_NE) == 0;
}

void ferr_fault(struct vcpu *v) {
    spinlock_acquire(&lock);
    bool ignore = ignne;
    if (!ignore && !v->ferr) {
        v->ferr = true;
        errors++;
        latch(true);
    }
    spinlock_release(&lock);
    if (ignore) {
        // The instruction runs with the flags put aside, which come back
        // beside those it sets.
        v->ferr_flags = status_word() & FSW_FLAGS;
        asm volatile ("fnclex");
        v->ferr_step = true;
        v->stepping = true;
        vcpu_single_step(v, true);
        return;
    }
    // The processor waits before the instruction (8.7.2), which the guest's
    // handler of what comes lets run again.
    if (!event_halt_ends(v)) {
        vcpu_wait(v, event_halt_ends, false);
    }
}

bool ferr_stepping(struct vcpu *v) {
    return v->ferr_step;
}

void ferr_step_done(struct vcpu *v) {
    uint8_t env[ENV_SIZE];
    // FNSTENV masks every exception, which FLDENV's control word undoes.
    asm volatile ("fnstenv %0" : "=m"(env));
    uint16_t control = read_le16(&env[ENV_CONTROL]);
    uint16_t status = read_le16(&env[ENV_STATUS]);
    status |= v->ferr_flags;
    if ((status & ~control & FCW_MASKS) != 0) {
        status |= FSW_ES | FSW_B;
    }
    write_le16(&env[ENV_STATUS], status);
    asm volatile ("fldenv %0" :: "m"(env));
    v->ferr_step = false;
    v->stepping = false;
    vcpu_single_step(v, false);
}

void ferr_follow(struct vcpu *v) {
    if (!v->ferr || v->ferr_step || (status_word() & FSW_ES) != 0) {
        return;
    }
    spinlock_acquire(&lock);
    v->ferr = false;
    if (--errors == 0) {
        ignne = false;
        latch(false);
    }
    spinlock_release(&lock);
}
