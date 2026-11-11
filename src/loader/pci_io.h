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

#ifndef LOADER__PCI_IO_H
#define LOADER__PCI_IO_H

#include <stdbool.h>
#include <stdint.h>
#include <efi.h>
#include <handoff.h>

// A PCI function the firmware enumerated.
struct pci_io_device {
    efi_handle handle;
    struct efi_pci_io_protocol *io;
    struct handoff_pci_address address;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t revision;
    uint8_t base_class;
    uint8_t subclass;
    uint8_t prog_if;
};

// Returns the PCI functions, in handle order, in pool memory to free with
// FreePool(), or NULL if there are none.
struct pci_io_device *pci_io_devices(size_t *count);

// Finds the PCI function a device path leads to.
bool pci_io_device_on_path(struct efi_device_path_protocol *path, struct pci_io_device *dev);

// Enables I/O decoding of the PCI function (and of the bridges above it) whose
// I/O BAR holds a UART at the port, as on a serial card the firmware has no
// driver for. Returns whether one does, and which in found.
bool pci_io_enable_io_port(uint16_t port, struct handoff_pci_address *found);

// Enables I/O, memory and bus-master decoding of the function and the bridges
// above it, as a card the firmware has no driver for needs before its option
// ROM runs.
void pci_io_enable_device(const struct pci_io_device *dev);

// Whether a port above the function says it leads to a slot, as those above a
// card's own bridges do.
bool pci_io_device_in_slot(const struct pci_io_device *dev);

#endif
