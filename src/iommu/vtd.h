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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <iommu/iommu_tables.h>

// Checks that every VT-d unit can translate through second-stage tables in
// legacy mode, with queued invalidation, and sets *leaf_level to the largest
// level of leaf all of them take (0 4KiB, 1 2MiB, 2 1GiB).
bool vtd_probe(const struct iommu_tables *tables, size_t *leaf_level);

// Whether every unit can remap interrupts in x2APIC format.
bool vtd_remap_x2apic_capable(void);
// Takes the units over from the firmware, and has them remap interrupts if
// they all can, in x2APIC format where the local APICs are in x2APIC mode and
// they all can: before any interrupt source is programmed.
void vtd_start(bool x2apic);

// Has every unit translate all DMA through the 4-level second-stage table at
// pml4.
void vtd_enable(uintptr_t pml4);

// Interrupt remapping, if on: count consecutive entries of the table, or -1;
// an entry pointed at a vector on the processor with an APIC ID; the message
// and the I/O APIC entry that name an entry.
bool vtd_remap_on(void);
int vtd_remap_alloc(size_t count);
void vtd_remap_free(uint16_t index, size_t count);
void vtd_remap_set(uint16_t index, uint32_t apic_id, uint8_t vector, bool level);
// Whether every unit posts interrupts too, and an entry that posts a vector to
// the 64-byte posted-interrupt descriptor at descriptor.
bool vtd_posting(void);
void vtd_remap_post(uint16_t index, uint8_t vector, uintptr_t descriptor);
// Whether the units remap in x2APIC format, reading the descriptors'
// destinations in it.
bool vtd_remap_x2apic_on(void);
void vtd_remap_message(uint16_t index, uint32_t *address, uint32_t *data);
uint64_t vtd_remap_rte(uint16_t index, uint8_t vector, bool level, bool active_low);

// Makes the units see changes to the tables made while they translate.
void vtd_flush(void);

// Logs at most limit of the faults that the units recorded, clears them all,
// and returns how many it left out.
size_t vtd_report_faults(size_t limit);

#endif
