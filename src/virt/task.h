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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <virt/vcpu.h>

// What started a task switch, in the order VMX's exit qualification numbers
// them.
#define TASK_CALL 0
#define TASK_IRET 1
#define TASK_JMP 2
#define TASK_GATE 3

// For TASK_GATE, the event delivered through the IDT's task gate.
struct task_event {
    uint8_t vector;
    // A hardware exception, which a fault during the switch can turn into a
    // double fault.
    bool exception;
    // From outside the program: anything but INT n, INT3, and INTO.
    bool external;
    // INT n, INT3, INTO, and INT1, whose task resumes after them.
    bool software;
    bool nmi;
    bool error_valid;
    uint32_t error;
};

// Carries out the task switch to the TSS selector names, which VMX, unlike the
// processor outside it, leaves to the hypervisor once it has checked the
// selector and the new TSS's descriptor. length is the instruction's that
// started it, if one did; event is NULL but for TASK_GATE.
void task_switch(
    struct vcpu *v, uint16_t selector, int source,
    const struct task_event *event, size_t length
);

#endif
