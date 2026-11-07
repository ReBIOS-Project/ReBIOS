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

#ifndef HW__ACPI_H
#define HW__ACPI_H

#include <stdbool.h>
#include <stdint.h>

// Sets up table access, switches the platform to ACPI mode, then loads the
// namespace, without initialising any device. Returns false if there is no
// usable ACPI; table access may still work if only the namespace failed, which
// acpi_tables_ready() tells.
bool acpi_init(void);
bool acpi_tables_ready(void);

// Serialises the hypervisor's use of uACPI across processors, outside the work
// it defers: GPE methods and Notify() handlers, which run in a task once
// acpi_namespace_start() is done, and before that when this is released.
void acpi_lock(void);
void acpi_unlock(void);

// Initialises the namespace where the hypervisor runs the firmware's ACPI, its
// embedded controller first, then enables the GPEs that have methods. Needs
// irq_init().
void acpi_namespace_start(void);

// Runs the deferred work's task until this CPU's time slice is over, unless it
// has none, sleeps, or runs elsewhere, or needs the boot processor and this is
// another; whether the task has work.
void acpi_slice(void);
bool acpi_working(void);

// Whether the firmware shows a device with one of the IDs, a NULL-ended list,
// as present or functioning by its _STA.
bool acpi_has_device(const char *const *ids);

// Whether a device the firmware shows has the I/O port among its current
// resources.
bool acpi_claims_port(uint16_t port);
// Whether a system board device, PNP0C01 or PNP0C02, has all of [base, end) in
// one memory range of its current resources, as firmware reserves ECAM space
// (Linux 7.3's is_acpi_reserved()).
bool acpi_motherboard_reserves(uint64_t base, uint64_t end);

// The SCI's interrupt as the FADT has it, which the caller connects to
// acpi_sci_interrupt(). Returns false if uACPI has no SCI handler.
bool acpi_sci_irq(uint32_t *irq);
void acpi_sci_interrupt(void *ctx);

#endif
