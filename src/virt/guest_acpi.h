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

#ifndef VIRT__GUEST_ACPI_H
#define VIRT__GUEST_ACPI_H

#include <stdint.h>
#include <stdbool.h>

#define GUEST_ACPI_RSDP_SIZE 36

// The firmware's FACS, which the guest must find where the firmware has it:
// the firmware reads the waking vector there, and shares the global lock.
// Returns false if there is none.
bool guest_acpi_facs(uint64_t *base, uint64_t *size);

// Copies the firmware's ACPI tables into guest memory below 4 GiB, but for
// those the guest must not or cannot use, and fills rsdp with an RSDP for the
// copies. Returns false if there are no tables to copy.
bool guest_acpi_build(uint8_t rsdp[GUEST_ACPI_RSDP_SIZE]);

#endif
