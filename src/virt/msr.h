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

#include <stdbool.h>
#include <stdint.h>

struct vcpu;

// The guest gets copies of the MSRs, or of their bits, that would change the
// processor under the hypervisor too. Needs virt_init() and cpu_tables_load().
void msr_init(void);
void msr_vcpu_init(struct vcpu *v);
// Sets each processor up as it starts for the MSRs the guest gets: on AMD's
// Zen, the TSC held at P0's current frequency.
void msr_cpu_init(void);

// Whether msr is one the guest has a copy of; fault is set if the access
// raises #GP.
bool msr_rdmsr(struct vcpu *v, uint32_t msr, uint64_t *value, bool *fault);
bool msr_wrmsr(struct vcpu *v, uint32_t msr, uint64_t value, bool *fault);

// Whether the guest's accesses to an MSR no module handles reach the
// processor: for MSRs the hypervisor knows, of the processor's model, as safe
// to give the guest; the others raise #GP as on a processor without them. The
// init lets those reach it without exits, once the modules have chosen theirs
// (after nvmx_init()).
bool msr_allowed(uint32_t msr);
void msr_allowed_init(void);

#endif
