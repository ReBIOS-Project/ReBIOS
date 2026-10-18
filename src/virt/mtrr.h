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

#ifndef VIRT__MTRR_H
#define VIRT__MTRR_H

#include <stdint.h>
#include <stdbool.h>

#define MTRR_FIXED_COUNT 11
#define MTRR_VARIABLE_COUNT 8

struct vcpu;

// The guest gets MTRRs of its own, which describe its memory, not the host's,
// and which nothing applies to guest-physical addresses: SDM 325462-093 Vol. 3C,
// 31.3.7.2, "The MTRRs have no effect on the memory type used for an access to a
// guest-physical address", and APM 40332 rev. 4.10, 15.25.8, "there is no
// hardware support for guest MTRRs". Needs virt_init().
void mtrr_init(void);

// Write-back, as a BIOS leaves them, but for the VGA window and the PCI hole.
// Needs layout_init().
void mtrr_vcpu_init(struct vcpu *v);

// Whether msr is one of the MTRRs; fault is set if the access raises #GP.
bool mtrr_rdmsr(struct vcpu *v, uint32_t msr, uint64_t *value, bool *fault);
bool mtrr_wrmsr(struct vcpu *v, uint32_t msr, uint64_t value, bool *fault);

#endif
