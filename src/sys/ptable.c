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

#include <stddef.h>
#include <stdint.h>
#include <sys/ptable.h>
#include <sys/mm.h>
#include <sys/x86.h>

uint64_t *ptable_table_for(
    const struct ptable_format *format,
    uint64_t *root,
    uintptr_t address,
    size_t level
) {
    uint64_t *table = root;
    for (size_t l = 3; l > level; l--) {
        uint64_t *e = &table[x86_level_index(address, l)];
        if (!format->present(*e)) {
            uint64_t *next = page_alloc(1);
            *e = format->table((uintptr_t)next, l);
        } else if (format->large(*e)) {
            uintptr_t base = x86_leaf_address(*e, l);
            uint64_t attributes = format->leaf_attributes(*e, l);
            uint64_t *next = page_alloc(1);
            for (size_t i = 0; i < 512; i++) {
                next[i] = format->leaf(base + i * x86_level_size(l - 1), attributes, l - 1);
            }
            *e = format->table((uintptr_t)next, l);
        }
        table = (uint64_t *)(uintptr_t)(*e & PTE_ADDRESS);
    }
    return table;
}

void ptable_map(
    const struct ptable_format *format,
    uint64_t *root,
    uintptr_t address,
    uintptr_t target,
    size_t size,
    uint64_t attributes,
    size_t max_level
) {
    while (size != 0) {
        size_t level = 0;
        for (size_t l = max_level; l > 0; l--) {
            size_t ls = x86_level_size(l);
            if ((address & (ls - 1)) == 0 && (target & (ls - 1)) == 0 && size >= ls) {
                level = l;
                break;
            }
        }

        uint64_t *table = ptable_table_for(format, root, address, level);
        // A leaf that replaces a table leaks the table, as processors or
        // IOMMUs may still walk it until their caches are flushed.
        table[x86_level_index(address, level)] = format->leaf(target, attributes, level);

        address += x86_level_size(level);
        target += x86_level_size(level);
        size -= x86_level_size(level);
    }
}

uint64_t *ptable_leaf(
    const struct ptable_format *format,
    uint64_t *root,
    uintptr_t address,
    size_t *level,
    uint64_t *entry
) {
    uint64_t *table = root;
    for (size_t l = 4; l-- > 0; ) {
        uint64_t *e = &table[x86_level_index(address, l)];
        // Read once, as a large page may turn into a table meanwhile.
        uint64_t value = *e;
        if (!format->present(value)) {
            return NULL;
        }
        if (l == 0 || format->large(value)) {
            *level = l;
            *entry = value;
            return e;
        }
        table = (uint64_t *)(uintptr_t)(value & PTE_ADDRESS);
    }
    return NULL;
}
