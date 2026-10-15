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

#ifndef IOMMU__AMDVI_H
#define IOMMU__AMDVI_H

#include <stdint.h>
#include <stdbool.h>
#include <iommu/iommu_tables.h>

// Checks that every AMD-Vi unit is reachable, and sets *leaf_level to the
// largest level of leaf all of them take (0 4KiB, 1 2MiB, 2 1GiB).
bool amdvi_probe(const struct iommu_tables *tables, int *leaf_level);

// Has every unit translate all DMA through the 4-level host page table at
// root, taking the units over from the firmware.
void amdvi_enable(uint64_t root);

// Makes the units see changes to the tables made while they translate.
void amdvi_flush(void);

void amdvi_report_faults(void);

#endif
