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

#ifndef VIRT__NSVM_H
#define VIRT__NSVM_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/x86.h>

struct vcpu;

// Whether the guest gets SVM of its own.
extern bool nsvm_enabled;

void nsvm_init(void);
void nsvm_vcpu_init(struct vcpu *v);

// The guest's SVM instructions, by the exits they cause.
void nsvm_instruction(struct vcpu *v, uint64_t code);
// For INVLPGB (svm.c): whether the guest's guest has it enabled, as TlbiCtl
// has the guest do; how many ASIDs it may name, 1 without EFER.SVME; whether
// the guest intercepts its guest's illegal one, which then exits; and the
// processor's ASID it is for, and whether it is one ASID's rather than every
// one's.
bool nsvm_tlbi_enabled(struct vcpu *v);
uint32_t nsvm_invlpgb_asids(struct vcpu *v);
bool nsvm_invlpgb_illegal(struct vcpu *v);
bool nsvm_invlpgb_asid(struct vcpu *v, bool named, uint32_t asid, uint32_t *processor);
// The guest's VM_CR and VM_HSAVE_PA.
bool nsvm_rdmsr(struct vcpu *v, uint32_t msr, uint64_t *value, bool *fault);
bool nsvm_wrmsr(struct vcpu *v, uint32_t msr, uint64_t value, bool *fault);
// The EFER.SVME of whichever of the guest and its guest the vCPU runs, which
// the processor's EFER always has, and whether that may be set.
bool nsvm_svme(struct vcpu *v);
void nsvm_set_svme(struct vcpu *v, bool on);
bool nsvm_svme_allowed(struct vcpu *v);
// CPUID's leaf 8000_000Ah, as the guest gets SVM.
struct cpuid_regs nsvm_cpuid(void);

// Whether the vCPU runs the guest's own guest, VMCB02 being its current VMCB.
bool nsvm_in_l2(struct vcpu *v);
// Whether any vCPU has SVM enabled, the guest's guests then reaching the local
// APICs' pages through the hypervisor's nested page tables without AVIC.
bool nsvm_any_svm(void);
// The global interrupt flag, which holds back interrupts, NMIs, and INIT while
// clear (APM 24593 rev. 3.46, 15.17).
bool nsvm_gif(struct vcpu *v);
// Takes the guest's global interrupt flag from VMCB01, which its own CLGI and
// STGI change where the processor virtualises it, as an exit begins.
void nsvm_sync_gif(struct vcpu *v);
// The guest's guest's TSC offset, from the guest's and the one its VMCB adds.
uint64_t nsvm_tsc_offset(struct vcpu *v);
// An exit of the guest's guest: whether it went to the guest, else the
// hypervisor's to handle as its own.
bool nsvm_l2_exit(struct vcpu *v);
// Before the vCPU's entry: whether it enters the guest's guest, the guest's
// own events seen to as exits.
bool nsvm_before_entry(struct vcpu *v);
// Whether INIT waits, as a clear global interrupt flag holds it, or for
// nsvm_before_entry() to take it as the guest's guest runs.
bool nsvm_holds_init(struct vcpu *v);
// INIT for a vCPU, which leaves the guest's guest and the guest's SVM state as
// the processor's own INIT does; whether the guest redirects it to #SX instead
// (APM 24593 rev. 3.46, 15.21.8).
bool nsvm_init_signal(struct vcpu *v);
// Whether the guest's guest can take the guest's interrupt now, as its VMCB's
// interrupt masking has it, and whether something ends its HLT.
bool nsvm_interruptible(struct vcpu *v);
bool nsvm_halt_ends(struct vcpu *v);
// An exit once the guest's guest can take an interrupt, or none, in VMCB02's
// virtual interrupt, which the guest's own then waits behind.
void nsvm_interrupt_window(struct vcpu *v, bool on);
// An exception that what the hypervisor carries out for the guest's guest
// raises: whether the guest intercepts it instead, with the EXITINFO2 given.
bool nsvm_exception(
    struct vcpu *v, uint8_t vector, bool has_error, uint32_t error, uint64_t info2
);
// Whether the guest intercepts an exception of its guest, which the vCPU runs.
bool nsvm_intercepts_exception(struct vcpu *v, uint8_t vector);
// Whether a software interrupt or exception that an exit cut short is the one
// VMRUN injected into the guest's guest, which then goes in again.
bool nsvm_inject_again(struct vcpu *v, uint64_t event);
// A shutdown of the guest's guest, which the vCPU runs: whether the guest
// intercepts it, which then exits to the guest.
bool nsvm_shutdown(struct vcpu *v);
// Whether such an exit waits for the next entry.
bool nsvm_exit_pending(struct vcpu *v);
// The guest's PAT, which its guest shares where the guest has no nested paging
// of its own.
void nsvm_set_pat(struct vcpu *v, uint64_t pat);
// Whether the vCPU runs the guest's guest through the guest's nested page
// tables, which then translate what the vCPU's accesses take as guest-physical
// addresses.
bool nsvm_npt_active(struct vcpu *v);
// An address of the guest's guest through the guest's nested page tables, for
// a data write, an instruction fetch, or neither, of the guest's guest's own
// page table walks or else at the address it translates to, in user mode or
// not: false where that is a nested page fault of the guest's instead, which
// waits for the next entry.
bool nsvm_npt_translate(
    struct vcpu *v, uintptr_t gpa, bool write, bool fetch, bool walk, bool user, uint64_t *out
);
// The hypervisor's nested page tables changed: the translations of every ASID
// the guest's guests ran in go too.
void nsvm_gpt_changed(struct vcpu *v);

#endif
