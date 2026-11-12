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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <uacpi/namespace.h>
#include <uacpi/types.h>

struct pci_host_range {
    uintptr_t base;
    uintptr_t end;
};

struct pci_host_window {
    uintptr_t base;
    uintptr_t end;
    bool prefetchable;
};

struct pci_host_bridge {
    uacpi_namespace_node *node;
    uint16_t segment;
    uint8_t bus;
    // The memory windows of its _CRS, as given.
    struct pci_host_window *windows;
    size_t window_count;
};

struct pci_host {
    struct pci_host_bridge *bridges;
    size_t bridge_count;
    // Sorted and page granular, as the ranges below: the memory of the
    // interrupt controllers and of interrupt messages, which the hypervisor
    // answers at these addresses.
    struct pci_host_range *interrupts;
    size_t interrupt_count;
    // The memory of the HPETs and of the ACPI devices but host bridges,
    // motherboard resources, and TPMs, which the guest finds at addresses of
    // its own.
    struct pci_host_range *devices;
    size_t device_count;
    // The TPMs' memory, which software finds where the TCG's PC Client specs
    // put it.
    struct pci_host_range *tpms;
    size_t tpm_count;
    // The rest of the memory that motherboard resources and host bridges
    // consume.
    struct pci_host_range *motherboard;
    size_t motherboard_count;
    // Sorted and page granular: the configuration space windows and the
    // IOMMUs' registers, which the guest must never reach at their addresses.
    struct pci_host_range *private;
    size_t private_count;
};

// The host bridges of the ACPI namespace and the memory around them that is
// spoken for. Needs acpi_init(). Runs once; later calls return the same.
const struct pci_host *pci_host_scan(void);

#endif
