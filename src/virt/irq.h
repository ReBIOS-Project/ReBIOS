// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of ReBIOS.
// Copyright (C) 2026 Mintsuki and contributors
//
// ReBIOS is free software: you can redistribute it and/or modify it under the
// terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version.
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

#endif
