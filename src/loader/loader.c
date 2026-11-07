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
#include <loader/loader.h>
#include <handoff.h>
#include <lib/log.h>
#include <loader/efi_glue.h>

struct handoff *loader_handoff;

void loader_reserve(uintptr_t base, size_t pages) {
    if (loader_handoff->reserved_count == HANDOFF_MAX_RESERVED) {
        panic("Too many hypervisor memory ranges\n");
    }
    loader_handoff->reserved[loader_handoff->reserved_count].base = base;
    loader_handoff->reserved[loader_handoff->reserved_count].size = pages * 4096;
    loader_handoff->reserved_count++;
}

uint64_t loader_alloc_reserved(size_t pages, uintptr_t max_addr) {
    uintptr_t base = efi_glue_alloc_pages(pages, max_addr);
    if (base != 0) {
        loader_reserve(base, pages);
    }
    return base;
}
