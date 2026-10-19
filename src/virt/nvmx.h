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

#ifndef VIRT__NVMX_H
#define VIRT__NVMX_H

#include <stdint.h>
#include <stdbool.h>

struct vcpu;

// Whether the guest gets VMX of its own.
extern bool nvmx_enabled;

void nvmx_init(void);
void nvmx_vcpu_init(struct vcpu *v);

// The guest's VMX instructions, by the exits they cause.
void nvmx_instruction(struct vcpu *v, uint32_t reason);
// The guest's IA32_FEATURE_CONTROL and VMX capability MSRs.
bool nvmx_rdmsr(struct vcpu *v, uint32_t msr, uint64_t *value, bool *fault);
bool nvmx_wrmsr(struct vcpu *v, uint32_t msr, uint64_t value, bool *fault);
// Whether the guest may write a value to CR0 or CR4, as VMX operation and the
// VMX it gets have it.
bool nvmx_cr_allowed(struct vcpu *v, unsigned cr, uint64_t value);

// Whether the vCPU runs the guest's own guest, VMCS02 being its current VMCS.
bool nvmx_in_l2(struct vcpu *v);
// An exit of the guest's guest: whether it went to the guest, else the
// hypervisor's to handle as its own.
bool nvmx_l2_exit(struct vcpu *v, uint32_t reason, uint64_t qual);
// A failed entry of the guest's guest: whether it was one, now the guest's.
bool nvmx_entry_failed(struct vcpu *v, uint32_t error);
// Before the vCPU's entry: whether it enters the guest's guest, the guest's
// own events seen to as exits.
bool nvmx_before_entry(struct vcpu *v);
// Whether INIT waits, as VMX operation blocks it.
bool nvmx_holds_init(struct vcpu *v);
// Before the vCPU's entry: in the activity states the guest's guest waits in
// here, until something ends the wait.
void nvmx_wait(struct vcpu *v);
// A start-up IPI for the vCPU, from any processor: whether its guest's guest
// waited for one, for the vCPU to wake for.
bool nvmx_sipi(struct vcpu *v, uint8_t vector);
// A fixed, edge-triggered interrupt for the guest, from any processor:
// whether it is the notification of its guest's posted interrupts instead,
// for the vCPU's processor to process with VLAPIC_NESTED_POSTED_VECTOR.
bool nvmx_posted_notification(struct vcpu *v, uint8_t vector);

// Whether the vCPU runs the guest's guest through the guest's EPT, which then
// translates what the vCPU's accesses take as guest-physical addresses.
bool nvmx_ept_active(struct vcpu *v);
// An address of the guest's guest through the guest's EPT, for an access the
// way an EPT violation's qualification gives one; false where that is an exit
// instead, which waits for the next entry.
bool nvmx_ept_translate(struct vcpu *v, uint64_t gpa, uint64_t access, uint64_t linear,
                        uint64_t *out);
// Whether such an exit waits.
bool nvmx_exit_pending(struct vcpu *v);
// An exception that what the hypervisor carries out for the guest's guest
// raises: whether the guest's exception bitmap makes it an exit instead, with
// the qualification given.
bool nvmx_exception(struct vcpu *v, uint8_t vector, bool has_error, uint32_t error, uint64_t qual);
// Whether the guest's guest is to make an MTF exit after each instruction, and
// such an exit after one the hypervisor carried out, with a single-step trap
// pending: whether there is one.
bool nvmx_monitor_trap_flag(struct vcpu *v);
bool nvmx_monitor_trap(struct vcpu *v, bool single_step);
// Right before an entry: VMCS02's VMX-preemption timer, from what is left of
// the guest's, so that time the hypervisor takes counts as the guest's guest's,
// as it would without the hypervisor; off while the hypervisor steps.
void nvmx_arm_timer(struct vcpu *v);
// The EPTP of VMCS02's shadow of the guest's EPT, or 0 where the vCPU runs on
// the hypervisor's EPT.
uint64_t nvmx_ept_pointer(struct vcpu *v);
// The hypervisor's EPT changed, and with it what the shadows hold.
void nvmx_gpt_changed(struct vcpu *v);

#endif
