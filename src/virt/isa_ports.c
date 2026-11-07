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
#include <hw/pci_enum.h>
#include <lib/log.h>
#include <virt/io.h>
#include <virt/irq.h>

// The standard PC serial and parallel ports' IDs that Linux 7.3's 8250_pnp and
// parport_pc drivers take.
static const char *const port_ids[] = { "PNP0500", "PNP0501", "PNP0400", "PNP0401", NULL };

#define UART_PORTS 8
#define MAX_GIVEN 8
#define MAX_RANGES 4
#define MAX_LINES 4
#define ISA_IRQS 16

// An IDE controller's channels in compatibility mode, at the PC/AT's ports and
// on its IRQs (ICH10 datasheet 319973-003, 14.1.6).
#define CLASS_STORAGE 0x01
#define SUBCLASS_IDE 0x01
#define IDE_PRIMARY_NATIVE 0x01
#define IDE_SECONDARY_NATIVE 0x04
#define IDE_PRIMARY_BASE 0x1f0
#define IDE_PRIMARY_CONTROL 0x3f6
#define IDE_PRIMARY_IRQ 14
#define IDE_SECONDARY_BASE 0x170
#define IDE_SECONDARY_CONTROL 0x376
#define IDE_SECONDARY_IRQ 15
#define IDE_CHANNEL_PORTS 8

struct device_ports {
    uint16_t base[MAX_RANGES];
    uint16_t count[MAX_RANGES];
    size_t ranges;
    // ISA IRQs, and GSIs where an extended descriptor gives them.
    uint32_t lines[MAX_LINES];
    bool line_is_gsi[MAX_LINES];
    size_t line_count;
};

static const void *given[MAX_GIVEN];
static size_t given_count;

bool isa_ports_given(const void *node) {
    for (size_t i = 0; i < given_count; i++) {
        if (given[i] == node) {
            return true;
        }
    }
    return false;
}

static void add_line(struct device_ports *d, uint32_t line, bool gsi) {
    if (d->line_count < MAX_LINES) {
        d->lines[d->line_count] = line;
        d->line_is_gsi[d->line_count] = gsi;
        d->line_count++;
    }
}

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
        case UACPI_RESOURCE_TYPE_IRQ: {
            for (size_t i = 0; i < resource->irq.num_irqs; i++) {
                add_line(d, resource->irq.irqs[i], false);
            }
            return UACPI_ITERATION_DECISION_CONTINUE;
        }
        case UACPI_RESOURCE_TYPE_EXTENDED_IRQ: {
            for (size_t i = 0; i < resource->extended_irq.num_irqs; i++) {
                add_line(d, resource->extended_irq.irqs[i], true);
            }
            return UACPI_ITERATION_DECISION_CONTINUE;
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
    struct device_ports d = { .ranges = 0, .line_count = 0 };
    uacpi_for_each_device_resource(node, "_CRS", collect_range, &d);
    const struct handoff_config *config = &core_handoff->config;
    uint16_t log = config->serial != 0 ? config->serial_port : 0;
    for (size_t i = 0; i < d.ranges; i++) {
        if (log >= d.base[i] && log - d.base[i] < d.count[i]) {
            return UACPI_ITERATION_DECISION_CONTINUE;
        }
    }
    if (given_count == MAX_GIVEN) {
        return UACPI_ITERATION_DECISION_CONTINUE;
    }
    given[given_count++] = node;
    for (size_t i = 0; i < d.ranges; i++) {
        io_pass(d.base[i], d.count[i]);
        printf(
            "The guest gets %.4s's ports %#x-%#x\n", uacpi_namespace_node_name(node).text,
            d.base[i], d.base[i] + d.count[i] - 1
        );
    }
    for (size_t i = 0; i < d.line_count; i++) {
        if (d.line_is_gsi[i]) {
            irq_give_gsi(d.lines[i]);
        } else if (d.lines[i] < ISA_IRQS) {
            irq_give_isa_line(d.lines[i]);
        }
    }
    return UACPI_ITERATION_DECISION_CONTINUE;
}

static void give_ide_channels(void) {
    const struct pci_topology *t = pci_enum_topology();
    for (size_t i = 0; t != NULL && i < t->function_count; i++) {
        const struct pci_function *f = &t->functions[i];
        if (f->base_class != CLASS_STORAGE || f->subclass != SUBCLASS_IDE) {
            continue;
        }
        if ((f->prog_if & IDE_PRIMARY_NATIVE) == 0) {
            io_pass(IDE_PRIMARY_BASE, IDE_CHANNEL_PORTS);
            io_pass(IDE_PRIMARY_CONTROL, 1);
            irq_give_isa_line(IDE_PRIMARY_IRQ);
        }
        if ((f->prog_if & IDE_SECONDARY_NATIVE) == 0) {
            io_pass(IDE_SECONDARY_BASE, IDE_CHANNEL_PORTS);
            io_pass(IDE_SECONDARY_CONTROL, 1);
            irq_give_isa_line(IDE_SECONDARY_IRQ);
        }
    }
}

void isa_ports_init(void) {
    give_ide_channels();
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
