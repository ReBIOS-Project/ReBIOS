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

#include <stdint.h>
#include <stdbool.h>

// Finds Intel's integrated graphics function and the OpRegion its ASLS
// register gives. After pci_enum_scan(), before layout_init().
void igfx_scan(void);

// The host range the guest needs at its own address, if any: the raw VBT of
// an OpRegion 2.0, which the OpRegion that the guest shares with the host's
// AML gives by its physical address.
bool igfx_one_to_one(uint64_t *base, uint64_t *end);

// Gives the guest the OpRegion at an address in its own memory map, which
// its ASLS reads. After pci_virt_init().
void igfx_init(void);

#endif
