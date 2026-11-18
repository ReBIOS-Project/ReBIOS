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

#ifndef LOADER__KEPT_H
#define LOADER__KEPT_H

#include <stdbool.h>
#include <stdint.h>

// Reserves size bytes for the hypervisor where the next start finds them as a
// restart left them, and returns their address, or 0 where there is no room:
// in the middle of the largest free memory from 16 MiB up to 4 GiB, away from
// both the regions that firmware uses at fixed low addresses while it starts,
// such as OVMF's decompression scratch, and what it hands out from the top
// down.
uint64_t kept_reserve(uint64_t size);

// Calls found(page, end, ctx), until it returns true, for each page of free
// memory from 16 MiB up to 4 GiB, end being where the free memory that the
// page is in ends.
void kept_find(bool (*found)(uintptr_t page, uint64_t end, void *ctx), void *ctx);

#endif
