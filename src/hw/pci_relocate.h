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

#ifndef HW__PCI_RELOCATE_H
#define HW__PCI_RELOCATE_H

#include <stdint.h>
#include <stdbool.h>

// Shrinks the Resizable BARs above 256 MiB, then lays out again below 4 GiB the
// memory BARs and bridge memory windows of each root bus that has any above
// it. Needs acpi_init() and pci_cfg_init().
void pci_relocate(void);

// After pci_relocate(): a range of size bytes, aligned to size (a power of two),
// inside a memory window below 4 GiB of the host bridge of the root bus segment:bus,
// that no claimed range and no BAR, expansion ROM BAR, or bridge window of any
// function uses. Ranges this returned count as used by later calls. False if
// there is none, or if the root bus has no host bridge.
bool pci_relocate_reserve(uint16_t segment, uint8_t bus, uint64_t size, uint64_t *base);

#endif
