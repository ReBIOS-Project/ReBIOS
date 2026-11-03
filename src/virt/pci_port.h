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

#ifndef VIRT__PCI_PORT_H
#define VIRT__PCI_PORT_H

#include <stddef.h>
#include <stdint.h>

// The configuration space of the PCI plan's ports. Needs pci_plan_make().
void pci_port_init(void);

// The port at the device and function of the guest's bus 0, or -1.
int pci_port_at(uint8_t device, uint8_t function);

uint32_t pci_port_read(size_t port, uint32_t offset, size_t size);
void pci_port_write(
    size_t port, uint32_t offset, size_t size, uint32_t value
);

#endif
