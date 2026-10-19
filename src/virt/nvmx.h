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

#ifndef VIRT__NVMX_H
#define VIRT__NVMX_H

#include <stdint.h>
#include <stdbool.h>

struct vcpu;

// Whether the guest gets VMX of its own.
extern bool nvmx_enabled;

void nvmx_init(void);
void nvmx_vcpu_init(struct vcpu *v);

// The guest's VMX instructions, by the exits they cause.
void nvmx_instruction(struct vcpu *v, uint32_t reason);
// The guest's IA32_FEATURE_CONTROL and VMX capability MSRs.
bool nvmx_rdmsr(struct vcpu *v, uint32_t msr, uint64_t *value, bool *fault);
bool nvmx_wrmsr(struct vcpu *v, uint32_t msr, uint64_t value, bool *fault);
// Whether the guest may write a value to CR0 or CR4, as VMX operation and the
// VMX it gets have it.
bool nvmx_cr_allowed(struct vcpu *v, unsigned cr, uint64_t value);

#endif
