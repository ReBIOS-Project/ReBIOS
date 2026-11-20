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
// You should have received a copy of the GNU General Public License along with
// ReBIOS. If not, see <https://www.gnu.org/licenses/>.

#ifndef VIRT__EXIT_H
#define VIRT__EXIT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <virt/vcpu.h>

// Vendor-independent handling of the exits both backends share. The handlers
// that complete the guest's instruction advance its RIP themselves.
void exit_cpuid(struct vcpu *v);
void exit_hypercall(struct vcpu *v);
void exit_rdmsr(struct vcpu *v);
void exit_wrmsr(struct vcpu *v);
// An MSR the guest reads or writes as RDMSR or WRMSR would have it, but for
// the instruction; false where it faults.
bool exit_msr_read(struct vcpu *v, uint32_t msr, uint64_t *value);
bool exit_msr_write(struct vcpu *v, uint32_t msr, uint64_t value);
// An MSR access that names its MSR and whose data is general register reg's,
// as RDMSR's and WRMSR's are EDX:EAX: the immediate forms of RDMSR and
// WRMSRNS.
void exit_msr_register(struct vcpu *v, uint32_t msr, unsigned int reg, bool write);
void exit_xsetbv(struct vcpu *v);

// An I/O port access the bitmaps trap. value is in and out.
void exit_io(struct vcpu *v, uint16_t port, size_t size, bool in, uint32_t *value);
// The same for INS and OUTS, which it emulates.
void exit_string_io(struct vcpu *v, uint16_t port);

#endif
