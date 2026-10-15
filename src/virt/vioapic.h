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

#ifndef VIRT__VIOAPIC_H
#define VIRT__VIOAPIC_H

#include <stdint.h>
#include <stdbool.h>
#include <virt/vcpu.h>

// Gives the guest I/O APICs of its own where the host's are, which the
// hypervisor programs the host's from. Needs ioapic_init().
void vioapic_init(void);

// An access to an I/O APIC's page. Returns false if gpa is not in one.
bool vioapic_access(struct vcpu *v, uint64_t gpa);

// The guest's redirection entry for a GSI. Returns false if no I/O APIC has
// it.
bool vioapic_rte(uint32_t gsi, uint64_t *rte);

#endif
