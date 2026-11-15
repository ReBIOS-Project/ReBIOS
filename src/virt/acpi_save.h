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

#ifndef VIRT__ACPI_SAVE_H
#define VIRT__ACPI_SAVE_H

#include <stdint.h>

// With save_acpi, copies the ACPI tables that the guest finds from its RSDP at
// rsdp into the memory that the loader reserved for them, in the form of
// struct handoff_acpi_save, which the loader writes out at the next start
// (loader/acpi_save.c). Once the guest's tables are in place, before the guest
// runs.
void acpi_save_guest(uint32_t rsdp);

#endif
