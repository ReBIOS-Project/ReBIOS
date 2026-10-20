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

#ifndef IOMMU__VTD_H
#define IOMMU__VTD_H

#include <stdint.h>
#include <stdbool.h>
#include <iommu/iommu_tables.h>

// Checks that every VT-d unit can translate through second-stage tables in
// legacy mode, with queued invalidation, and sets *leaf_level to the largest
// level of leaf all of them take (0 4KiB, 1 2MiB, 2 1GiB).
bool vtd_probe(const struct iommu_tables *tables, int *leaf_level);

// Takes the units over from the firmware, and has them remap interrupts if
// they all can: before any interrupt source is programmed.
void vtd_start(void);

// Has every unit translate all DMA through the 4-level second-stage table at
// pml4.
void vtd_enable(uint64_t pml4);

// Interrupt remapping, if on: count consecutive entries of the table, or -1;
// an entry pointed at a vector on the processor with an APIC ID; the message
// and the I/O APIC entry that name an entry.
bool vtd_remap_on(void);
int vtd_remap_alloc(unsigned count);
void vtd_remap_free(int index, unsigned count);
void vtd_remap_set(int index, uint32_t apic_id, uint8_t vector, bool level);
void vtd_remap_message(int index, uint32_t *address, uint32_t *data);
uint64_t vtd_remap_rte(int index, uint8_t vector, bool level, bool active_low);

// Makes the units see changes to the tables made while they translate.
void vtd_flush(void);

void vtd_report_faults(void);

#endif
