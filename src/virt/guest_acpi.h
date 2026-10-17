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

#ifndef VIRT__GUEST_ACPI_H
#define VIRT__GUEST_ACPI_H

#include <stdint.h>
#include <stdbool.h>

#define GUEST_ACPI_RSDP_SIZE 36

// Puts the guest's ACPI tables into its memory below 4 GiB: the hypervisor's
// FADT, FACS, and DSDT, an RSDT for ACPI 1.0, and copies of the firmware's
// tables that make sense to the guest as they are, with the processors the
// guest does not get marked unusable. Fills rsdp with an RSDP for them. Needs
// smp_start_aps() and vacpi_init(). Returns false if there is no usable ACPI.
bool guest_acpi_build(uint8_t rsdp[GUEST_ACPI_RSDP_SIZE]);

#endif
