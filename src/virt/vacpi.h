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

#ifndef VIRT__VACPI_H
#define VIRT__VACPI_H

#include <stddef.h>
#include <stdint.h>

#define VACPI_FADT_SIZE 116
#define VACPI_FACS_SIZE 64

// After irq_init(): starts the firmware's ACPI under the hypervisor and
// connects its events to the guest: the SCI, Notify(), and the power button,
// through PM1 and GPE registers the hypervisor emulates where the host has its
// own.
void vacpi_init(void);

// The guest's DSDT, generated from the firmware's namespace in ACPI 1.0 AML,
// and the device its methods call the firmware's through, which this places in
// guest memory. Needs the guest's PCI interrupt routing, irq_route_pci().
// Returns the table, which the caller frees, and its length, or NULL.
uint8_t *vacpi_dsdt(size_t *length);

// A revision 1 FADT and a FACS for the guest's tables.
void vacpi_fadt(uint8_t fadt[VACPI_FADT_SIZE], uint32_t facs, uint32_t dsdt);
void vacpi_facs(uint8_t facs[VACPI_FACS_SIZE]);

#endif
