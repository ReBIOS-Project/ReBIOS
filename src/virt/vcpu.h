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

#ifndef VIRT__VCPU_H
#define VIRT__VCPU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <virt/mtrr.h>

// Indexed by the register numbers used in instruction encodings. The RSP slot
// is unused: the guest's RSP lives in the VMCS or VMCB.
struct gprs {
    uint64_t rax, rcx, rdx, rbx, rsp, rbp, rsi, rdi;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
};

#define SEG_ES 0
#define SEG_CS 1
#define SEG_SS 2
#define SEG_DS 3
#define SEG_FS 4
#define SEG_GS 5
#define SEG_LDTR 6
#define SEG_TR 7
#define SEG_GDTR 8
#define SEG_IDTR 9

// Attributes are in the VMCB's packed form: descriptor bits 47:40 in bits 7:0
// and descriptor bits 55:52 in bits 11:8, plus SEG_UNUSABLE.
#define SEG_L ((uint32_t)1 << 9)
#define SEG_DB ((uint32_t)1 << 10)
#define SEG_UNUSABLE ((uint32_t)1 << 16)

struct segment {
    uint16_t selector;
    uint32_t attrib;
    uint32_t limit;
    uintptr_t base;
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
struct mmio_step;
struct nvmx;
struct vlapic;
struct vmcb;
struct vmcs_cache;

struct vcpu {
    // Must come first: the entry stubs address it from the struct's start.
    struct gprs gprs;

    struct cpu *cpu;

    // VMX.
    void *vmcs;
    uintptr_t vmcs_pa;
    bool vmx_launched;
    struct vmcs_cache *vmcs_cache;

    // SVM.
    struct vmcb *vmcb;
    uintptr_t vmcb_pa;
    // The VMCB as the last exit left it, for its clean bits.
    uint8_t *vmcb_seen;
    // Whether the vCPU is in the guest or entering it, for AVIC's doorbells.
    volatile bool in_guest;
    // The instruction bytes the processor fetched for the exit at hand, at
    // fetched_rip, if fetched_count is not 0.
    uint8_t fetched[15];
    size_t fetched_count;
    uint64_t fetched_rip;
    // The gpt_generation the vCPU's TLB is current with.
    uint64_t gpt_generation;

    // An event whose delivery an exit cut short, to deliver again, with its
    // error code and, for VMX's software events, instruction length: in the
    // format VMX's interruption information and SVM's events share.
    uint32_t redeliver;
    uint32_t redeliver_error;
    size_t redeliver_length;
    // Whether vcpu_inject_exception() set up an event for the coming entry.
    bool injecting;
    bool nmi_pending;
    struct vlapic *vlapic;
    struct nvmx *nvmx;
    // Whether the guest runs one instruction against emulated memory's values,
    // which mmio.c sets up, and what SVM's stepping changed.
    bool stepping;
    struct mmio_step *step;
    bool step_tf;
    uint64_t step_dr6;
    uint32_t step_exceptions;
    uint64_t step_debugctl;
    // DR6's B0-B3 bits for the guest's breakpoints that the accesses the
    // hypervisor carries out for it hit: a debug trap once the instruction, or
    // its string iteration, ends.
    uint64_t db_pending;
    // Set by an emulated device's write that it is not done with, which then
    // runs again once the guest took its events.
    bool mmio_retry;
    // A port's handler says the same of the access it was given, which is not
    // to happen yet.
    bool io_retry;

