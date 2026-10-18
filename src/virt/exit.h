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

#ifndef VIRT__EXIT_H
#define VIRT__EXIT_H

#include <stdint.h>
#include <stdbool.h>
#include <virt/vcpu.h>

// Vendor-independent handling of the exits both backends share. The handlers
// that complete the guest's instruction advance its RIP themselves.
void exit_cpuid(struct vcpu *v);
void exit_hypercall(struct vcpu *v);
void exit_rdmsr(struct vcpu *v);
void exit_wrmsr(struct vcpu *v);
void exit_xsetbv(struct vcpu *v);

// An I/O port access the bitmaps trap. value is in and out.
void exit_io(struct vcpu *v, uint16_t port, unsigned size, bool in, uint32_t *value);
// The same for INS and OUTS, which it emulates.
void exit_string_io(struct vcpu *v, uint16_t port);

// A #UD the guest raised, which may be its VMCALL on an AMD processor.
void exit_invalid_opcode(struct vcpu *v);

#endif
