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

#ifndef HW__PCI_CFG_H
#define HW__PCI_CFG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The configuration mechanism's ports, CONFIG_ADDRESS and CONFIG_DATA, and
// CONFIG_ADDRESS's enable bit (PCI Local Bus Specification 3.0, 3.2.2.3.2).
#define PCI_CFG_ADDRESS 0xcf8
#define PCI_CFG_DATA 0xcfc
#define PCI_CFG_ENABLE ((uint32_t)1 << 31)

struct pci_cfg_addr {
    uint16_t segment;
    uint8_t bus;
    uint8_t device;
    uint8_t function;
};

// Uses the MCFG table's configuration space windows once acpi_init() has run,
// and the legacy configuration mechanism otherwise.
void pci_cfg_init(void);
// Once the namespace runs, uses the windows that pci_cfg_init() left unused
// for want of a reservation in the memory map, where the system board devices
// or the memory map's MMIO reserve them.
void pci_cfg_verify(void);

// size is 1, 2, or 4, and offset a multiple of it. Reads from a function that
// cannot be reached return all ones, and writes to it are dropped.
uint32_t pci_cfg_read(struct pci_cfg_addr addr, uint32_t offset, size_t size);
void pci_cfg_write(struct pci_cfg_addr addr, uint32_t offset, size_t size, uint32_t value);

// The configuration space window covering the segment and bus, or 0.
uintptr_t pci_cfg_ecam_base(uint16_t segment, uint8_t bus);

// Whether the configuration ports at CF8h and CFCh answer, as pci_cfg_init()
// found.
bool pci_cfg_ports_answer(void);

// For printf(), as segment:bus:device.function.
#define PCI_CFG_ADDR_FMT "%04x:%02x:%02x.%x"
#define PCI_CFG_ADDR_ARGS(a) (a).segment, (a).bus, (a).device, (a).function

#endif
