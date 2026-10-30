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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Builds 4-level page tables identity mapping [0, *map_top), rounding *map_top
// up to what the tables actually cover, in one block of *pages pages below
// 4GiB, as the APs load CR3 outside of long mode. Returns the physical address
// of the PML4, or 0 on failure.
uintptr_t paging_build_identity_map(uintptr_t *map_top, size_t *pages);

// Adds [base, base + size) to those tables, uncached, for device memory the
// core reaches beyond *map_top. False if a table cannot be allocated.
bool paging_map_device(uintptr_t pml4, uintptr_t base, size_t size);

#endif
