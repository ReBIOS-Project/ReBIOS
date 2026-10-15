// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of ReBIOS.
// Copyright (C) 2026 Mintsuki and contributors
//
// ReBIOS is free software: you can redistribute it and/or modify it under the
// terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version.
//
// ReBIOS is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
// FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
// details.
//
// You should have received a copy of the GNU General Public License along
// with ReBIOS. If not, see <https://www.gnu.org/licenses/>.

#ifndef VIRT__VAPIC_H
#define VIRT__VAPIC_H

#include <stdint.h>
#include <stdbool.h>
#include <virt/vcpu.h>

// Where processors cannot wait for start-up IPIs in the guest, the guest's
// writes to its local APIC's ICR are caught: through the x2APIC ICR MSR, and
// through the xAPIC page, mapped read-only. INITs and start-up IPIs then go
// to the vCPUs, and other IPIs on to the processor's APIC. Needs
// smp_start_aps().
void vapic_init(void);

// A write to the xAPIC page. Returns false if gpa is not in it, or the write
// cannot be decoded.
bool vapic_mmio_write(struct vcpu *v, uint64_t gpa);

// A write to the x2APIC ICR. Returns false if the ICR is not caught.
bool vapic_msr_write(struct vcpu *v, uint32_t msr, uint64_t value);

#endif
