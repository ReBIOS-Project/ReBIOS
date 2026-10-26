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

#ifndef LOADER__FWTABLES_H
#define LOADER__FWTABLES_H

#include <stdint.h>

// Fills loader_handoff->acpi_rsdp and smbios_entry.
void fwtables_collect(void);

// The string at offset in the firmware's first SMBIOS structure of type, as
// Linux 7.3's DMI code reads it (drivers/firmware/dmi_scan.c): "" for no
// string or only spaces, NULL where there is no such structure or field.
const char *fwtables_smbios_string(uint8_t type, uint8_t offset);

#endif
