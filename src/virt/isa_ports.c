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
#include <hw/isa.h>
#include <hw/pci_cfg.h>
#include <hw/pci_enum.h>
#include <hw/pci_regs.h>
#include <lib/log.h>
#include <virt/io.h>
#include <virt/irq.h>
#include <virt/vcpu.h>

// The standard PC serial and parallel ports' IDs that Linux 7.3's 8250_pnp and
// parport_pc drivers take.
const char *const isa_ports_ids[] = { "PNP0500", "PNP0501", "PNP0400", "PNP0401", NULL };

#define UART_PORTS 8
#define MAX_DEVICES 8
#define MAX_RANGES 4
#define MAX_LINES 4

// An IDE controller's channels in compatibility mode, at the PC/AT's ports and
// on its IRQs (ICH10 datasheet 319973-003, 14.1.6).
#define IDE_PRIMARY_NATIVE 0x01
#define IDE_SECONDARY_NATIVE 0x04
#define IDE_PRIMARY_BASE 0x1f0
#define IDE_PRIMARY_CONTROL 0x3f6
#define IDE_PRIMARY_IRQ 14
#define IDE_SECONDARY_BASE 0x170
#define IDE_SECONDARY_CONTROL 0x376
#define IDE_SECONDARY_IRQ 15
#define IDE_CHANNEL_PORTS 8

// An Intel IDE function's timing registers, one per channel, whose IDE Decode
// Enable bit has the function claim that channel's ports (PIIX4 datasheet
// 290562-001, 5.1.10; ICH10 datasheet 319973-003, 14.1.22).
#define IDE_PRIMARY_TIMING 0x40
#define IDE_SECONDARY_TIMING 0x42
#define IDE_TIMING_DECODE 0x8000

// ACPI 6.6's resource descriptors that take I/O ports, by their first byte:
// the small I/O Port and Fixed Location I/O Port ones (6.4.2.5, 6.4.2.6), and
// the large Word, DWord, QWord, and Extended Address Space ones (6.4.3.5.1 to
// 6.4.3.5.4) of resource type 1, I/O; and the End Tag (6.4.2.9).
#define RES_FIXED_IO_MASK 0x3ff

struct device_ports {
    uint16_t base[MAX_RANGES];
    uint16_t count[MAX_RANGES];
    size_t ranges;
    // ISA IRQs, and GSIs where an extended descriptor gives them.
    uint32_t lines[MAX_LINES];
    bool line_is_gsi[MAX_LINES];
    size_t line_count;
};

// A serial or parallel port in the guest's namespace, which the guest's OS may
// turn on, off, or move through its _SRS and _DIS: what the guest has of it.
struct port_device {
    uacpi_namespace_node *node;
    struct device_ports given;
};

static struct port_device devices[MAX_DEVICES];
static size_t device_count;

static struct port_device *device_of(const void *node) {
    for (size_t i = 0; i < device_count; i++) {
        if (devices[i].node == node) {
            return &devices[i];
        }
    }
    return NULL;
}

bool isa_ports_shown(const void *node) {
    return device_of(node) != NULL;
}

static uint16_t log_port(void) {
    const struct handoff_config *config = &core_handoff->config;
    return config->serial != 0 ? config->serial_port : 0;
}

