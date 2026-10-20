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


#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <iommu/amdvi.h>
#include <iommu/iommu_tables.h>
#include <iommu/remap.h>
#include <iommu/vtd.h>

// VT-d's one table serves every requester, AMD-Vi has one per device.
static bool amd(void) {
    return iommu_tables_parse()->kind == IOMMU_KIND_AMD;
}

void remap_init(void) {
    if (amd()) {
        amdvi_remap_start();
    }
}

bool remap_on(void) {
    return amd() ? amdvi_remap_on() : vtd_remap_on();
}

int32_t remap_requester(uint8_t type, uint8_t id) {
    const struct iommu_special_device *d = iommu_special_for(type, id);
    return d != NULL ? d->rid : -1;
}

int remap_alloc(int32_t requester, unsigned count) {
    return amd() ? amdvi_remap_alloc(requester, count) : vtd_remap_alloc(count);
}

void remap_free(int32_t requester, int index, unsigned count) {
    if (amd()) {
        amdvi_remap_free(requester, index, count);
    } else {
        vtd_remap_free(index, count);
    }
}

void remap_set(int32_t requester, int index, uint32_t apic_id, uint8_t vector, bool level) {
    if (amd()) {
        amdvi_remap_set(requester, index, apic_id, vector);
    } else {
        vtd_remap_set(index, apic_id, vector, level);
    }
}

void remap_message(int32_t requester, int index, uint32_t *address, uint32_t *data) {
    (void)requester;
    if (amd()) {
        amdvi_remap_message(index, address, data);
    } else {
        vtd_remap_message(index, address, data);
    }
}

uint64_t remap_rte(int32_t requester, int index, uint8_t vector, bool level, bool active_low) {
    (void)requester;
    return amd() ? amdvi_remap_rte(index, level, active_low)
                 : vtd_remap_rte(index, vector, level, active_low);
}
