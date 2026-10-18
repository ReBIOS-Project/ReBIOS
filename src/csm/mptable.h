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

#ifndef CSM__MPTABLE_H
#define CSM__MPTABLE_H

#include <stdint.h>

// Writes the guest's MP table to buf: an MP floating pointer structure, then
// the MP configuration table it points to, as they will be at guest-physical
// address guest. Returns the length written, or 0 if there is no I/O APIC or
// buf is too small.
uint32_t mptable_build(void *buf, uint32_t size, uint32_t guest);

#endif
