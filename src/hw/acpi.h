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

#include <stdint.h>
#include <stdbool.h>

// Sets up table access, switches the platform to ACPI mode, then loads the
// namespace, without initialising any device. Returns false if there is no
// usable ACPI; table access may still work if only the namespace failed, which
// acpi_tables_ready() tells.
bool acpi_init(void);
bool acpi_tables_ready(void);

// Serialises the hypervisor's use of uACPI across processors. Releasing it
// runs the work uACPI deferred meanwhile: GPE methods and Notify() handlers.
void acpi_lock(void);
void acpi_unlock(void);

// Initialises the namespace where the hypervisor runs the firmware's ACPI,
// its embedded controller first, then enables the GPEs that have methods.
// Needs irq_init().
void acpi_namespace_start(void);

// The SCI's interrupt as the FADT has it, which the caller connects to
// acpi_sci_interrupt(). Returns false if uACPI has no SCI handler.
bool acpi_sci_irq(uint32_t *irq);
void acpi_sci_interrupt(void *ctx);

#endif
