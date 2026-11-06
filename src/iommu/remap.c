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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <iommu/remap.h>
#include <iommu/amdvi.h>
#include <iommu/iommu_tables.h>
#include <iommu/vtd.h>

// VT-d's one table serves every requester, AMD-Vi has one per device.
static bool amd(void) {
    return iommu_tables_parse()->kind == IOMMU_KIND_AMD;
}

void remap_init(bool guest_vapic, uint8_t vector) {
    if (amd()) {
        amdvi_remap_start(guest_vapic, vector);
    }
}

bool remap_on(void) {
    return amd() ? amdvi_remap_on() : vtd_remap_on();
}

int32_t remap_requester(uint8_t type, uint8_t id) {
    const struct iommu_special_device *d = iommu_special_for(type, id);
    return d != NULL ? d->rid : -1;
}

int remap_alloc(int32_t requester, size_t count) {
    return amd() ? amdvi_remap_alloc(requester, count) : vtd_remap_alloc(count);
}

void remap_free(int32_t requester, uint16_t index, size_t count) {
    if (amd()) {
        amdvi_remap_free(requester, index, count);
    } else {
        vtd_remap_free(index, count);
    }
}

void remap_set(int32_t requester, uint16_t index, uint32_t apic_id, uint8_t vector, bool level) {
    if (amd()) {
        amdvi_remap_set(requester, index, apic_id, vector);
    } else {
        vtd_remap_set(index, apic_id, vector, level);
    }
}

bool remap_posting(void) {
    return amd() ? amdvi_posting() : vtd_posting();
}

bool remap_post(
    int32_t requester, uint16_t index, uint8_t vector, const struct remap_target *target
) {
    if (amd()) {
        return amdvi_remap_post(requester, index, vector, target);
    }
    vtd_remap_post(index, vector, target->descriptor);
    return true;
}

void remap_running(uint32_t tag, bool running) {
    if (amd()) {
        amdvi_running(tag, running);
    }
}

void remap_poll(void (*woken)(uint32_t tag)) {
    if (amd()) {
        amdvi_poll(woken);
    }
}

bool remap_x2apic_capable(void) {
    return !amd() && vtd_remap_x2apic_capable();
}

bool remap_x2apic_on(void) {
    return !amd() && vtd_remap_x2apic_on();
}

void remap_x2apic(bool on) {
    if (!amd()) {
        vtd_remap_x2apic(on);
    }
}

void remap_message(int32_t requester, uint16_t index, uint32_t *address, uint32_t *data) {
    (void)requester;
    if (amd()) {
        amdvi_remap_message(index, address, data);
    } else {
        vtd_remap_message(index, address, data);
    }
}

uint64_t remap_rte(
    int32_t requester, uint16_t index, uint8_t vector, bool level, bool active_low
) {
    (void)requester;
    return amd()
         ? amdvi_remap_rte(index, level, active_low)
         : vtd_remap_rte(index, vector, level, active_low);
}
