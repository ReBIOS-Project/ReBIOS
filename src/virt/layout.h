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

#ifndef VIRT__LAYOUT_H
#define VIRT__LAYOUT_H

#include <stdint.h>
#include <stdbool.h>
#include <csm/e820.h>

// The guest-physical memory map: RAM where a PC has it, contiguous from 1 MiB
// to the guest's PCI hole, backed by whatever host memory is free, and the
// host ranges the guest must find at their own addresses.

// Lays out the guest's PCI memory, pci_plan_make(), then maps the guest's RAM
// and what it finds at host addresses; what is mapped nowhere reads as absent.
// Needs pci_enum_scan().
void layout_init(void);

// Where layout_init() put the emulated VGA's linear framebuffer, vga_lfb_size()
// bytes of bus 0's PCI memory that no BAR uses, or 0 if it put none.
uint64_t layout_lfb(void);

// Whether [base, base + size) is clear of the guest's RAM, of what it finds
// at host addresses, of its configuration space window, and of the linear
// framebuffer, for a BAR there.
bool layout_mmio_free(uint64_t base, uint64_t size);
// Whether [gpa, gpa + size) is all the guest's RAM.
bool layout_ram(uint64_t gpa, uint64_t size);
// The host address of pages pages of the guest's RAM from gpa, for the
// processor to write to by host address: they move to contiguous host memory,
// contents and all, while the other vCPUs wait, where they are not. False
// where that would take more of the hypervisor's memory than it gives.
bool layout_contiguous(uint64_t gpa, unsigned pages, uint64_t *hpa);

// The hypervisor's view of guest-physical address gpa, below 1 MiB and
// outside the VGA window.
void *layout_low(uint32_t gpa);

// Takes size bytes, rounded up to pages, off the top of the guest's RAM below
// 4 GiB, at an align boundary, for memory the guest's memory map shows as
// e820_type. Returns 0 if there is no room.
uint32_t layout_take(uint32_t size, uint32_t align, uint32_t e820_type);

// Makes [gpa, gpa + size), page aligned, read as all ones and ignore writes,
// as addresses nothing decodes do.
void layout_absent(uint64_t gpa, uint64_t size);

// Makes [gpa, gpa + size) of the guest's first megabyte read-only, as a BIOS
// locks shadowed ROMs: writes go nowhere. Page aligned.
void layout_read_only(uint64_t gpa, uint64_t size);

// Fills the guest's memory map.
bool layout_e820(struct e820_map *map);

#endif
