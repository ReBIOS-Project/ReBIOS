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

#ifndef VIRT__CBTABLE_H
#define VIRT__CBTABLE_H

#include <stdint.h>

// Finds the table of coreboot that the firmware's ACPI device BOOT0000 or
// GOOGCB00 points to, where the guest finds one of its own instead, with the
// guest's memory map, the records that hold on the guest's machine, and the
// firmware data that they point to, which the guest reads where they say. Puts
// the table's memory, and that firmware data, among the device memory. Needs
// pci_host_scan(); before devmem_plan().
void cbtable_find(void);
// Writes the guest's table, with rsdp the address of the guest's ACPI RSDP, or
// 0. Needs devmem_map(), and the guest's memory map as layout_e820() gives it.
void cbtable_build(uint32_t rsdp);

#endif
