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

#ifndef IOMMU__REMAP_H
#define IOMMU__REMAP_H

#include <stdint.h>
#include <stdbool.h>

// Interrupt remapping by the IOMMUs, where they all can: interrupt messages
// then name entries of a table of the hypervisor's, and compatibility format
// ones are blocked, whatever device sends them.
bool remap_on(void);
// count consecutive entries for the interrupts of a requester ID, or -1.
int remap_alloc(uint16_t requester, unsigned count);
void remap_free(uint16_t requester, int index, unsigned count);
// Points an entry at a vector on the processor with an APIC ID.
void remap_set(uint16_t requester, int index, uint32_t apic_id, uint8_t vector, bool level);
// The message, and the I/O APIC entry, that name an entry.
void remap_message(uint16_t requester, int index, uint32_t *address, uint32_t *data);
uint64_t remap_rte(uint16_t requester, int index, uint8_t vector, bool level, bool active_low);

#endif
