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

#ifndef VIRT__IGFX_H
#define VIRT__IGFX_H

#include <stdbool.h>
#include <stdint.h>

// Finds Intel's integrated graphics function and the OpRegion its ASLS
// register gives. After pci_enum_scan(), before layout_init().
void igfx_scan(void);

// The host range the guest needs at its own address, if any: the raw VBT of an
// OpRegion 2.0, which the OpRegion that the guest shares with the host's AML
// gives by its physical address.
bool igfx_one_to_one(uint64_t *base, uint64_t *end);

// The host's data stolen memory that the guest gets at an address of its own,
// if any, and the host range the guest's own must stay clear of: the stolen
// memory and the GGTT's below it. layout_init() places it and tells
// igfx_stolen_placed() where.
bool igfx_stolen(
    uint64_t *host, uint64_t *size, uint64_t *clear_base, uint64_t *clear_end
);
void igfx_stolen_placed(uintptr_t gpa);
// Whether the stolen memory's RMRR is gone: where layout_init() cannot place
// the stolen memory, it keeps the host's addresses for it as the RMRR would.
bool igfx_stolen_rmrr_dropped(void);

// Gives the guest the OpRegion and the stolen memory at the addresses in its
// own memory map, which the registers that give them read. After
// pci_virt_init().
void igfx_init(void);

// Around dma_enable(): the firmware's GGTT, whose entries give host addresses,
// keeps working while the IOMMU starts translating, then gives the guest's.
void igfx_switch_begin(void);
void igfx_switch_end(void);

#endif
