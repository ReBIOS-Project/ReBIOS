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
#include <stddef.h>
#include <stdbool.h>
#include <memory.h>
#include <csm/pir.h>
#include <hw/acpi_irq.h>
#include <lib/log.h>
#include <virt/irq.h>

// The structures of the Microsoft PCI IRQ Routing Table Specification 1.0.

#define PIR_VERSION 0x0100
#define PIR_PINS 4
#define ISA_IRQS 16

struct pir_header {
    char signature[4];
    uint16_t version;
    uint16_t table_size;
    uint8_t router_bus;
    uint8_t router_devfn;
    uint16_t exclusive_irqs;
    uint32_t compatible_router;
    uint32_t miniport_data;
    uint8_t reserved[11];
    uint8_t checksum;
} __attribute__((packed));

struct pir_link {
    uint8_t link;
    uint16_t bitmap;
} __attribute__((packed));

struct pir_slot {
    uint8_t bus;
    uint8_t devfn;
    struct pir_link pins[PIR_PINS];
    uint8_t slot;
    uint8_t reserved;
} __attribute__((packed));

_Static_assert(offsetof(struct pir_header, exclusive_irqs) == 10, "$PIR header layout");
_Static_assert(offsetof(struct pir_header, checksum) == 31, "$PIR header layout");
_Static_assert(sizeof(struct pir_header) == 32, "$PIR header layout");
_Static_assert(offsetof(struct pir_slot, slot) == 14, "$PIR slot entry layout");
_Static_assert(sizeof(struct pir_slot) == 16, "$PIR slot entry layout");

// Fills the slot entry of the device whose routes start at routes[*index],
// and moves *index past them, as the routes of a device follow each other.
// Returns whether any of its pins is on an ISA IRQ.
static bool slot_fill(const struct acpi_irq_pci_route *routes, uint32_t count, uint32_t *index,
                      struct pir_slot *slot) {
    const struct pci_addr first = routes[*index].addr;
    // Slot number 0, built in, as which devices sit in slots is not known.
    *slot = (struct pir_slot){.bus = first.bus, .devfn = (uint8_t)(first.device << 3)};
    bool any = false;
    for (; *index < count; (*index)++) {
        const struct acpi_irq_pci_route *route = &routes[*index];
        if (route->addr.segment != first.segment || route->addr.bus != first.bus
            || route->addr.device != first.device) {
            break;
        }
        if (route->pin < 1 || route->pin > PIR_PINS) {
            continue;
        }
        int irq = irq_isa_irq(route->gsi);
        struct pir_link *link = &slot->pins[route->pin - 1];
        if (irq < 0 || irq >= ISA_IRQS || link->link != 0) {
            continue;
        }
        link->link = (uint8_t)(irq + 1);
        link->bitmap = (uint16_t)(1u << irq);
        any = true;
    }
    return any;
}

uint32_t pir_build(void *buf, uint32_t size) {
    uint32_t count;
    const struct acpi_irq_pci_route *routes = acpi_irq_pci_routes(&count);

    uint32_t slots = 0;
    for (uint32_t i = 0; i < count;) {
        struct pir_slot slot;
        if (routes[i].addr.segment != 0) {
            i++;
        } else if (slot_fill(routes, count, &i, &slot)) {
            slots++;
        }
    }
    if (slots == 0) {
        printf("No PCI interrupts on ISA IRQs, so no PCI IRQ routing table\n");
        return 0;
    }
    uint32_t length = sizeof(struct pir_header) + slots * sizeof(struct pir_slot);
    if (length > size || length > UINT16_MAX) {
        printf("The PCI IRQ routing table takes %u bytes, more than the %u it has\n", length,
               size);
        return 0;
    }

    uint8_t *bytes = buf;
    uint32_t offset = sizeof(struct pir_header);
    uint16_t exclusive = 0;
    for (uint32_t i = 0; i < count;) {
        struct pir_slot slot;
        if (routes[i].addr.segment != 0) {
            i++;
        } else if (slot_fill(routes, count, &i, &slot)) {
            for (unsigned pin = 0; pin < PIR_PINS; pin++) {
                exclusive |= slot.pins[pin].bitmap;
            }
            memcpy(bytes + offset, &slot, sizeof(slot));
            offset += sizeof(slot);
        }
    }

    // No interrupt router, as the guest cannot change where interrupts go.
    struct pir_header header = {
        .version = PIR_VERSION,
        .table_size = (uint16_t)length,
        .exclusive_irqs = exclusive,
    };
    memcpy(header.signature, "$PIR", sizeof(header.signature));
    memcpy(bytes, &header, sizeof(header));
    uint8_t sum = 0;
    for (uint32_t i = 0; i < length; i++) {
        sum = (uint8_t)(sum + bytes[i]);
    }
    bytes[offsetof(struct pir_header, checksum)] = (uint8_t)-sum;

    printf("PCI IRQ routing table: %u slots, PCI IRQs %#x\n", slots, exclusive);
    return length;
}
