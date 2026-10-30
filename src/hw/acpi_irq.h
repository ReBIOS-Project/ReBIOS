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

#ifndef HW__ACPI_IRQ_H
#define HW__ACPI_IRQ_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <hw/pci_cfg.h>

struct acpi_irq_pci_route {
    struct pci_addr addr;
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

#endif