// Whether ports from base up to end, which it does not include, take one of
// the log UART's.
static bool takes_log(uint64_t base, uint64_t end) {
    uint16_t log = log_port();
    return log != 0 && base < (uint64_t)log + UART_PORTS && log < end;
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
        case UACPI_RESOURCE_TYPE_ADDRESS16: {
            if (resource->address16.common.type != UACPI_RANGE_IO) {
                return UACPI_ITERATION_DECISION_CONTINUE;
            }
            base = resource->address16.minimum;
            count = resource->address16.address_length;
            break;
        }
        case UACPI_RESOURCE_TYPE_ADDRESS32: {
            if (resource->address32.common.type != UACPI_RANGE_IO
             || resource->address32.minimum > UINT16_MAX
             || resource->address32.address_length > UINT16_MAX) {
                return UACPI_ITERATION_DECISION_CONTINUE;
            }
            base = (uint16_t)resource->address32.minimum;
            count = (uint16_t)resource->address32.address_length;
            break;
        }
        case UACPI_RESOURCE_TYPE_ADDRESS64: {
            if (resource->address64.common.type != UACPI_RANGE_IO
             || resource->address64.minimum > UINT16_MAX
             || resource->address64.address_length > UINT16_MAX) {
                return UACPI_ITERATION_DECISION_CONTINUE;
            }
            base = (uint16_t)resource->address64.minimum;
            count = (uint16_t)resource->address64.address_length;
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

// The resources the device has, as its _CRS says while it is enabled: false
// where they take the log UART's ports, which the guest never gets.
static bool current_ports(uacpi_namespace_node *node, struct device_ports *d, bool *enabled) {
    *d = (struct device_ports){ .ranges = 0, .line_count = 0 };
    uacpi_u32 sta;
    *enabled = uacpi_eval_sta(node, &sta) == UACPI_STATUS_OK
            && (sta & ACPI_STA_RESULT_DEVICE_ENABLED) != 0;
    if (!*enabled) {
        return true;
    }
    uacpi_for_each_device_resource(node, "_CRS", collect_range, d);
    for (size_t i = 0; i < d->ranges; i++) {
        if (takes_log(d->base[i], (uint64_t)d->base[i] + d->count[i])) {
            return false;
        }
    }
    return true;
}

static void give_lines(const struct device_ports *d) {
    for (size_t i = 0; i < d->line_count; i++) {
        if (d->line_is_gsi[i]) {
            irq_give_gsi(d->lines[i]);
        } else if (d->lines[i] < ISA_IRQS) {
            irq_give_isa_line(d->lines[i]);
        }
    }
}

static void log_ports(uacpi_namespace_node *node, const struct device_ports *d) {
    for (size_t i = 0; i < d->ranges; i++) {
        printf(
            "The guest gets %.4s's ports %#x-%#x\n", uacpi_namespace_node_name(node).text,
            d->base[i], d->base[i] + d->count[i] - 1
        );
    }
}

// Those the firmware enabled, and those it left for the OS to turn on, but for
// the one of ReBIOS's log.
static uacpi_iteration_decision find_device(
    void *user,
    uacpi_namespace_node *node,
    uacpi_u32 depth
) {
    (void)user;
    (void)depth;
    uacpi_u32 sta;
    uacpi_namespace_node *srs;
    if (uacpi_eval_sta(node, &sta) != UACPI_STATUS_OK
     || (sta & ACPI_STA_RESULT_DEVICE_PRESENT) == 0
     || ((sta & ACPI_STA_RESULT_DEVICE_ENABLED) == 0
      && uacpi_namespace_node_find(node, "_SRS", &srs) != UACPI_STATUS_OK)) {
        return UACPI_ITERATION_DECISION_CONTINUE;
    }
    struct device_ports d;
    bool enabled;
    if (!current_ports(node, &d, &enabled) || device_count == MAX_DEVICES) {
        return UACPI_ITERATION_DECISION_CONTINUE;
    }
    devices[device_count++] = (struct port_device){ .node = node, .given = d };
    for (size_t i = 0; i < d.ranges; i++) {
        io_pass(d.base[i], d.count[i]);
    }
    give_lines(&d);
    if (!enabled) {
        printf("The guest's OS may turn %.4s on\n", uacpi_namespace_node_name(node).text);
    }
    log_ports(node, &d);
    return UACPI_ITERATION_DECISION_CONTINUE;
}

static uint64_t field(const uint8_t *p, size_t width) {
    uint64_t value = 0;
    for (size_t i = 0; i < width; i++) {
        value |= (uint64_t)p[i] << (8 * i);
    }
    return value;
}

// The ports an I/O descriptor of an _SRS template sets, from its minimum, the
// base that firmware takes, up to end, whatever its maximum, which Linux 7.3
// gives as the last port: false for a descriptor of anything else.
static bool template_ports(const uint8_t *r, size_t size, uint64_t *base, uint64_t *end) {
    switch (r[0]) {
        case ACPI_TAG_IO: {
            *base = field(&r[2], 2);
            *end = *base + r[7];
            return true;
        }
        case ACPI_TAG_FIXED_IO: {
            *base = field(&r[1], 2) & RES_FIXED_IO_MASK;
            *end = *base + r[3];
            return true;
        }
        case ACPI_TAG_ADDRESS16:
        case ACPI_TAG_ADDRESS32:
        case ACPI_TAG_ADDRESS64:
        case ACPI_TAG_ADDRESS64_EXTENDED: {
            // Granularity, minimum, maximum, translation, and length follow
            // each other.
            size_t width = r[0] == ACPI_TAG_ADDRESS16 ? 2 : r[0] == ACPI_TAG_ADDRESS32 ? 4 : 8;
            size_t at = r[0] == ACPI_TAG_ADDRESS64_EXTENDED ? 8 : 6;
            if (r[3] != UACPI_RANGE_IO || at + 5 * width > size) {
                return false;
            }
            *base = field(&r[at + width], width);
            *end = *base + field(&r[at + 4 * width], width);
            return true;
        }
        default: {
            return false;
        }
    }
}

bool isa_ports_settable(const uint8_t *template, size_t length) {
    size_t at = 0;
    while (at < length && template[at] != ACPI_TAG_END) {
        const uint8_t *r = &template[at];
        size_t size;
        if ((r[0] & ACPI_LARGE_ITEM) == 0) {
            size = 1 + (size_t)(r[0] & ACPI_SMALL_ITEM_LENGTH_MASK);
        } else if (at + 3 <= length) {
            size = 3 + (size_t)field(&r[1], 2);
        } else {
            return false;
        }
        uint64_t base;
        uint64_t end;
        if (at + size > length || (template_ports(r, size, &base, &end) && takes_log(base, end))) {
            return false;
        }
        at += size;
    }
    return true;
}

void isa_ports_changed(const void *node) {
    struct port_device *dev = device_of(node);
    if (dev == NULL) {
        return;
    }
    struct device_ports now;
    bool enabled;
    if (!current_ports(dev->node, &now, &enabled)) {
        now = (struct device_ports){ .ranges = 0, .line_count = 0 };
    }
    vcpu_pause_others();
    for (size_t i = 0; i < dev->given.ranges; i++) {
        io_unpass(dev->given.base[i], dev->given.count[i]);
    }
    for (size_t i = 0; i < now.ranges; i++) {
        io_pass(now.base[i], now.count[i]);
    }
    vcpu_resume_others();
    give_lines(&now);
    dev->given = now;
    if (!enabled) {
        printf("The guest's OS turned %.4s off\n", uacpi_namespace_node_name(dev->node).text);
    }
    log_ports(dev->node, &now);
}

// A legacy BIOS turns the channel's decode on, which the firmware may leave
// off, and which drivers take for the channel's presence: Windows XP's, and
// Linux 7.3's (ata_piix.c, piix_pata_prereset()).
static void decode_ide_channel(const struct pci_enum_function *f, uint32_t timing) {
    if (f->vendor_id != PCI_VENDOR_ID_INTEL) {
        return;
    }
    uint32_t value = pci_cfg_read(f->addr, timing, 2);
    if ((value & IDE_TIMING_DECODE) != 0) {
        return;
    }
    pci_cfg_write(f->addr, timing, 2, value | IDE_TIMING_DECODE);
    printf(
        "IDE " PCI_CFG_ADDR_FMT ": decoding the %s channel, which the firmware left off\n",
        PCI_CFG_ADDR_ARGS(f->addr), timing == IDE_PRIMARY_TIMING ? "primary" : "secondary"
    );
}

static void give_ide_channels(void) {
    const struct pci_enum_topology *t = pci_enum_topology();
    for (size_t i = 0; t != NULL && i < t->function_count; i++) {
        const struct pci_enum_function *f = &t->functions[i];
        if (f->base_class != PCI_BASE_CLASS_STORAGE || f->subclass != PCI_SUBCLASS_STORAGE_IDE) {
            continue;
        }
        if ((f->prog_if & IDE_PRIMARY_NATIVE) == 0) {
            decode_ide_channel(f, IDE_PRIMARY_TIMING);
            io_pass(IDE_PRIMARY_BASE, IDE_CHANNEL_PORTS);
            io_pass(IDE_PRIMARY_CONTROL, 1);
            irq_give_isa_line(IDE_PRIMARY_IRQ);
        }
        if ((f->prog_if & IDE_SECONDARY_NATIVE) == 0) {
            decode_ide_channel(f, IDE_SECONDARY_TIMING);
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
    uacpi_find_devices_at(uacpi_namespace_root(), isa_ports_ids, find_device, NULL);
    acpi_unlock();
}
