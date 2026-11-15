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

#ifndef LOADER__ACPI_SAVE_H
#define LOADER__ACPI_SAVE_H

// With save_acpi: writes the ACPI tables that ReBIOS gave the guest at its
// last start, which the core kept in memory (virt/acpi_save.c), to
// rebios-guest-acpi.txt, and the firmware's to rebios-acpi.txt, both beside
// the loader's image, in acpidump's text, then reserves where the core keeps
// the guest's this time. Says on the screen where the files went, or why they
// did not. Before the loader allocates much, which could take the memory the
// core's copy is in.
void acpi_save_start(void);

#endif
