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

#define PCI_CLASS_DISPLAY 0x03

// For printf(), as segment:bus:device.function.
#define PCI_ADDRESS_FMT "%04x:%02x:%02x.%x"
#define PCI_ADDRESS_ARGS(a) (a).segment, (a).bus, (a).devfn >> 3, (a).devfn & 7

// A PCI function the firmware enumerated.
struct pci_io_device {
    EFI_HANDLE handle;
    EFI_PCI_IO_PROTOCOL *io;
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
struct pci_io_device *pci_io_devices(EFI_UINTN *count);

// Finds the PCI function a device path leads to.
bool pci_io_device_on_path(
    EFI_DEVICE_PATH_PROTOCOL *path, struct pci_io_device *dev
);

// Whether a port above the function says it leads to a slot, as those above a
// card's own bridges do.
bool pci_io_device_in_slot(const struct pci_io_device *dev);

static inline bool pci_address_equal(
    struct handoff_pci_address a, struct handoff_pci_address b
) {
    return a.segment == b.segment && a.bus == b.bus && a.devfn == b.devfn;
}

static inline int pci_address_compare(
    struct handoff_pci_address a, struct handoff_pci_address b
) {
    uint32_t ka = (uint32_t)a.segment << 16 | (uint32_t)a.bus << 8 | a.devfn;
    uint32_t kb = (uint32_t)b.segment << 16 | (uint32_t)b.bus << 8 | b.devfn;
    return ka < kb ? -1 : ka > kb;
}

#endif
