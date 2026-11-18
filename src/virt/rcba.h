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

#ifndef VIRT__RCBA_H
#define VIRT__RCBA_H

// Finds the Root Complex Register Block of Intel's chipsets that have one, the
// chipset configuration registers that hold the SPI controller that flashing
// tools drive, through the LPC bridge's RCBA register, where the firmware's
// ACPI claims it as a motherboard resource; the guest finds it at an address
// of its own, in the block of its ACPI devices' memory. Needs pci_enum_scan(),
// and comes before layout_init().
void rcba_find(void);
// Gives the guest the block found. Needs pci_virt_init().
void rcba_init(void);

#endif
