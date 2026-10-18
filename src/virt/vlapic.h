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

#ifndef VIRT__VLAPIC_H
#define VIRT__VLAPIC_H

#include <stdint.h>
#include <stdbool.h>
#include <virt/vcpu.h>

// Vectors the hypervisor takes its own interrupts on at the processor's
// local APIC, which guests use for none of their MSIs and I/O APIC entries.
#define VLAPIC_POSTED_VECTOR 0xfb
#define VLAPIC_DEVICE_VECTOR 0xfc
#define VLAPIC_KICK_VECTOR 0xfd
#define VLAPIC_TIMER_VECTOR 0xfe
#define VLAPIC_SPURIOUS_VECTOR 0xff

// Each vCPU's local APIC is emulated, on the processor's, which the
// hypervisor keeps for itself: x2APIC mode where the processor has it, xAPIC
// mode always. Needs virt_init().
void vlapic_init(void);

// A vCPU's local APIC: the boot processor's as a legacy BIOS hands it over,
// set up as the processor's local APIC is when this is called, the others' as
// after INIT.
void vlapic_create(struct vcpu *v);
// The state INIT leaves the local APIC in.
void vlapic_reset(struct vcpu *v);

// The local APIC's MSRs: return false for others, and set *fault where the
// access raises #GP.
bool vlapic_rdmsr(struct vcpu *v, uint32_t msr, uint64_t *value, bool *fault);
bool vlapic_wrmsr(struct vcpu *v, uint32_t msr, uint64_t value, bool *fault);

// Whether the CPUID APIC flag shows.
bool vlapic_enabled(struct vcpu *v);
// The guest-physical page of the vCPU's local APIC in xAPIC mode.
uint64_t vlapic_page(struct vcpu *v);

// A fixed interrupt for the vCPU, from any processor.
void vlapic_accept(struct vcpu *v, uint8_t vector, bool level);
// An interrupt the hypervisor sends as an I/O APIC entry would, to the vCPUs
// an 8-bit destination names, or to one of them for lowest priority.
void vlapic_deliver_external(uint32_t dest, bool logical, uint8_t vector, bool level,
                             bool lowest);
// The processor's timer, which runs the vCPU's local APIC timer, fired.
void vlapic_timer_fired(struct vcpu *v);

// The vector of the interrupt the vCPU takes next, or -1, and taking it.
int vlapic_pending(struct vcpu *v);
void vlapic_take(struct vcpu *v, uint8_t vector);
// The priority class of a requested interrupt that only the TPR holds back,
// or -1.
int vlapic_tpr_held(struct vcpu *v);
// Whether the 8259's interrupts reach the vCPU through LINT0.
bool vlapic_extint_open(struct vcpu *v);

// Whether the processor delivers the vCPU's interrupts from its local APIC
// itself, which then only need putting in its registers.
bool vlapic_virtual_delivery(struct vcpu *v);
// The guest ended a level-triggered interrupt the processor delivered.
void vlapic_eoi_exit(struct vcpu *v, uint8_t vector);
// The guest wrote a register with side effects at a page offset, with the
// value already in place.
void vlapic_apic_write(struct vcpu *v, uint32_t offset);

// The TPR, which the guest also changes through CR8 without exits, kept in
// step around entries.
void vlapic_before_entry(struct vcpu *v);
void vlapic_after_exit(struct vcpu *v);

#endif
