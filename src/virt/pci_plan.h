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

#include <stdint.h>
#include <stdbool.h>

// Memory BARs up to this size go below 4 GiB, as do larger ones that cannot be
// above it; the rest stays above.
#define PCI_PLAN_LOW_BAR_MAX (256ull << 20)

struct pci_plan_range {
    uint64_t base;
    uint64_t end;
};

// Where the guest first finds a function's memory, by guest-physical address:
// 0 for a BAR or expansion ROM it does not find anywhere.
struct pci_plan_function {
    // A 64-bit BAR at the index of its lower register.
    uint64_t bars[6];
    uint64_t rom;
    // Bridges: the windows, inclusive, base above limit when closed.
    uint64_t mem_base;
    uint64_t mem_limit;
    uint64_t pref_base;
    uint64_t pref_limit;
};

struct pci_plan_root {
    uint16_t segment;
    uint8_t bus;
    // What its functions decode below and above 4 GiB, empty where base equals
    // end.
    struct pci_plan_range low;
    struct pci_plan_range high;
};

struct pci_plan {
    // In the order of the topology's functions and root buses.
    struct pci_plan_function *functions;
    struct pci_plan_root *roots;
    uint32_t root_count;
    // The hypervisor's own function on segment 0's bus 0, -1 if none, and its
    // BAR.
    int virt_device;
    uint64_t virt_bar;
    // Segment 0's configuration space window, for buses ecam_start to
    // ecam_end, at ecam (bus ecam_start's), or 0 if it has none.
    uint64_t ecam;
    uint8_t ecam_start;
    uint8_t ecam_end;
    // The guest's PCI hole starts here, and its larger BARs from high_floor.
    uint64_t tolud;
    uint64_t high_floor;
};

// Lays the guest's PCI memory out around obstacles, the sorted ranges below
// 4 GiB the guest finds at their addresses, as high below 4 GiB as it goes,
// with a function of the hypervisor's own if virt_bar_size is not 0, and the
// larger BARs from high_floor up. Needs pci_enum_scan() and pci_cfg_init().
const struct pci_plan *pci_plan_make(const struct pci_plan_range *obstacles, uint32_t count,
                                     uint64_t virt_bar_size, uint64_t high_floor);

// The plan, or NULL before pci_plan_make().
const struct pci_plan *pci_plan(void);

#endif
