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

#ifndef VIRT__SVM_H
#define VIRT__SVM_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// VMCB layout (APM 24593 rev. 3.45, Appendix B).
struct vmcb_control {
    uint32_t intercept_cr;
    uint32_t intercept_dr;
    uint32_t intercept_exceptions;
    uint32_t intercept_misc1;
    uint32_t intercept_misc2;
    uint32_t intercept_misc3;
    uint8_t reserved0[0x3c - 0x18];
    uint16_t pause_filter_threshold;
    uint16_t pause_filter_count;
    uint64_t iopm_base_pa;
    uint64_t msrpm_base_pa;
    uint64_t tsc_offset;
    uint32_t guest_asid;
    uint8_t tlb_control;
    uint8_t rap_control;
    uint8_t reserved1[2];
    uint64_t vintr;
    uint64_t interrupt_shadow;
    uint64_t exitcode;
    uint64_t exitinfo1;
    uint64_t exitinfo2;
    uint64_t exitintinfo;
    uint64_t np_enable;
    uint64_t avic_apic_bar;
    uint64_t ghcb_pa;
    uint64_t eventinj;
    uint64_t n_cr3;
    uint64_t virt_ext;
    uint32_t vmcb_clean;
    uint32_t reserved2;
    uint64_t nrip;
    uint8_t insn_len;
    uint8_t insn_bytes[15];
    uint64_t avic_backing_page;
    uint64_t reserved3;
    uint64_t avic_logical_table;
    uint64_t avic_physical_table;
    uint8_t reserved4[0x400 - 0x100];
} __attribute__((packed));

struct vmcb_seg {
    uint16_t selector;
    uint16_t attrib;
    uint32_t limit;
    uint64_t base;
} __attribute__((packed));

struct vmcb_save {
    struct vmcb_seg es;
    struct vmcb_seg cs;
    struct vmcb_seg ss;
    struct vmcb_seg ds;
    struct vmcb_seg fs;
    struct vmcb_seg gs;
    struct vmcb_seg gdtr;
    struct vmcb_seg ldtr;
    struct vmcb_seg idtr;
    struct vmcb_seg tr;
    uint8_t reserved0[0xcb - 0xa0];
    uint8_t cpl;
    uint32_t reserved1;
    uint64_t efer;
    uint8_t reserved2[0x148 - 0xd8];
    uint64_t cr4;
    uint64_t cr3;
    uint64_t cr0;
    uint64_t dr7;
    uint64_t dr6;
    uint64_t rflags;
    uint64_t rip;
    uint8_t reserved3[0x1d8 - 0x180];
    uint64_t rsp;
    uint64_t s_cet;
    uint64_t ssp;
    uint64_t isst_addr;
    uint64_t rax;
    uint64_t star;
    uint64_t lstar;
    uint64_t cstar;
    uint64_t sfmask;
    uint64_t kernel_gs_base;
    uint64_t sysenter_cs;
    uint64_t sysenter_esp;
    uint64_t sysenter_eip;
    uint64_t cr2;
    uint8_t reserved4[0x268 - 0x248];
    uint64_t g_pat;
    uint64_t dbgctl;
    uint64_t br_from;
    uint64_t br_to;
    uint64_t last_excp_from;
    uint64_t last_excp_to;
} __attribute__((packed));

struct vmcb {
    struct vmcb_control control;
    struct vmcb_save save;
} __attribute__((packed));

_Static_assert(offsetof(struct vmcb_control, iopm_base_pa) == 0x40, "VMCB layout");
_Static_assert(offsetof(struct vmcb_control, guest_asid) == 0x58, "VMCB layout");
_Static_assert(offsetof(struct vmcb_control, exitcode) == 0x70, "VMCB layout");
_Static_assert(offsetof(struct vmcb_control, eventinj) == 0xa8, "VMCB layout");
_Static_assert(offsetof(struct vmcb_control, n_cr3) == 0xb0, "VMCB layout");
_Static_assert(offsetof(struct vmcb_control, vmcb_clean) == 0xc0, "VMCB layout");
_Static_assert(offsetof(struct vmcb_control, nrip) == 0xc8, "VMCB layout");
_Static_assert(offsetof(struct vmcb_control, insn_len) == 0xd0, "VMCB layout");
_Static_assert(offsetof(struct vmcb_control, avic_backing_page) == 0xe0, "VMCB layout");
_Static_assert(offsetof(struct vmcb_control, avic_physical_table) == 0xf8, "VMCB layout");
_Static_assert(sizeof(struct vmcb_control) == 0x400, "VMCB layout");
_Static_assert(offsetof(struct vmcb_save, tr) == 0x90, "VMCB layout");
_Static_assert(offsetof(struct vmcb_save, cpl) == 0xcb, "VMCB layout");
_Static_assert(offsetof(struct vmcb_save, efer) == 0xd0, "VMCB layout");
_Static_assert(offsetof(struct vmcb_save, cr4) == 0x148, "VMCB layout");
_Static_assert(offsetof(struct vmcb_save, rip) == 0x178, "VMCB layout");
_Static_assert(offsetof(struct vmcb_save, rsp) == 0x1d8, "VMCB layout");
_Static_assert(offsetof(struct vmcb_save, rax) == 0x1f8, "VMCB layout");
_Static_assert(offsetof(struct vmcb_save, cr2) == 0x240, "VMCB layout");
_Static_assert(offsetof(struct vmcb_save, g_pat) == 0x268, "VMCB layout");

