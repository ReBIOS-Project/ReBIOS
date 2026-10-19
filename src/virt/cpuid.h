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

#ifndef VIRT__CPUID_H
#define VIRT__CPUID_H

#include <stdint.h>
#include <sys/x86.h>
#include <virt/vcpu.h>

// Needs virt_init() and msr_init(), which lifts the firmware's CPUID limit.
void cpuid_init(void);

// What the guest's CPUID returns: the processor's answers, cut down to the
// leaves and bits the hypervisor knows the meaning of and lets the guest have,
// the rest as a processor without them returns them.
struct cpuid_regs cpuid_guest(struct vcpu *v, uint32_t leaf, uint32_t subleaf);

// The XSAVE state components the guest may enable in XCR0 and in IA32_XSS.
uint64_t cpuid_xcr0_allowed(void);
uint64_t cpuid_xss_allowed(void);

// The EFER bits the guest may set, those of the features its CPUID shows. SVME
// stays the hypervisor's.
uint64_t cpuid_efer_allowed(struct vcpu *v);

#endif
