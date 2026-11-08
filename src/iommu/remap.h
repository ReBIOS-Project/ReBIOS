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
// the guest virtual APIC's format where AVIC may run the guest's local APICs,
// the units then interrupting the calling processor on vector for what they
// log.
void remap_init(bool guest_vapic, uint8_t vector);
bool remap_on(void);
// The requester ID that an I/O APIC's (by its APIC ID) or an HPET's (by its
// number) messages carry, as the firmware's tables give it, or -1.
int32_t remap_requester(uint8_t type, uint8_t id);
// count consecutive entries for the interrupts of a requester ID, or -1.
int remap_alloc(int32_t requester, size_t count);
void remap_free(int32_t requester, uint16_t index, size_t count);
// Points an entry at a vector on the processor with an APIC ID.
void remap_set(int32_t requester, uint16_t index, uint32_t apic_id, uint8_t vector, bool level);
// Where the IOMMUs can post a vCPU's interrupts: VT-d to its posted-interrupt
// descriptor, AMD-Vi to its virtual APIC backing page, ringing the doorbell of
// the processor with the APIC ID while the vCPU runs, and logging the tag
// while it does not.
struct remap_target {
    uintptr_t descriptor;
    uint32_t tag;
    uint32_t apic_id;
};

// Whether the IOMMUs can post interrupts to vCPUs instead, and an entry that
// has them post a vector to a target, false where it cannot.
bool remap_posting(void);
bool remap_post(
    int32_t requester, uint16_t index, uint8_t vector, const struct remap_target *target
);
// Whether the vCPU a tag names runs, as AMD-Vi's doorbells need it to.
void remap_running(uint32_t tag, bool running);
// Calls woken with the tag of each vCPU the IOMMUs logged an interrupt for
// while it did not run, and with every tag where a log lost some.
void remap_poll(void (*woken)(uint32_t tag));
// Whether the IOMMUs remap to x2APIC destinations, VT-d also reading the
// descriptors' destinations in x2APIC format.
bool remap_x2apic_on(void);
// The message, and the I/O APIC entry, that name an entry.
void remap_message(int32_t requester, uint16_t index, uint32_t *address, uint32_t *data);
uint64_t remap_rte(int32_t requester, uint16_t index, uint8_t vector, bool level, bool active_low);

#endif
