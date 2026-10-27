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

#ifndef VIRT__EMULATE_H
#define VIRT__EMULATE_H

#include <stdint.h>
#include <stdbool.h>
#include <virt/insn.h>
#include <virt/vcpu.h>

// How emulated instructions reach memory and ports. Accesses are of at most 8
// bytes at a linear address, which the callbacks translate; they return false
// where the access faulted, having raised the fault.
struct emulate_mem {
    bool (*read)(struct vcpu *v, uint64_t linear, void *buf, unsigned size);
    bool (*write)(struct vcpu *v, uint64_t linear, const void *buf, unsigned size);
    // INS's data, for its destination: false where INS is not to be emulated.
    bool (*ins)(struct vcpu *v, uint64_t linear, uint16_t port, unsigned size, uint32_t *value);
    void (*outs)(struct vcpu *v, uint16_t port, unsigned size, uint32_t value);
};

enum emulate_result {
    EMULATE_DONE,
    // A fault was raised, or a device is not done with an access, and the
    // instruction is to run again after the guest's events.
    EMULATE_FAULT,
    // Not one of the instructions emulated here.
    EMULATE_UNSUPPORTED,
};

// Emulates the decoded instruction at the guest's RIP: the common integer
// forms with a memory operand, and the string instructions.
enum emulate_result emulate(struct vcpu *v, const struct insn *insn, const struct emulate_mem *mem);

#endif
