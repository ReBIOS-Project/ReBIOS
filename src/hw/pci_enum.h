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
// You should have received a copy of the GNU General Public License along with
// ReBIOS. If not, see <https://www.gnu.org/licenses/>.

#ifndef HW__PCI_ENUM_H
#define HW__PCI_ENUM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <hw/pci_cfg.h>

#define PCI_ENUM_BAR_NONE 0
#define PCI_ENUM_BAR_IO 1
#define PCI_ENUM_BAR_MEM32 2
#define PCI_ENUM_BAR_MEM64 3

struct pci_enum_bar {
    // One of PCI_BAR_*. A 64-bit BAR is described by the entry of its lower
    // register; the entry of its upper register is PCI_ENUM_BAR_NONE.
    uint8_t kind;
    bool prefetchable;
    // Configuration space offset of its (lower) register.
    uint16_t reg;
    // As the firmware programmed it, and its size; size 0 means unimplemented.
    uintptr_t base;
    size_t size;
    // A Resizable BAR's largest size that the host has room for at its base:
    // the firmware's, before ReBIOS shrank it. 0 where that is its size, or it
    // is no Resizable BAR.
    size_t room;
};

struct pci_enum_function {
    struct pci_cfg_addr addr;
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
    uint16_t vf_rebar_ext_cap;
    // Device/Port Type from the PCI Express Capabilities register, valid if
    // pcie_cap is not 0.
    uint8_t pcie_port_type;

    // 6 BARs for header type 0, 2 for header type 1, none for type 2.
    size_t bar_count;
    struct pci_enum_bar bars[6];
    // The expansion ROM BAR, kind PCI_ENUM_BAR_MEM32 if implemented.
    struct pci_enum_bar rom;
    // A physical function's VFs (Single Root I/O Virtualization and Sharing
    // Specification 1.1): the most it can have, and its VF BARs, each the size
    // of one VF's at the default System Page Size of 4 KiB, holding total_vfs
    // of them in a row from its base.
    uint16_t total_vfs;
    struct pci_enum_bar vf_bars[6];

    // Header types 1 and 2.
    uint8_t primary_bus;
    uint8_t secondary_bus;
    uint8_t subordinate_bus;
    // Header type 1 windows, each inclusive, limit < base when the window is
    // closed. The prefetchable window is 64-bit when the bridge says so.
    uint32_t io_base;
    uint32_t io_limit;
    uintptr_t mem_base;
    uint32_t mem_limit;
    uintptr_t pref_base;
    uint64_t pref_limit;
    uint16_t bridge_control;

    // The bridge whose secondary bus this function is on, NULL on a root bus.
    struct pci_enum_function *parent;
};

struct pci_enum_root_bus {
    uint16_t segment;
    uint8_t bus;
};

struct pci_enum_topology {
    // Sorted by segment, bus, device, and function.
    size_t function_count;
    struct pci_enum_function *functions;
    // Sorted by segment and bus.
    size_t root_bus_count;
    struct pci_enum_root_bus *root_buses;
};

// Scans every reachable function. Needs pci_cfg_init(). Runs once; later calls
// return the same topology.
const struct pci_enum_topology *pci_enum_scan(void);

// The topology, or NULL before pci_enum_scan().
const struct pci_enum_topology *pci_enum_topology(void);

struct pci_enum_function *pci_enum_find(struct pci_cfg_addr addr);

// What the function's VF BAR b spans for all of its VFs: 0 where it has none.
uint64_t pci_enum_vf_span(const struct pci_enum_function *f, size_t b);

// Reads the function at vf->addr, a VF of the physical function pf that
// answers there, into vf: the Vendor ID of its PF and the Device ID that its
// PF's SR-IOV capability gives, as a VF's own read as all ones, and its class
// and capabilities. A VF has no BARs or expansion ROM of its own (Single Root
// I/O Virtualization and Sharing Specification 1.1, 3.4.1).
void pci_enum_vf_read(struct pci_enum_function *vf, const struct pci_enum_function *pf);

// The function of the PCI device that owns the legacy VGA resources, NULL
// where there is none.
struct pci_enum_function *pci_enum_vga(void);

// Shrinks each Resizable BAR above max_size, and records its new size.
void pci_enum_shrink_rebars(size_t max_size);

// Sizes the function's Resizable BARs as enumeration and
// pci_enum_shrink_rebars() left them, where a reset of the function put them
// back to its defaults. Returns whether it changed any.
bool pci_enum_rebar_restore(const struct pci_enum_function *f);

// Resizes the function's Resizable BAR index to size on the host, at its base,
// and records its new size: false where its capability does not offer that
// size, or the BAR does not take it.
bool pci_enum_rebar_resize(struct pci_enum_function *f, size_t index, size_t size);

#endif
