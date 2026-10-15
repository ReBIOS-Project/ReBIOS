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

#ifndef HANDOFF_H
#define HANDOFF_H

#include <stdint.h>

#define HANDOFF_MAX_RESERVED 16

struct handoff_range {
    uint64_t base;
    uint64_t size;
};

// What the loader passes to the core.
struct handoff {
    // Memory the hypervisor keeps, which the guest must never reach: ReBIOS's
    // own image, the core's heap, page tables, and what the loader hands over.
    struct handoff_range reserved[HANDOFF_MAX_RESERVED];
    uint32_t reserved_count;

    // The page tables the core starts on, identity mapping the bottom of the
    // physical address space up to map_top.
    uint64_t pml4;
    uint64_t map_top;

    // Memory the core manages itself.
    uint64_t heap_base;
    uint64_t heap_size;

    // The memory map current at ExitBootServices().
    uint64_t efi_mmap;
    uint64_t efi_mmap_size;
    uint64_t efi_mmap_desc_size;
    uint32_t efi_mmap_desc_version;
};

#endif
