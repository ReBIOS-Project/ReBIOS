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

#ifndef LOADER__LOADER_H
#define LOADER__LOADER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <handoff.h>

// How long a notice that ReBIOS cannot run on the machine stays on the screen
// before the firmware goes on.
#define LOADER_NOTICE_SECONDS 10

// What the core is told, filled in by the loader's modules.
extern struct handoff *loader_handoff;

// Records pages that the hypervisor keeps for itself, which the guest never
// sees.
void loader_reserve(uintptr_t base, size_t pages);
// Makes room in the list of those pages for more ranges, which it may take
// pages for: false where the firmware has none. Not once boot services end.
bool loader_reserve_room(size_t more);
// Records the list's own pages, with the room that loader_reserve_room() made
// for them, once nothing more is taken.
void loader_reserve_list(void);

// Memory for the hypervisor, which the guest never sees. Returns 0 on failure.
// max_addr is inclusive; 0 means anywhere.
uint64_t loader_alloc_reserved(size_t pages, uintptr_t max_addr);

#endif
