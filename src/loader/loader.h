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

#ifndef LOADER__LOADER_H
#define LOADER__LOADER_H

#include <stdint.h>
#include <handoff.h>

// What the core is told, filled in by the loader's modules.
extern struct handoff *loader_handoff;

// Memory for the hypervisor, which the guest never sees. Returns 0 on failure.
// max_addr is inclusive; 0 means anywhere.
uint64_t loader_alloc_reserved(uint64_t pages, uint64_t max_addr);

#endif
