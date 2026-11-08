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

#ifndef SYS__CACHE_H
#define SYS__CACHE_H

#include <stdint.h>

// The host's memory types, the same on every processor: loads the PAT, and
// gives each other processor the MTRRs the boot processor has, where the
// firmware left it others. The boot processor's call comes before the others',
// with interrupts disabled on each.
void cache_cpu_init(uint64_t pat);

#endif
