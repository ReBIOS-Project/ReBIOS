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

#ifndef VIRT__IRQ_H
#define VIRT__IRQ_H

#include <stdint.h>
#include <stdbool.h>
#include <virt/vcpu.h>

// The host's interrupts reach the guest through its virtual 8259s and I/O
// APICs: puts the host in APIC mode for good, masks its 8259s, and routes the
// ISA IRQs as the MADT has them. Needs vioapic_init().
void irq_init(void);

// Whether the hypervisor takes the host's interrupts on a vector, and one it
// took, which it EOIs.
bool irq_host_vector(uint8_t vector);
void irq_host_interrupt(uint8_t vector);

// A GSI PCI devices interrupt on, as the ACPI namespace has it in APIC mode.
struct irq_pci_line {
    uint32_t gsi;
    bool level;
    bool active_low;
};

// Has the guest's 8259s see each GSI PCI devices use on an ISA IRQ: one of
// those neither ISA devices (isa_claimed, bit n for IRQ n), the SCI, nor the
// hypervisor's own devices use, in turn, so that as few GSIs share one as can
// be. Before the guest runs.
void irq_route_pci(const struct irq_pci_line *lines, uint32_t count, uint16_t isa_claimed);

// The ISA IRQ the guest's 8259s see a GSI's interrupts on, or -1.
int irq_isa_irq(uint32_t gsi);
// The GSI of an ISA IRQ, as the MADT overrides it, or UINT32_MAX. Before
// irq_route_pci() puts PCI's on ISA IRQs.
uint32_t irq_isa_gsi(unsigned irq);

// After the guest changed an I/O APIC entry.
void irq_gsi_changed(uint32_t gsi);

// Whether the guest's 8259 interrupts the vCPU, through LINT0 or an I/O APIC
// ExtINT entry, and the interrupt acknowledge that answers it.
bool irq_extint_pending(struct vcpu *v);
uint8_t irq_extint_acknowledge(void);

// A guest vector the processor took: whether one of the guest's level-
// triggered I/O APIC entries sent it here, which then waits for the guest's
// EOI of the vector, masked at the host, its Remote IRR set.
bool irq_mirror_taken(uint8_t vector);
void irq_mirror_eoi(uint8_t vector);
bool irq_remote_irr(uint32_t gsi);

// Takes the host's line of a GSI for the hypervisor: its interrupts call
// handler on the boot processor, with the line masked until handler returns.
void irq_take_host_line(uint32_t gsi, void (*handler)(void *ctx), void *ctx);
// An interrupt on VLAPIC_DEVICE_VECTOR.
void irq_device_interrupt(void);

// The level of a line the hypervisor drives for the guest on a GSI, which
// reaches the guest's I/O APIC entry or 8259 input for the GSI as a device's
// level-triggered line would.
void irq_virtual_line(uint32_t gsi, bool asserted);

#endif
