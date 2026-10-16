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
#define SEG_L (1u << 9)
#define SEG_DB (1u << 10)
#define SEG_UNUSABLE (1u << 16)

struct segment {
    uint16_t selector;
    uint32_t attrib;
    uint32_t limit;
    uint64_t base;
};

// The processor's operating mode, short of the general registers.
struct vcpu_mode {
    uint64_t cr0;
    uint64_t cr3;
    uint64_t cr4;
    uint64_t efer;
    uint64_t rflags;
    struct segment segs[SEG_IDTR + 1];
};

#define VCPU_SIPI_RUNNING (-2)
#define VCPU_SIPI_WAITING (-1)

struct cpu;
struct vlapic;
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

    // An event whose delivery an exit cut short, to deliver again, with its
    // error code: in the format VMX's interruption information and SVM's
    // events share.
    uint32_t redeliver;
    uint32_t redeliver_error;
    // Whether vcpu_inject_exception() set up an event for the coming entry.
    bool injecting;
    bool nmi_pending;
    struct vlapic *vlapic;

    // What the guest believes CR0 and CR4 hold, for the bits the host owns.
    uint64_t cr0_shadow;
    uint64_t cr4_shadow;

    // Whether the vCPU waits for a start-up IPI, VCPU_SIPI_WAITING, or runs,
    // VCPU_SIPI_RUNNING, or else, where the processor cannot wait for one in
    // the guest, the vector of the one passed on to it.
    volatile int32_t sipi;

    // The legacy BIOS's 32-bit flat mode, and the mode its 16-bit code was in
    // when it last switched there, for HYPERCALL_CALL32_*.
    bool call32_ready;
    struct vcpu_mode call32_flat;
    struct vcpu_mode call32_saved;
};

extern bool virt_vmx;

struct vcpu *vcpu_create(struct cpu *cpu);
__attribute__((noreturn)) void vcpu_run(struct vcpu *v);

// Catches up with what changed while the vCPU was out of the guest, and sets
// up what it gets on entering it.
void vcpu_before_entry(struct vcpu *v);

// Makes the CPU's vCPU leave the guest, if it runs it, for vcpu_before_entry()
// to see what changed.
void vcpu_kick(struct cpu *cpu);

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
void vcpu_save_mode(struct vcpu *v, struct vcpu_mode *mode);
void vcpu_load_mode(struct vcpu *v, const struct vcpu_mode *mode);

// Advances RIP past the instruction that caused the current exit.
void vcpu_skip_instruction(struct vcpu *v);
// Moves past length bytes of instruction, as completing it does.
void vcpu_advance(struct vcpu *v, unsigned length);
void vcpu_inject_exception(struct vcpu *v, uint8_t vector, bool has_error, uint32_t error);

// Puts the vCPU in the state the processor has after INIT, in real mode at
// cs:ip, as for a start-up IPI or the reset vector.
void vcpu_set_real_mode(struct vcpu *v, uint16_t cs, uint16_t ip);

// Makes an application processor's vCPU wait for a start-up IPI, as after an
// INIT.
void vcpu_wait_for_sipi(struct vcpu *v);
// An INIT for the vCPU, and a start-up IPI with its vector.
void vcpu_init_signal(struct vcpu *v);
void vcpu_startup(struct vcpu *v, uint8_t vector);

#endif
