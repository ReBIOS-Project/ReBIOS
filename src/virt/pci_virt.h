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

#ifndef VIRT__PCI_VIRT_H
#define VIRT__PCI_VIRT_H

#include <stdint.h>
#include <stdbool.h>

// The configuration space of a function the hypervisor presents itself. offset
// is below 4096 and a multiple of size, which is 1, 2, or 4.
struct pci_virt_ops {
    uint32_t (*read)(void *ctx, uint32_t offset, unsigned size);
    void (*write)(void *ctx, uint32_t offset, unsigned size, uint32_t value);
};

// Presents a single-function device on a device number of segment 0's bus 0
// that no hardware uses, through the configuration mechanism of ports CF8h and
// CFCh and through the bus's ECAM window, which reach every other function as
// before. Needs pci_enum_scan(). Returns the device number, or -1 if no device
// number is free.
int pci_virt_add(const struct pci_virt_ops *ops, void *ctx);

#endif
