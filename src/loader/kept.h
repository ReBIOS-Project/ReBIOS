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
#include <stddef.h>
#include <stdint.h>
#include <handoff.h>

// Reserves size bytes for the hypervisor where the next start finds them as a
// restart left them, and returns their address, or 0 where there is no room:
// in the middle of the largest free memory from 16 MiB up to 4 GiB, away from
// both the regions that firmware uses at fixed low addresses while it starts,
// such as OVMF's decompression scratch, and what it hands out from the top
// down.
uint64_t kept_reserve(uint64_t size);

// Where kept_reserve() would put size bytes now, with [taken, taken +
// taken_size) already taken where taken_size is not 0, or 0 where there is no
// room. Takes nothing.
uint64_t kept_place(uint64_t size, uint64_t taken, uint64_t taken_size);

// Whether [base, base + size) is free memory, all in one range of the
// firmware's memory map.
bool kept_free(uint64_t base, uint64_t size);

// Calls found(page, end, ctx), until it returns true, for each page of free
// memory from 16 MiB up to 4 GiB, end being where the free memory that the
// page is in ends.
void kept_find(bool (*found)(uintptr_t page, uint64_t end, void *ctx), void *ctx);

// Copies the header of a kept area at page (handoff.h), its parity at
// parity_at, to header, corrected, where it has magic: false where it does
// not, or more of it is in error than its parity corrects.
bool kept_header(
    uintptr_t page,
    const char magic[static HANDOFF_KEPT_MAGIC_SIZE],
    void *header,
    size_t parity_at
);

#endif
