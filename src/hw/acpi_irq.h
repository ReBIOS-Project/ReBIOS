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
// You should have received a copy of the GNU General Public License along with
// ReBIOS. If not, see <https://www.gnu.org/licenses/>.

#ifndef HW__ACPI_IRQ_H
#define HW__ACPI_IRQ_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <uacpi/namespace.h>
#include <hw/pci_cfg.h>

struct acpi_irq_pci_route {
    struct pci_cfg_addr addr;
    // The function's Interrupt Pin register: 1 for INTA# to 4 for INTD#.
    uint8_t pin;
    uint32_t gsi;
    bool level;
    bool active_low;
};

// The GSIs the PCI functions pci_enum_scan() finds interrupt on in APIC mode,
// as the ACPI namespace has them: one route per function with an interrupt pin
// the namespace routes, in the topology's order. Needs irq_init(). Looks them
// up once; later calls return the same routes.
const struct acpi_irq_pci_route *acpi_irq_pci_routes(size_t *count);

// The ISA IRQs that the devices the ACPI namespace describes as present and
// enabled use, by their current resources: bit n for IRQ n.
uint16_t acpi_irq_isa_claimed(void);

// An interrupt that a device's current resources give it, and how the
// descriptor says it is sensed.
struct acpi_irq_device_line {
    uacpi_namespace_node *node;
    uint32_t line;
    // Whether the line is a GSI, as an Extended Interrupt descriptor gives
    // one, or else an ISA IRQ, as an IRQ descriptor does.
    bool gsi;
    bool level;
    bool active_low;
};

// The interrupts of the devices that the ACPI namespace describes as present
// and enabled, by their current resources, but for those with PNP0xxx IDs, the
// legacy PC's devices and interrupt links, HID over I2C's PNP0C50 aside. Looks
// them up once; later calls return the same.
const struct acpi_irq_device_line *acpi_irq_device_lines(size_t *count);

// The SCI's ISA IRQ, or GSI where it is 16 or more (the FADT's SCI_INT), and
// whether it is active low and level-triggered, as the MADT's override of it
// has it. False on a machine without one.
bool acpi_irq_sci(uint32_t *irq, bool *active_low, bool *level);

#endif
