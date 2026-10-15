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

#ifndef VIRT__VCPU_H
#define VIRT__VCPU_H

#include <stdint.h>
#include <stdbool.h>

// Indexed by the register numbers used in instruction encodings. The RSP slot
// is unused: the guest's RSP lives in the VMCS or VMCB.
struct gprs {
    uint64_t rax, rcx, rdx, rbx, rsp, rbp, rsi, rdi;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
};

enum seg_reg {
    SEG_ES,
    SEG_CS,
    SEG_SS,
    SEG_DS,
    SEG_FS,
    SEG_GS,
    SEG_LDTR,
    SEG_TR,
    SEG_GDTR,
    SEG_IDTR,
};

// Attributes are in the VMCB's packed form: descriptor bits 47:40 in bits 7:0
// and descriptor bits 55:52 in bits 11:8, plus SEG_UNUSABLE.
#define SEG_UNUSABLE (1u << 16)

struct segment {
    uint16_t selector;
    uint32_t attrib;
    uint32_t limit;
    uint64_t base;
};

struct cpu;
struct vmcb;

struct vcpu {
    // Must come first: the entry stubs address it from the struct's start.
    struct gprs gprs;

    struct cpu *cpu;

    // VMX.
    void *vmcs;
    uint64_t vmcs_pa;
    bool vmx_launched;

    // SVM.
    struct vmcb *vmcb;
    uint64_t vmcb_pa;
    // The gpt_generation the vCPU's TLB is current with.
    uint64_t gpt_generation;

    // What the guest believes CR0 and CR4 hold, for the bits the host owns.
    uint64_t cr0_shadow;
    uint64_t cr4_shadow;
};

extern bool virt_vmx;

struct vcpu *vcpu_create(struct cpu *cpu);
__attribute__((noreturn)) void vcpu_run(struct vcpu *v);

uint64_t vcpu_rip(struct vcpu *v);
void vcpu_set_rip(struct vcpu *v, uint64_t rip);
uint64_t vcpu_rsp(struct vcpu *v);
void vcpu_set_rsp(struct vcpu *v, uint64_t rsp);
uint64_t vcpu_rflags(struct vcpu *v);
void vcpu_set_rflags(struct vcpu *v, uint64_t rflags);
uint64_t vcpu_cr0(struct vcpu *v);
uint64_t vcpu_cr3(struct vcpu *v);
uint64_t vcpu_cr4(struct vcpu *v);
uint64_t vcpu_efer(struct vcpu *v);
void vcpu_get_seg(struct vcpu *v, enum seg_reg reg, struct segment *seg);
void vcpu_set_seg(struct vcpu *v, enum seg_reg reg, const struct segment *seg);

// Advances RIP past the instruction that caused the current exit.
void vcpu_skip_instruction(struct vcpu *v);
void vcpu_inject_exception(struct vcpu *v, uint8_t vector, bool has_error, uint32_t error);

// Puts the vCPU in the state the processor has after INIT, in real mode at
// cs:ip, as for a start-up IPI or the reset vector.
void vcpu_set_real_mode(struct vcpu *v, uint16_t cs, uint16_t ip);

#endif
