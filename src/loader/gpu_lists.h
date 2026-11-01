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

#ifndef LOADER__GPU_LISTS_H
#define LOADER__GPU_LISTS_H

#include <stddef.h>
#include <stdint.h>

#define GPU_ANY_REVISION (-1)

// PCI device IDs of a vendor from first to last, of one PCI revision or of
// any.
struct gpu_id {
    uint16_t vendor;
    uint16_t first;
    uint16_t last;
    int16_t revision;
};

// Integrated graphics and GPUs soldered to the board from 2018 on, whose Video
// BIOS does not run on its own, and takes precedence over the allowlist.
extern const struct gpu_id vbios_blocklist[];
extern const size_t vbios_blocklist_count;

// Those from before 2018, and cards for slots, whose Video BIOS runs.
extern const struct gpu_id vbios_allowlist[];
extern const size_t vbios_allowlist_count;

#endif
