// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of ReBIOS.
// Copyright (C) 2026 Mintsuki and contributors
//
// ReBIOS is free software: you can redistribute it and/or modify it under the
// terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version.
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
// wherever the host's own map allows, backed by whatever host memory is free,
// and the host ranges the guest must find at their own addresses.

// Maps the guest's memory, and the rest of the physical address space one to
// one, except host memory the guest has no business with.
void layout_init(void);

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
bool layout_is_absent(uint64_t gpa);

// Fills the guest's memory map.
bool layout_e820(struct e820_map *map);

#endif
