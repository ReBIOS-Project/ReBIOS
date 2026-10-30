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

#ifndef IOMMU__AMDVI_H
#define IOMMU__AMDVI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <iommu/iommu_tables.h>

// Checks that every AMD-Vi unit is reachable, and sets *leaf_level to the
// largest level of leaf all of them take (0 4KiB, 1 2MiB, 2 1GiB).
bool amdvi_probe(const struct iommu_tables *tables, size_t *leaf_level);

// Takes the units over from the firmware, DMA and interrupts passing
// untranslated meanwhile.
void amdvi_start(void);

// Has every unit translate all DMA through the 4-level host page table at
// root, and, with remapping on, block the interrupts no table covers.
void amdvi_enable(uintptr_t root);

// Makes the units see changes to the tables made while they translate.
void amdvi_flush(void);

void amdvi_report_faults(void);

// Interrupt remapping, once the I/O APICs are known: tables per device ID, of
// the requester IDs that interrupt messages carry.
void amdvi_remap_start(void);
bool amdvi_remap_on(void);
int amdvi_remap_alloc(int32_t requester, size_t count);
void amdvi_remap_free(int32_t requester, int index, size_t count);
void amdvi_remap_set(
    int32_t requester, int index, uint32_t apic_id, uint8_t vector
);
void amdvi_remap_message(int index, uint32_t *address, uint32_t *data);
uint64_t amdvi_remap_rte(int index, bool level, bool active_low);

#endif
