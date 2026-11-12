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

#ifndef HW__PCI_ASSIGN_H
#define HW__PCI_ASSIGN_H

// Gives each memory BAR that the firmware left unassigned an address in the
// window of the bus its function is on, programs it, and records it in the
// topology. Needs pci_enum_scan() and acpi_init().
void pci_assign_unassigned_bars(void);

#endif
