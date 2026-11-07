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
#include <uacpi/acpi.h>
#include <uacpi/namespace.h>
#include <uacpi/platform/types.h>
#include <uacpi/resources.h>
#include <uacpi/status.h>
#include <uacpi/types.h>
#include <uacpi/utilities.h>
#include <virt/isa_ports.h>
#include <core.h>
#include <handoff.h>
#include <hw/acpi.h>
#include <lib/log.h>
#include <virt/io.h>

// The standard PC serial and parallel ports' IDs that Linux 7.3's 8250_pnp and
// parport_pc drivers take.
static const char *const port_ids[] = { "PNP0500", "PNP0501", "PNP0400", "PNP0401", NULL };

#define UART_PORTS 8
#define MAX_RANGES 4

struct device_ports {
    uint16_t base[MAX_RANGES];
    uint16_t count[MAX_RANGES];
    size_t ranges;
};

static uacpi_iteration_decision collect_range(void *user, uacpi_resource *resource) {
    struct device_ports *d = user;
    uint16_t base;
    uint16_t count;
    switch (resource->type) {
        case UACPI_RESOURCE_TYPE_IO: {
            base = resource->io.minimum;
            count = resource->io.length;
            break;
        }
        case UACPI_RESOURCE_TYPE_FIXED_IO: {
            base = resource->fixed_io.address;
            count = resource->fixed_io.length;
            break;
        }
        default: {
            return UACPI_ITERATION_DECISION_CONTINUE;
        }
    }
    if (count != 0 && d->ranges < MAX_RANGES) {
        d->base[d->ranges] = base;
        d->count[d->ranges] = count;
        d->ranges++;
    }
    return UACPI_ITERATION_DECISION_CONTINUE;
}

static uacpi_iteration_decision give_device(
    void *user, uacpi_namespace_node *node, uacpi_u32 depth
) {
    (void)user;
    (void)depth;
    uacpi_u32 sta;
    if (uacpi_eval_sta(node, &sta) != UACPI_STATUS_OK
     || (sta & ACPI_STA_RESULT_DEVICE_ENABLED) == 0) {
        return UACPI_ITERATION_DECISION_CONTINUE;
    }
    struct device_ports d = { .ranges = 0 };
    uacpi_for_each_device_resource(node, "_CRS", collect_range, &d);
    const struct handoff_config *config = &core_handoff->config;
    uint16_t log = config->serial != 0 ? config->serial_port : 0;
    for (size_t i = 0; i < d.ranges; i++) {
        if (log >= d.base[i] && log - d.base[i] < d.count[i]) {
            return UACPI_ITERATION_DECISION_CONTINUE;
        }
    }
    for (size_t i = 0; i < d.ranges; i++) {
        io_pass(d.base[i], d.count[i]);
        printf(
            "The guest gets %.4s's ports %#x-%#x\n", uacpi_namespace_node_name(node).text,
            d.base[i], d.base[i] + d.count[i] - 1
        );
    }
    return UACPI_ITERATION_DECISION_CONTINUE;
}

void isa_ports_init(void) {
    const struct handoff_config *config = &core_handoff->config;
    if (config->serial != 0 && config->serial_port != 0) {
        io_block(config->serial_port, UART_PORTS);
    }
    if (!acpi_init()) {
        return;
    }
    acpi_lock();
    uacpi_find_devices_at(uacpi_namespace_root(), port_ids, give_device, NULL);
    acpi_unlock();
}