    // The guest's MTRRs: the default type, the fixed ranges, and the variable
    // ranges' bases and masks.
    uint64_t mtrr_def_type;
    uint64_t mtrr_fixed[MTRR_FIXED_COUNT];
    uint64_t mtrr_variable[2 * MTRR_VARIABLE_COUNT];
    // The guest's copies of the IA32_MISC_ENABLE bits the processor's keep
    // clear (msr.c).
    uint64_t misc_enable;
    // The guest's IA32_MCG_CTL where the processor gets another (msr.c).
    uint64_t mcg_ctl;
    // The guest's own AMD memory routing (msr.c): SYSCFG, the IORRs, the tops
    // of memory, and the configuration space window.
    uint64_t amd_syscfg;
    uint64_t amd_iorr[4];
    uint64_t amd_top_mem;
    uint64_t amd_top_mem2;
    uint64_t amd_mmio_cfg_base;
    // AMD's CPUID override MSRs, MSRC001_1002 to MSRC001_1005 (msr.c): the
    // processor's values, the guest's, and which ones the processor has.
    uint64_t amd_cpuid_hardware[4];
    uint64_t amd_cpuid[4];
    uint8_t amd_cpuid_present;
    // The guest's AMD core performance event selects (msr.c).
    uint64_t amd_perf_evt_sel[6];
    // The guest's TSC is the processor's plus this, and its IA32_TSC_ADJUST is
    // its own (msr.c).
    uint64_t tsc_offset;
    uint64_t tsc_adjust;

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
    // For profile.c: the exit being handled, and when it began.
    unsigned int profile_slot;
    uint64_t profile_tsc;
    // The last exit's VMX basic exit reason or SVM exit code, for panics.
    uint32_t last_exit;
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
// An interrupt of the hypervisor's own pending at the entry, which ends it
// right after any event it delivers.
void vcpu_exit_soon(struct vcpu *v);

// Interrupts the processor's halt, as one waiting for a start-up IPI is in.
void vcpu_wake(struct cpu *cpu);

// Waits in the hypervisor, taking interrupts, until ends() holds: halted, or
// spinning where nothing would interrupt the halt in time.
void vcpu_wait(struct vcpu *v, bool (*ends)(struct vcpu *v), bool spin);
// The guest's HLT, which waits in the hypervisor, as an interrupt that only
// becomes deliverable while the guest halts must end it.
void vcpu_halt(struct vcpu *v);

// Keeps every other vCPU out of the guest until vcpu_resume_others(), and
// whether vCPUs can be single-stepped and making them.
void vcpu_pause_others(void);
void vcpu_resume_others(void);
bool vcpu_can_single_step(void);
void vcpu_single_step(struct vcpu *v, bool on);

uint64_t vcpu_rip(struct vcpu *v);
void vcpu_set_rip(struct vcpu *v, uintptr_t rip);
uint64_t vcpu_rsp(struct vcpu *v);
void vcpu_set_rsp(struct vcpu *v, uintptr_t rsp);
uint64_t vcpu_rflags(struct vcpu *v);
void vcpu_set_rflags(struct vcpu *v, uint64_t rflags);
uint64_t vcpu_cr0(struct vcpu *v);
uint64_t vcpu_cr3(struct vcpu *v);
uint64_t vcpu_cr4(struct vcpu *v);
uint64_t vcpu_efer(struct vcpu *v);
// A 64-bit mode data access's linear address through a segment, without the
// metadata LAM masks (SDM 325462-093 Vol. 3A, 4.4.2) or the bits Upper Address
// Ignore leaves out (APM 24593 rev. 3.45, 5.10.2).
uint64_t vcpu_untagged(struct vcpu *v, size_t reg, uintptr_t linear);
void vcpu_get_seg(struct vcpu *v, size_t reg, struct segment *seg);
void vcpu_set_seg(struct vcpu *v, size_t reg, const struct segment *seg);
void vcpu_save_mode(struct vcpu *v, struct vcpu_mode *mode);
void vcpu_load_mode(struct vcpu *v, const struct vcpu_mode *mode);

// Advances RIP past the instruction that caused the current exit.
void vcpu_skip_instruction(struct vcpu *v);
// Moves past length bytes of instruction, as completing it does, with the
// single-step trap the guest's TF asks for.
void vcpu_advance(struct vcpu *v, size_t length);
// Whether the guest single-steps, and its trap after an instruction, or a
// string instruction's iteration, that the hypervisor carried out for it.
bool vcpu_guest_tf(struct vcpu *v);
void vcpu_step_trap(struct vcpu *v);
// A debug trap with the given DR6 bits, as the processor raises one.
void vcpu_debug_trap(struct vcpu *v, uint64_t dr6);
// Where an access the hypervisor carries out for the guest meets the guest's
// data and I/O breakpoints.
void vcpu_data_access(
    struct vcpu *v, uintptr_t linear, size_t size, bool write
);
void vcpu_io_access(struct vcpu *v, uint16_t port, size_t size);
// The debug trap for the breakpoints those hit, and the single step asked for.
void vcpu_debug_traps(struct vcpu *v, bool single_step);
// After an instruction, or a string instruction's iteration, the hypervisor
// carried out: the trap the guest's TF asks for, or the MTF exit the guest
// asks its guest to make; and whether either stops the guest after each.
void vcpu_step_boundary(struct vcpu *v);
bool vcpu_stepped(struct vcpu *v);
void vcpu_inject_exception(
    struct vcpu *v, uint8_t vector, bool has_error, uint32_t error
);
void vcpu_inject_page_fault(struct vcpu *v, uintptr_t linear, uint32_t error);

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
