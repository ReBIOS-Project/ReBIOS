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

#ifndef VIRT__TASK_H
#define VIRT__TASK_H

#include <stdint.h>
#include <stdbool.h>
#include <virt/vcpu.h>

// What started a task switch, in the order VMX's exit qualification numbers
// them.
enum task_source {
    TASK_CALL,
    TASK_IRET,
    TASK_JMP,
    TASK_GATE,
};

// For TASK_GATE, the event delivered through the IDT's task gate.
struct task_event {
    // INT n, INT3, INTO, and INT1, whose task resumes after them.
    bool software;
    bool nmi;
    bool error_valid;
    uint32_t error;
};

// Carries out the task switch to the TSS selector names, which VMX, unlike
// the processor outside it, leaves to the hypervisor once it has checked the
// selector and the new TSS's descriptor. length is the instruction's that
// started it, if one did; event is NULL but for TASK_GATE.
void task_switch(struct vcpu *v, uint16_t selector, enum task_source source,
                 const struct task_event *event, unsigned length);

#endif
