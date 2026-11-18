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

#ifndef VIRT__SLEEP_H
#define VIRT__SLEEP_H

#include <stdbool.h>
#include <stdint.h>

struct cpu;
struct vcpu;

// The machine's sleeping states that the guest may ask for through its virtual
// PM1 control register (ACPI 6.6, 7.4.2): S3 alone so far.
#define SLEEP_S3 3

// Whether the guest gets the sleeping state: where the machine's namespace has
// its \_Sx_, and, for S3, where the guest drives the display itself, as the
// hypervisor cannot light an emulated VGA's screen again after it.
bool sleep_offered(uint8_t state);
// The guest asks for the sleeping state, from any processor.
void sleep_request(uint8_t state);
// Whether a sleeping state is asked for, which ends a vCPU's waits.
bool sleep_requested(void);
// Before the vCPU enters the guest, and while it waits: where a sleeping state
// is asked for, the boot processor puts the machine to sleep and returns once
// it is awake, the guest at its reset vector, while the others stop for good,
// to start again on waking.
void sleep_check(struct vcpu *v);
// What waking took from the processor, given back before it does anything
// else: its TSC, tables, local APICs, and virtualisation.
void sleep_cpu_wake(struct cpu *cpu);
// How far the hypervisor moved the TSC on after the machine last woke, back to
// where it stood as the machine slept: 0 before any sleep, and where the TSC
// went on counting.
uint64_t sleep_tsc_moved(void);

#endif
