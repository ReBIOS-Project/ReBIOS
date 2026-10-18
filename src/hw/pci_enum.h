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

#ifndef HW__PCI_ENUM_H
#define HW__PCI_ENUM_H

#include <stdint.h>
#include <stdbool.h>
#include <hw/pci_cfg.h>

#define PCI_BAR_NONE 0
#define PCI_BAR_IO 1
#define PCI_BAR_MEM32 2
#define PCI_BAR_MEM64 3

struct pci_bar {
    // One of PCI_BAR_*. A 64-bit BAR is described by the entry of its lower
    // register; the entry of its upper register is PCI_BAR_NONE.
    uint8_t kind;
    bool prefetchable;
    // Configuration space offset of its (lower) register.
    uint8_t reg;
    // As the firmware programmed it, and its size; size 0 means unimplemented.
    uint64_t base;
    uint64_t size;
};

struct pci_function {
    struct pci_addr addr;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t base_class;
    uint8_t subclass;
    uint8_t prog_if;
    uint8_t revision;
    // Header layout, without the multi-function bit: 0, 1 (PCI-to-PCI bridge),
    // or 2 (CardBus bridge).
    uint8_t header_type;
    uint8_t interrupt_pin;
    uint8_t interrupt_line;

    // Capability offsets, 0 if absent.
    uint8_t pm_cap;
    uint8_t msi_cap;
    uint8_t msix_cap;
    uint8_t pcie_cap;
    uint16_t rebar_ext_cap;
    uint16_t sriov_ext_cap;
    // Device/Port Type from the PCI Express Capabilities register, valid if
    // pcie_cap is not 0.
    uint8_t pcie_port_type;

    // 6 BARs for header type 0, 2 for header type 1, none for type 2.
    uint8_t bar_count;
    struct pci_bar bars[6];
    // The expansion ROM BAR, kind PCI_BAR_MEM32 if implemented.
    struct pci_bar rom;

    // Header types 1 and 2.
    uint8_t primary_bus;
    uint8_t secondary_bus;
    uint8_t subordinate_bus;
    // Header type 1 windows, each inclusive, limit < base when the window is
    // closed. The prefetchable window is 64-bit when the bridge says so.
    uint32_t io_base;
    uint32_t io_limit;
    uint32_t mem_base;
    uint32_t mem_limit;
    uint64_t pref_base;
    uint64_t pref_limit;
    uint16_t bridge_control;

    // The bridge whose secondary bus this function is on, NULL on a root bus.
    struct pci_function *parent;
};

struct pci_root_bus {
    uint16_t segment;
    uint8_t bus;
};

struct pci_topology {
    // Sorted by segment, bus, device, and function.
    uint32_t function_count;
    struct pci_function *functions;
    // Sorted by segment and bus.
    uint32_t root_bus_count;
    struct pci_root_bus *root_buses;
};

// Scans every reachable function. Needs pci_cfg_init(). Runs once; later
// calls return the same topology.
const struct pci_topology *pci_enum_scan(void);

// The topology, or NULL before pci_enum_scan().
const struct pci_topology *pci_enum_topology(void);

struct pci_function *pci_enum_find(struct pci_addr addr);

// Shrinks each Resizable BAR above max_size, and records its new size.
void pci_enum_shrink_rebars(uint64_t max_size);

#endif
