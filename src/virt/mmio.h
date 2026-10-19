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

#ifndef VIRT__MMIO_H
#define VIRT__MMIO_H

#include <stdint.h>
#include <stdbool.h>
#include <virt/vcpu.h>

// A device the hypervisor emulates at guest-physical addresses, which it
// keeps unmapped, or mapped without the access it emulates.
struct mmio_ops {
    // Accesses of 1, 2, 4, or 8 bytes, naturally aligned, offset into the
    // region. Reads have no side effects.
    uint64_t (*read)(struct vcpu *v, void *ctx, uint64_t offset, unsigned size);
    void (*write)(struct vcpu *v, void *ctx, uint64_t offset, unsigned size, uint64_t value);
    // Whether writes go nowhere, so that INS may be emulated into it: the
    // processor may have read the port already.
    bool writes_ignored;
};

// Emulates a page-aligned region, for every vCPU.
void mmio_register(uint64_t base, uint64_t size, const struct mmio_ops *ops, void *ctx);

// Guest memory at a linear address as the guest's instructions reach it: RAM,
// emulated regions, or nothing. Accesses are of at most 8 bytes; they return
// false where the access faulted, having raised the fault.
bool mmio_read_linear(struct vcpu *v, uint64_t linear, void *buf, unsigned size);
bool mmio_write_linear(struct vcpu *v, uint64_t linear, const void *buf, unsigned size);

// An EPT violation or a nested page fault at gpa, for an access other than a
// fetch: emulates the instruction that made it, where nothing decodes as
// memory that is not there. Returns false where gpa is mapped after all, as
// another vCPU may just have mapped it: the guest then runs the access again.
bool mmio_fault(struct vcpu *v, uint64_t gpa);
// One for a fetch, of code there: runs it a step at a time.
bool mmio_fetch(struct vcpu *v, uint64_t gpa);

// The end of a single-step mmio_fault() set up for an instruction it does not
// emulate itself: the step's monitor trap, single-step trap, or fault.
void mmio_step_done(struct vcpu *v);

#endif
