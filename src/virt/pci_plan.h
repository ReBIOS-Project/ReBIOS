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

#ifndef VIRT__PCI_PLAN_H
#define VIRT__PCI_PLAN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Memory BARs up to this size go below 4 GiB, as do larger ones that cannot be
// above it; the rest stays above.
#define PCI_PLAN_LOW_BAR_MAX ((uint64_t)256 << 20)

struct pci_plan_range {
    uintptr_t base;
    uintptr_t end;
};

// Where the guest first finds a function's memory, by guest-physical address:
// 0 for a BAR or expansion ROM it does not find anywhere.
struct pci_plan_function {
    // A 64-bit BAR at the index of its lower register.
    uint64_t bars[6];
    uint64_t rom;
    // Bridges: the windows, inclusive, base above limit when closed.
    uintptr_t mem_base;
    uint64_t mem_limit;
    uintptr_t pref_base;
    uint64_t pref_limit;
};

struct pci_plan_root {
    uint16_t segment;
    uint8_t bus;
    // What its functions decode below and above 4 GiB, empty where base equals
    // end: the guest's bus 0's own, or the memory and prefetchable windows of
    // the port in its place.
    struct pci_plan_range low;
    struct pci_plan_range pref;
    struct pci_plan_range high;
};

// A function of the guest's bus 0 in place of a root bus of the host's other
// than the one the guest's bus 0 is: a PCI Express switch's upstream port,
// below which an operating system looks for every device of a bus, as it does
// not below a root port.
struct pci_plan_port {
    // The root bus behind it.
    size_t root;
    uint8_t device;
    uint8_t function;
    // The guest's numbers of that bus and of the last below it.
    uint8_t secondary;
    uint8_t subordinate;
    // Its windows, inclusive, base above limit when closed.
    uint32_t io_base;
    uint32_t io_limit;
    uintptr_t mem_base;
    uint64_t mem_limit;
    uintptr_t pref_base;
    uint64_t pref_limit;
};

// A bus number of the guest's, and the host's bus it is.
struct pci_plan_bus {
    bool present;
    uint16_t segment;
    uint8_t bus;
};

struct pci_plan {
    // In the order of the topology's functions and root buses.
    struct pci_plan_function *functions;
    struct pci_plan_root *roots;
    size_t root_count;
    // The root bus that is the guest's bus 0, and the ports in place of the
    // others the guest has, which take guest bus numbers that segment 0 does
    // not use where they are of other segments.
    size_t primary;
    struct pci_plan_port *ports;
    size_t port_count;
    struct pci_plan_bus buses[256];
    // The highest bus number in use.
    uint8_t bus_end;
    // The guest's configuration space window, for buses 0 to bus_end, or 0 if
    // the host has none.
    uintptr_t ecam;
    // The emulated VGA's linear framebuffer on segment 0's bus 0, or 0.
    uintptr_t lfb;
    // The guest's PCI hole starts here, and its larger BARs from high_floor.
    uintptr_t tolud;
    uint64_t high_floor;
};

// Lays the guest's PCI memory out around obstacles, the sorted ranges below 4
// GiB the guest finds at their addresses, as high below 4 GiB as it goes, with
// lfb_size bytes for the emulated VGA's linear framebuffer, and the larger
// BARs from high_floor up. Needs pci_enum_scan() and pci_cfg_init().
const struct pci_plan *pci_plan_make(
    const struct pci_plan_range *obstacles, size_t count, size_t lfb_size, uint64_t high_floor
);

// The plan, or NULL before pci_plan_make().
const struct pci_plan *pci_plan(void);

// The guest's number of a bus of the host's, or -1 where the guest does not
// have it.
int pci_plan_guest_bus(uint16_t segment, uint8_t bus);

#endif
