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

#ifndef VIRT__PCI_VIRT_H
#define VIRT__PCI_VIRT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <hw/pci_cfg.h>
#include <virt/mmio.h>

// Takes the guest's configuration space accesses over, through ports CF8h and
// CFCh and through the configuration space windows, segment 0's where the plan
// puts it and the others' where the host has them, and gives the guest the
// plan's BARs and bridge windows, mapped onto the host's BARs. Needs
// layout_init().
void pci_virt_init(void);

// Registers of a function that another module keeps: read gives the dword at
// the aligned reg, write merges the bytes of value under mask into it, and
// each returns false for a register that is not one of its own.
struct pci_virt_hook {
    bool (*read)(void *ctx, uint32_t reg, uint32_t *value);
    bool (*write)(void *ctx, uint32_t reg, uint32_t value, uint32_t mask);
};

// Hands the function's registers to hook before anything else, or no longer
// for NULL. After pci_virt_init().
void pci_virt_hook(
    struct pci_addr addr, const struct pci_virt_hook *hook, void *ctx
);

// Hands the guest's accesses to the pages holding [offset, offset + size) of a
// memory BAR of the function to ops, wherever the guest puts the BAR, with
// offsets from the first of those pages. False where another range has any of
// those pages, an MSI-X table's among them, where the BAR has as many ranges
// as it can take, or where it is smaller than a page. After pci_virt_init().
bool pci_virt_trap(
    struct pci_addr addr, unsigned int bar, uint64_t offset, size_t size,
    const struct mmio_ops *ops, void *ctx
);
// Gives the guest the pages of the range at offset back, mapped as the rest of
// the BAR is.
void pci_virt_untrap(struct pci_addr addr, unsigned int bar, uint64_t offset);

#endif
