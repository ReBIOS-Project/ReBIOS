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

#ifndef HW__PCI_HOST_H
#define HW__PCI_HOST_H

#include <stdint.h>
#include <stdbool.h>
#include <uacpi/namespace.h>

struct pci_host_range {
    uint64_t base;
    uint64_t end;
};

struct pci_host_window {
    uint64_t base;
    uint64_t end;
    bool prefetchable;
};

struct pci_host_bridge {
    uacpi_namespace_node *node;
    uint16_t segment;
    uint8_t bus;
    // The memory windows of its _CRS, as given.
    struct pci_host_window *windows;
    uint32_t window_count;
};

struct pci_host {
    struct pci_host_bridge *bridges;
    uint32_t bridge_count;
    // Sorted and page granular: memory the interrupt controllers, the HPETs,
    // interrupt messages, and the ACPI devices other than host bridges use,
    // which software finds at these addresses, but for the configuration space
    // windows and the IOMMUs.
    struct pci_host_range *claimed;
    uint32_t claimed_count;
    // Sorted and page granular: the configuration space windows and the
    // IOMMUs' registers, which the guest must never reach at their addresses.
    struct pci_host_range *private;
    uint32_t private_count;
};

// The host bridges of the ACPI namespace and the memory around them that is
// spoken for. Needs acpi_init(). Runs once; later calls return the same.
const struct pci_host *pci_host_scan(void);

#endif
