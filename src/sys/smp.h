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

#ifndef SYS__SMP_H
#define SYS__SMP_H

#include <stdint.h>
#include <stdbool.h>
#include <sys/cpu.h>

// Finds the processors the MADT lists, and gives each its area. Needs
// acpi_init().
void smp_init(void);
// Starts every other processor into the hypervisor, where it waits for the
// guest to start it. Needs virt_init() and the guest's memory.
void smp_start_aps(void);

// The processors found, the boot processor first, started or not.
uint32_t smp_cpu_count(void);
struct cpu *smp_cpu(uint32_t index);
struct cpu *smp_cpu_by_apic_id(uint32_t apic_id);

// Whether the guest gets the processor with the APIC ID, as the configuration
// says.
bool smp_guest_cpu(uint32_t apic_id);

#endif
