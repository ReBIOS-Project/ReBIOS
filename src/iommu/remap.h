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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Interrupt remapping by the IOMMUs, where they all can: interrupt messages
// then name entries of tables of the hypervisor's, and other ones are blocked,
// whatever device sends them. AMD-Vi's starts once the I/O APICs are known.
void remap_init(void);
bool remap_on(void);
// The requester ID that an I/O APIC's (by its APIC ID) or an HPET's (by its
// number) messages carry, as the firmware's tables give it, or -1.
int32_t remap_requester(uint8_t type, uint8_t id);
// count consecutive entries for the interrupts of a requester ID, or -1.
int remap_alloc(int32_t requester, size_t count);
void remap_free(int32_t requester, int index, size_t count);
// Points an entry at a vector on the processor with an APIC ID.
void remap_set(
    int32_t requester, int index, uint32_t apic_id, uint8_t vector, bool level
);
// The message, and the I/O APIC entry, that name an entry.
void remap_message(
    int32_t requester, int index, uint32_t *address, uint32_t *data
);
uint64_t remap_rte(
    int32_t requester, int index, uint8_t vector, bool level, bool active_low
);

#endif
