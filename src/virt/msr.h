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

#ifndef VIRT__MSR_H
#define VIRT__MSR_H

#include <stdint.h>
#include <stdbool.h>

struct vcpu;

// The guest gets copies of the MSRs, or of their bits, that would change the
// processor under the hypervisor too. Needs virt_init() and cpu_tables_load().
void msr_init(void);
void msr_vcpu_init(struct vcpu *v);

// Whether msr is one the guest has a copy of; fault is set if the access raises
// #GP.
bool msr_rdmsr(struct vcpu *v, uint32_t msr, uint64_t *value, bool *fault);
bool msr_wrmsr(struct vcpu *v, uint32_t msr, uint64_t value, bool *fault);

#endif
