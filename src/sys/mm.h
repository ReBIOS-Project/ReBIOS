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

#ifndef SYS__MM_H
#define SYS__MM_H

#include <stdint.h>
#include <stddef.h>

#define PAGE_SIZE 4096

void mm_init(uint64_t base, uint64_t size);

// Zeroed, page aligned, physically contiguous. Panics when out of memory, as
// nothing the core allocates is optional.
void *page_alloc(size_t count);
// align is in bytes, a multiple of the page size.
void *page_alloc_aligned(size_t count, size_t align);
void page_free(void *pages, size_t count);

#endif
