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

#ifndef SYS__SMP_H
#define SYS__SMP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/cpu.h>

// Finds the processors the MADT lists, and gives each its area. Needs
// acpi_init().
void smp_init(void);
// Starts every other processor into the hypervisor, where it waits for the
// guest to start it. Needs virt_init() and the guest's memory.
void smp_start_aps(void);
// Puts the start trampoline in its page for the boot processor alone, which
// the firmware's waking vector brings to entry, on the stack of size bytes at
// stack: the page, the waking vector's real-mode address.
uintptr_t smp_trampoline_wake(void (*entry)(void *), uint8_t *stack, size_t size);
// Starts the other processors again after the machine slept.
void smp_restart_aps(void);

// The processors found, the boot processor first, started or not.
size_t smp_cpu_count(void);
struct cpu *smp_cpu(size_t index);
struct cpu *smp_cpu_by_apic_id(uint32_t apic_id);

// Whether a processor's APIC ID reaches Fh, so that local APIC messages need
// 8-bit APIC IDs to name every processor alone.
bool smp_long_apic_ids(void);

#endif
