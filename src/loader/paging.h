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

#ifndef LOADER__PAGING_H
#define LOADER__PAGING_H

#include <stdint.h>

// Builds 4-level page tables identity mapping [0, *map_top), rounding
// *map_top up to what the tables actually cover, in one block of *pages pages
// below 4GiB, as the APs load CR3 outside of long mode. Returns the physical
// address of the PML4, or 0 on failure.
uint64_t paging_build_identity_map(uint64_t *map_top, uint64_t *pages);

#endif
