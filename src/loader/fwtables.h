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

#ifndef LOADER__FWTABLES_H
#define LOADER__FWTABLES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Fills loader_handoff->acpi_rsdp and smbios_entry.
void fwtables_collect(void);

// The string at offset in the firmware's first SMBIOS structure of type, as
// Linux 7.3's DMI code reads it (drivers/firmware/dmi_scan.c): "" for no
// string or only spaces, NULL where there is no such structure or field.
const char *fwtables_smbios_string(uint8_t type, uint8_t offset);

// The address of the RSDP in the firmware's configuration tables, ACPI 2.0's
// where it is sound, else ACPI 1.0's, or 0. Needs no fwtables_collect().
uintptr_t fwtables_acpi_rsdp(void);
// The root table that RSDP gives, as fwtables.c takes it, its entries
// entry_size bytes each, or NULL.
const uint8_t *fwtables_acpi_root(size_t *entry_size);

// Gets the FADT's preferred power management profile, if there is one.
bool fwtables_fadt_pm_profile(uint8_t *profile);

// The I/O port of the 16550 or 16450 the firmware's SPCR table, or else its
// DBG2 table, describes, or 0, and its clock if the table gives it, else 0.
// Needs no fwtables_collect().
uint16_t fwtables_serial_port(uint32_t *clock);

// Gets the byte at offset in the firmware's first SMBIOS structure of type, if
// there is such a structure and field.
bool fwtables_smbios_byte(uint8_t type, uint8_t offset, uint8_t *value);

#endif
