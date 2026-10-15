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

#ifndef HW__PCI_CFG_H
#define HW__PCI_CFG_H

#include <stdint.h>
#include <stdbool.h>

struct pci_addr {
    uint16_t segment;
    uint8_t bus;
    uint8_t device;
    uint8_t function;
};

// Uses the MCFG table's configuration space windows once acpi_init() has run,
// and the legacy configuration mechanism otherwise.
void pci_cfg_init(void);

// size is 1, 2, or 4, and offset a multiple of it. Reads from a function that
// cannot be reached return all ones, and writes to it are dropped.
uint32_t pci_cfg_read(struct pci_addr addr, uint32_t offset, unsigned size);
void pci_cfg_write(struct pci_addr addr, uint32_t offset, unsigned size, uint32_t value);

// The configuration space window covering the segment and bus, or 0.
uint64_t pci_cfg_ecam_base(uint16_t segment, uint8_t bus);

#endif