// Intercept vector 3, at offset 00Ch.
#define SVM_INTERCEPT_INTR (1u << 0)
#define SVM_INTERCEPT_NMI (1u << 1)
#define SVM_INTERCEPT_SMI (1u << 2)
#define SVM_INTERCEPT_INIT (1u << 3)
#define SVM_INTERCEPT_VINTR (1u << 4)
#define SVM_INTERCEPT_CPUID (1u << 18)
#define SVM_INTERCEPT_RSM (1u << 19)
#define SVM_INTERCEPT_INVD (1u << 22)
#define SVM_INTERCEPT_HLT (1u << 24)
#define SVM_INTERCEPT_IOIO (1u << 27)
#define SVM_INTERCEPT_MSR (1u << 28)
#define SVM_INTERCEPT_TASK_SWITCH (1u << 29)
#define SVM_INTERCEPT_SHUTDOWN (1u << 31)

// Intercept vector 4, at offset 010h.
#define SVM_INTERCEPT_VMRUN (1u << 0)
#define SVM_INTERCEPT_VMMCALL (1u << 1)
#define SVM_INTERCEPT_VMLOAD (1u << 2)
#define SVM_INTERCEPT_VMSAVE (1u << 3)
#define SVM_INTERCEPT_STGI (1u << 4)
#define SVM_INTERCEPT_CLGI (1u << 5)
#define SVM_INTERCEPT_SKINIT (1u << 6)
#define SVM_INTERCEPT_XSETBV (1u << 13)

// Exit codes (APM 24593 rev. 3.45, Appendix C).
#define SVM_EXIT_CR0_WRITE 0x10
#define SVM_EXIT_EXCEPTION_BASE 0x40
#define SVM_EXIT_INTR 0x60
#define SVM_EXIT_NMI 0x61
#define SVM_EXIT_SMI 0x62
#define SVM_EXIT_INIT 0x63
#define SVM_EXIT_VINTR 0x64
#define SVM_EXIT_CPUID 0x72
#define SVM_EXIT_RSM 0x73
#define SVM_EXIT_INVD 0x76
#define SVM_EXIT_HLT 0x78
#define SVM_EXIT_IOIO 0x7b
#define SVM_EXIT_MSR 0x7c
#define SVM_EXIT_TASK_SWITCH 0x7d
#define SVM_EXIT_SHUTDOWN 0x7f
#define SVM_EXIT_VMRUN 0x80
#define SVM_EXIT_VMMCALL 0x81
#define SVM_EXIT_VMLOAD 0x82
#define SVM_EXIT_VMSAVE 0x83
#define SVM_EXIT_STGI 0x84
#define SVM_EXIT_CLGI 0x85
#define SVM_EXIT_SKINIT 0x86
#define SVM_EXIT_XSETBV 0x8d
#define SVM_EXIT_NPF 0x400
#define SVM_EXIT_AVIC_INCOMPLETE_IPI 0x401
#define SVM_EXIT_AVIC_NOACCEL 0x402
#define SVM_EXIT_INVALID 0xffffffffffffffffull

// Nested page fault error code: the access was a write, or a fetch.
#define NPF_WRITE (1ull << 1)
#define NPF_FETCH (1ull << 4)

// Virtual interrupt control, at offset 060h.
#define SVM_VINTR_IRQ (1ull << 8)
#define SVM_VINTR_GIF (1ull << 9)
#define SVM_VINTR_NMI (1ull << 11)
#define SVM_VINTR_NMI_MASK (1ull << 12)
#define SVM_VINTR_PRIO_SHIFT 16
#define SVM_VINTR_PRIO_MASK (0xfull << SVM_VINTR_PRIO_SHIFT)
#define SVM_VINTR_IGN_TPR (1ull << 20)
#define SVM_VINTR_MASKING (1ull << 24)
#define SVM_VINTR_GIF_ENABLE (1ull << 25)
#define SVM_VINTR_NMI_ENABLE (1ull << 26)
#define SVM_VINTR_X2AVIC (1ull << 30)
#define SVM_VINTR_AVIC (1ull << 31)

// Event injection.
#define SVM_EVENT_VALID (1ull << 31)
#define SVM_EVENT_ERROR_VALID (1ull << 11)
#define SVM_EVENT_TYPE_INTR (0ull << 8)
#define SVM_EVENT_TYPE_NMI (2ull << 8)
#define SVM_EVENT_TYPE_EXCEPTION (3ull << 8)
#define SVM_EVENT_TYPE_SOFT_INT (4ull << 8)

#define MSR_DC_CFG 0xc0011022
#define DC_CFG_ERRATUM_383 (1ull << 47)
#define MSR_VM_CR 0xc0010114
#define MSR_VM_HSAVE_PA 0xc0010117
#define VM_CR_R_INIT (1u << 1)
#define VM_CR_LOCK (1u << 3)
#define VM_CR_SVMDIS (1u << 4)

struct vcpu;

// Whether the hypervisor works around AMD's erratum 383 (41322 rev. 3.92) with
// DC_CFG.
extern bool svm_erratum_383;
extern bool svm_nrips;
// The processor keeps the guest's NMI blocking and takes a virtual NMI when the
// guest can (APM 24593 rev. 3.45, 15.21.10), with GIF virtualised, as Linux
// 7.3's KVM has it.
extern bool svm_vnmi;
// The processor's AVIC (APM 24593 rev. 3.45, 15.29), where it can be used, and
// whether it runs x2APIC mode's MSRs too (APM 24593 rev. 3.45, 15.29.10).
extern bool svm_avic;
extern bool svm_x2avic;

bool svm_supported(void);
// The guest's translations go before its next run, as when the hypervisor
// changes its paging state or nested page tables (APM 24593 rev. 3.45,
// 15.16).
void svm_flush_guest_tlb(struct vcpu *v);
bool svm_huge_pages(void);
void svm_cpu_init(void);
void svm_vcpu_init(struct vcpu *v);
__attribute__((noreturn)) void svm_run(struct vcpu *v);

#endif
