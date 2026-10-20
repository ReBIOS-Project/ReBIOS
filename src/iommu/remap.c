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

#include <stdint.h>
#include <stdbool.h>
#include <iommu/remap.h>
#include <iommu/vtd.h>

// VT-d's one table serves every requester.
bool remap_on(void) {
    return vtd_remap_on();
}

int remap_alloc(uint16_t requester, unsigned count) {
    (void)requester;
    return vtd_remap_alloc(count);
}

void remap_free(uint16_t requester, int index, unsigned count) {
    (void)requester;
    vtd_remap_free(index, count);
}

void remap_set(uint16_t requester, int index, uint32_t apic_id, uint8_t vector, bool level) {
    (void)requester;
    vtd_remap_set(index, apic_id, vector, level);
}

void remap_message(uint16_t requester, int index, uint32_t *address, uint32_t *data) {
    (void)requester;
    vtd_remap_message(index, address, data);
}

uint64_t remap_rte(uint16_t requester, int index, uint8_t vector, bool level, bool active_low) {
    (void)requester;
    return vtd_remap_rte(index, vector, level, active_low);
}
