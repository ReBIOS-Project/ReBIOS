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
// whatever device sends them. AMD-Vi's starts once the I/O APICs are known, in
// the guest virtual APIC's format where AVIC may run the guest's local APICs.
void remap_init(bool guest_vapic);
bool remap_on(void);
// The requester ID that an I/O APIC's (by its APIC ID) or an HPET's (by its
// number) messages carry, as the firmware's tables give it, or -1.
int32_t remap_requester(uint8_t type, uint8_t id);
// count consecutive entries for the interrupts of a requester ID, or -1.
int remap_alloc(int32_t requester, size_t count);
void remap_free(int32_t requester, uint16_t index, size_t count);
// Points an entry at a vector on the processor with an APIC ID.
void remap_set(int32_t requester, uint16_t index, uint32_t apic_id, uint8_t vector, bool level);
// Whether the IOMMUs can post interrupts to a vCPU's posted-interrupt
// descriptor instead, and an entry that has them post a vector there.
bool remap_posting(void);
void remap_post(int32_t requester, uint16_t index, uint8_t vector, uintptr_t descriptor);
// Whether the IOMMUs can also remap in x2APIC format, reading the descriptors'
// destinations in it, whether they do now, and switching them to it or back.
bool remap_x2apic_capable(void);
bool remap_x2apic_on(void);
void remap_x2apic(bool on);
// The message, and the I/O APIC entry, that name an entry.
void remap_message(int32_t requester, uint16_t index, uint32_t *address, uint32_t *data);
uint64_t remap_rte(int32_t requester, uint16_t index, uint8_t vector, bool level, bool active_low);

#endif
