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

#ifndef SYS__PTABLE_H
#define SYS__PTABLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// A format of 4-level paging structures shaped as x86 paging's: tables of 512
// entries, from level 3 down to level 0's 4 KiB pages, whose entries hold the
// addresses they lead to in the bits of PTE_ADDRESS.
struct ptable_format {
    // Whether an entry maps memory or leads to a table.
    bool (*present)(uint64_t entry);
    // Whether a present entry above level 0 maps memory rather than a table.
    bool (*large)(uint64_t entry);
    // The entry at level that leads to the table at address.
    uint64_t (*table)(uintptr_t address, size_t level);
    // The entry at level that maps memory at address, with attributes that
    // only the format gives a meaning to.
    uint64_t (*leaf)(uintptr_t address, uint64_t attributes, size_t level);
    // The attributes of a leaf at level, for smaller leaves to map alike.
    uint64_t (*leaf_attributes)(uint64_t entry, size_t level);
};

// The table under root that holds the entry for address at level, making the
// tables on the way, and splitting the large pages on the way into tables of
// smaller ones that map the same.
uint64_t *ptable_table_for(
    const struct ptable_format *format, uint64_t *root, uintptr_t address, size_t level
);

// Maps [address, address + size) to [target, target + size), both page
// aligned, as size is, with the largest pages that fit, up to max_level.
void ptable_map(
    const struct ptable_format *format, uint64_t *root, uintptr_t address, uintptr_t target,
    size_t size, uint64_t attributes, size_t max_level
);

// The entry of the leaf under root that maps address, or NULL if none does,
// with the leaf's level and its value as found.
uint64_t *ptable_leaf(
    const struct ptable_format *format, uint64_t *root, uintptr_t address, size_t *level,
    uint64_t *entry
);

#endif
