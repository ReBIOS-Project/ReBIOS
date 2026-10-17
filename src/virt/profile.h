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

#ifndef VIRT__PROFILE_H
#define VIRT__PROFILE_H

#include <stdint.h>
#include <stdbool.h>
#include <virt/vcpu.h>

// With profile = true: counts the guest's exits by reason and the time the
// hypervisor takes over each, and the trapped ports and emulated pages they
// are for, and logs them every few seconds. Needs virt_init().
void profile_init(void);

extern bool profile_on;

// An exit, by its VMX basic exit reason or SVM exit code, as the handler
// begins; and the next entry, which ends the exit's time.
void profile_exit(struct vcpu *v, uint32_t reason);
void profile_entry(struct vcpu *v);
void profile_io(uint16_t port);
void profile_hypercall(uint32_t function);
void profile_mmio(uint64_t gpa);

#endif
