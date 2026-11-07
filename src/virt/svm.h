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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdnoreturn.h>

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
    uint8_t reserved4[0x120 - 0x100];
    // The bus locks the guest may make before one exits (APM 24593 rev. 3.46,
    // 15.14.5).
    uint16_t bus_lock_counter;
    uint8_t reserved5[0x170 - 0x122];
    // FRED's event data (AMD 69191 rev. 1.10).
    uint64_t exitintdata;
    uint64_t eventinjdata;
    uint8_t reserved6[0x1c8 - 0x180];
    uint64_t pml_base;
    uint16_t pml_index;
    uint8_t reserved7[0x400 - 0x1d2];
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
    uint8_t reserved5[0x8c0 - 0x298];
    // The FRED MSRs that the processor switches with the VMCB, all but
    // FRED_RSP0 (AMD 69191 rev. 1.10).
    uint64_t fred_rsp[3];
    uint64_t fred_stklvls;
    uint64_t fred_ssp[3];
    uint64_t fred_config;
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
_Static_assert(offsetof(struct vmcb_control, bus_lock_counter) == 0x120, "VMCB layout");
_Static_assert(offsetof(struct vmcb_control, exitintdata) == 0x170, "VMCB layout");
_Static_assert(offsetof(struct vmcb_control, pml_index) == 0x1d0, "VMCB layout");
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
_Static_assert(offsetof(struct vmcb_save, fred_rsp) == 0x8c0, "VMCB layout");

// The intercept vectors, 32 bits each from offset 000h, in which an exit code
// below C0h is the number of its intercept's bit (APM 24593 rev. 3.46, Table
// B-1 and Table C-1).
#define SVM_INTERCEPT_VECTORS 6

// Intercept vector 0, at offset 000h: reads and writes of CR0 and CR8, and
// writes of CR4 (APM 24593 rev. 3.46, Table B-1).
#define SVM_INTERCEPT_CR0_READ ((uint32_t)1 << 0)
#define SVM_INTERCEPT_CR8_READ ((uint32_t)1 << 8)
#define SVM_INTERCEPT_CR0_WRITE ((uint32_t)1 << 16)
#define SVM_INTERCEPT_CR4_WRITE ((uint32_t)1 << 20)
#define SVM_INTERCEPT_CR8_WRITE ((uint32_t)1 << 24)

// Intercept vector 3, at offset 00Ch.
#define SVM_INTERCEPT_INTR ((uint32_t)1 << 0)
#define SVM_INTERCEPT_NMI ((uint32_t)1 << 1)
#define SVM_INTERCEPT_SMI ((uint32_t)1 << 2)
#define SVM_INTERCEPT_INIT ((uint32_t)1 << 3)
#define SVM_INTERCEPT_VINTR ((uint32_t)1 << 4)
// Writes of CR0 that change bits other than TS and MP.
#define SVM_INTERCEPT_CR0_SEL_WRITE ((uint32_t)1 << 5)
#define SVM_INTERCEPT_CPUID ((uint32_t)1 << 18)
#define SVM_INTERCEPT_RSM ((uint32_t)1 << 19)
#define SVM_INTERCEPT_INVD ((uint32_t)1 << 22)
#define SVM_INTERCEPT_HLT ((uint32_t)1 << 24)
#define SVM_INTERCEPT_INVLPGA ((uint32_t)1 << 26)
#define SVM_INTERCEPT_IOIO ((uint32_t)1 << 27)
#define SVM_INTERCEPT_MSR ((uint32_t)1 << 28)
#define SVM_INTERCEPT_TASK_SWITCH ((uint32_t)1 << 29)
#define SVM_INTERCEPT_SHUTDOWN ((uint32_t)1 << 31)

// Intercept vector 4, at offset 010h.
#define SVM_INTERCEPT_VMRUN ((uint32_t)1 << 0)
#define SVM_INTERCEPT_VMMCALL ((uint32_t)1 << 1)
#define SVM_INTERCEPT_VMLOAD ((uint32_t)1 << 2)
#define SVM_INTERCEPT_VMSAVE ((uint32_t)1 << 3)
#define SVM_INTERCEPT_STGI ((uint32_t)1 << 4)
#define SVM_INTERCEPT_CLGI ((uint32_t)1 << 5)
#define SVM_INTERCEPT_SKINIT ((uint32_t)1 << 6)
#define SVM_INTERCEPT_XSETBV ((uint32_t)1 << 13)

// Intercept vector 5, at offset 014h.
#define SVM_INTERCEPT_INVLPGB ((uint32_t)1 << 0)
#define SVM_INTERCEPT_INVLPGB_ILLEGAL ((uint32_t)1 << 1)
#define SVM_INTERCEPT_INVPCID ((uint32_t)1 << 2)
#define SVM_INTERCEPT_TLBSYNC ((uint32_t)1 << 4)
#define SVM_INTERCEPT_BUS_LOCK ((uint32_t)1 << 5)
#define SVM_INTERCEPT_IDLE_HLT ((uint32_t)1 << 6)

// Exit codes (APM 24593 rev. 3.45, Appendix C).
#define SVM_EXIT_CR0_READ 0x00
#define SVM_EXIT_CR8_READ 0x08
#define SVM_EXIT_CR0_WRITE 0x10
#define SVM_EXIT_CR4_WRITE 0x14
#define SVM_EXIT_CR8_WRITE 0x18
#define SVM_EXIT_EXCEPTION_BASE 0x40
#define SVM_EXIT_INTR 0x60
#define SVM_EXIT_NMI 0x61
#define SVM_EXIT_SMI 0x62
#define SVM_EXIT_INIT 0x63
#define SVM_EXIT_VINTR 0x64
#define SVM_EXIT_CR0_SEL_WRITE 0x65
#define SVM_EXIT_CPUID 0x72
#define SVM_EXIT_RSM 0x73
#define SVM_EXIT_INVD 0x76
#define SVM_EXIT_HLT 0x78
#define SVM_EXIT_INVLPGA 0x7a
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
#define SVM_EXIT_INVLPGB 0xa0
#define SVM_EXIT_INVLPGB_ILLEGAL 0xa1
#define SVM_EXIT_IDLE_HLT 0xa6
#define SVM_EXIT_NPF 0x400
#define SVM_EXIT_AVIC_INCOMPLETE_IPI 0x401
#define SVM_EXIT_AVIC_NOACCEL 0x402
#define SVM_EXIT_PML_FULL 0x407
#define SVM_EXIT_INVALID ((uint64_t)0xffffffffffffffff)

// A nested page fault's error code (APM 24593 rev. 3.46, 15.25.7).
#define NPF_P ((uint64_t)1 << 0)
#define NPF_RW ((uint64_t)1 << 1)
#define NPF_US ((uint64_t)1 << 2)
#define NPF_RSV ((uint64_t)1 << 3)
#define NPF_ID ((uint64_t)1 << 4)
#define NPF_FINAL ((uint64_t)1 << 32)
#define NPF_TABLE ((uint64_t)1 << 33)

// Virtual interrupt control, at offset 060h.
#define SVM_VINTR_TPR_MASK ((uint64_t)0xf)
#define SVM_VINTR_IRQ ((uint64_t)1 << 8)
#define SVM_VINTR_GIF ((uint64_t)1 << 9)
#define SVM_VINTR_NMI ((uint64_t)1 << 11)
#define SVM_VINTR_NMI_MASK ((uint64_t)1 << 12)
#define SVM_VINTR_PRIO_SHIFT 16
#define SVM_VINTR_PRIO_MASK ((uint64_t)0xf << SVM_VINTR_PRIO_SHIFT)
#define SVM_VINTR_IGN_TPR ((uint64_t)1 << 20)
#define SVM_VINTR_MASKING ((uint64_t)1 << 24)
#define SVM_VINTR_GIF_ENABLE ((uint64_t)1 << 25)
#define SVM_VINTR_NMI_ENABLE ((uint64_t)1 << 26)
#define SVM_VINTR_X2AVIC ((uint64_t)1 << 30)
#define SVM_VINTR_AVIC ((uint64_t)1 << 31)
#define SVM_VINTR_VECTOR_MASK ((uint64_t)0xff << 32)

// The interrupt shadow, at offset 068h.
#define SVM_INTERRUPT_SHADOW ((uint64_t)1 << 0)

// TLB_CONTROL, at offset 05Ch (APM 24593 rev. 3.46, Table 15-9), and the
// return address predictor's control beside it.
#define SVM_TLB_FLUSH_ALL 1
#define SVM_TLB_FLUSH_ASID 3
#define SVM_TLB_FLUSH_ASID_LOCAL 7
#define SVM_RAP_ALLOW_LARGER ((uint8_t)1 << 0)
#define SVM_RAP_CLEAR ((uint8_t)1 << 1)

// CPUID Fn8000_000A_EDX (APM 24594 rev. 3.39, E.4.9).
#define SVM_FEATURE_NP ((uint32_t)1 << 0)
#define SVM_FEATURE_LBRV ((uint32_t)1 << 1)
#define SVM_FEATURE_SVML ((uint32_t)1 << 2)
#define SVM_FEATURE_NRIPS ((uint32_t)1 << 3)
#define SVM_FEATURE_TSC_RATIO ((uint32_t)1 << 4)
#define SVM_FEATURE_VMCB_CLEAN ((uint32_t)1 << 5)
#define SVM_FEATURE_FLUSH_BY_ASID ((uint32_t)1 << 6)
#define SVM_FEATURE_DECODE_ASSISTS ((uint32_t)1 << 7)
#define SVM_FEATURE_PAUSE_FILTER ((uint32_t)1 << 10)
#define SVM_FEATURE_PAUSE_FILTER_THRESHOLD ((uint32_t)1 << 12)
#define SVM_FEATURE_AVIC ((uint32_t)1 << 13)
#define SVM_FEATURE_VLS ((uint32_t)1 << 15)
#define SVM_FEATURE_VGIF ((uint32_t)1 << 16)
#define SVM_FEATURE_GMET ((uint32_t)1 << 17)
#define SVM_FEATURE_X2AVIC ((uint32_t)1 << 18)
#define SVM_FEATURE_ROGPT ((uint32_t)1 << 21)
#define SVM_FEATURE_HOST_MCE_OVERRIDE ((uint32_t)1 << 23)
#define SVM_FEATURE_TLBICTL ((uint32_t)1 << 24)
#define SVM_FEATURE_VNMI ((uint32_t)1 << 25)
#define SVM_FEATURE_EXT_LVT_AVIC_ACCESS ((uint32_t)1 << 27)
#define SVM_FEATURE_VMCB_ADDR_CHECK ((uint32_t)1 << 28)
#define SVM_FEATURE_BUS_LOCK_THRESHOLD ((uint32_t)1 << 29)
#define SVM_FEATURE_IDLE_HLT ((uint32_t)1 << 30)

// Nested paging and what goes with it, at offset 090h.
#define SVM_NP_ENABLE ((uint64_t)1 << 0)
#define SVM_NP_GMET ((uint64_t)1 << 3)
#define SVM_NP_ROGPT ((uint64_t)1 << 6)
#define SVM_NP_PML ((uint64_t)1 << 11)
#define SVM_NP_INVLPGB ((uint64_t)1 << 7)

// Virtualisation extensions, at offset 0B8h.
#define SVM_VIRT_EXT_LBR ((uint64_t)1 << 0)
#define SVM_VIRT_EXT_VLS ((uint64_t)1 << 1)
#define SVM_VIRT_EXT_FRED ((uint64_t)1 << 4)

// Event injection.
#define SVM_EVENT_VALID ((uint64_t)1 << 31)
#define SVM_EVENT_ERROR_VALID ((uint64_t)1 << 11)
#define SVM_EVENT_VECTOR_MASK ((uint64_t)0xff)
#define SVM_EVENT_TYPE_MASK ((uint64_t)7 << 8)
#define SVM_EVENT_TYPE_INTR ((uint64_t)0 << 8)
#define SVM_EVENT_TYPE_NMI ((uint64_t)2 << 8)
#define SVM_EVENT_TYPE_EXCEPTION ((uint64_t)3 << 8)
#define SVM_EVENT_TYPE_SOFT_INT ((uint64_t)4 << 8)
// FRED's event types and nested-exception bit, in the events of a guest with
// CR4.FRED (AMD 69191 rev. 1.10).
#define SVM_EVENT_TYPE_INT1 ((uint64_t)5 << 8)
#define SVM_EVENT_TYPE_SYSCALL ((uint64_t)7 << 8)
#define SVM_EVENT_NESTED ((uint64_t)1 << 13)

#define MSR_DC_CFG 0xc0011022
#define DC_CFG_ERRATUM_383 ((uint64_t)1 << 47)
#define MSR_VM_CR 0xc0010114
#define MSR_VM_HSAVE_PA 0xc0010117
#define MSR_SVM_KEY 0xc0010118
#define MSR_TSC_RATIO 0xc0000104
#define VM_CR_DPD ((uint32_t)1 << 0)
#define VM_CR_R_INIT ((uint32_t)1 << 1)
#define VM_CR_DIS_A20M ((uint32_t)1 << 2)
#define VM_CR_LOCK ((uint32_t)1 << 3)
#define VM_CR_SVMDIS ((uint32_t)1 << 4)

struct vcpu;

// Whether the hypervisor works around AMD's erratum 383 (41322 rev. 3.92) with
// DC_CFG.
extern bool svm_erratum_383;
// Whether VMRUN and #VMEXIT switch DebugCtl and the last branch records.
extern bool svm_lbr_virtualisation;
extern bool svm_nrips;
// Exits report the operands of the instructions they intercept, and the bytes
// of those whose data accesses fault (APM 24593 rev. 3.46, 15.8).
extern bool svm_decode_assists;
// The processor keeps the guest's NMI blocking and takes a virtual NMI when
// the guest can (APM 24593 rev. 3.45, 15.21.10), with GIF virtualised, as
// Linux 7.3's KVM has it.
extern bool svm_vnmi;
// The processor holds virtual interrupts back under a virtual GIF, which the
// guest's CLGI and STGI change where they do not exit (APM 24593 rev. 3.46,
// 15.33.2).
extern bool svm_vgif;
// The guest's VMLOAD and VMSAVE run on the processor where they do not exit,
// with addresses in the guest's physical memory (APM 24593 rev. 3.46,
// 15.33.1).
extern bool svm_vls;
// VMRUN, VMLOAD, and VMSAVE exit on their intercepts whatever their address,
// rather than raising #GP at addresses the processor reserves.
extern bool svm_vmcb_addr_check;
// SVM-Lock: the SVM key can clear VM_CR.LOCK, and STGI and SKINIT need no
// EFER.SVME (APM 24593 rev. 3.46, 15.31).
extern bool svm_svml;
// The processor's AVIC (APM 24593 rev. 3.45, 15.29), where it can be used, and
// whether it runs x2APIC mode's MSRs too (APM 24593 rev. 3.45, 15.29.10).
extern bool svm_avic;
extern bool svm_x2avic;
// AVIC reads AMD's extended LVT entries from the backing page and traps writes
// to them, where it otherwise faults on both (APM 24593 rev. 3.46, Table
// 15-22; APM 24594 rev. 3.39, CPUID Fn8000_000A_EDX[ExtLvtAvicAccessChg]).
extern bool svm_avic_ext_lvt_reads;
// TLB_CONTROL can flush one ASID's translations, and the processor's return
// address predictor is the guest's to have cleared (svm.c).
extern bool svm_flush_by_asid;
extern bool svm_eraps;
// TlbiCtl: the hypervisor enables its guests' INVLPGB and TLBSYNC, which run
// without it on processors that lack it, and TLBSYNC can be intercepted (APM
// 24593 rev. 3.46, Table B-1).
extern bool svm_tlbictl;
// The processor's VM_CR as the firmware left it.
extern uint64_t svm_firmware_vm_cr;

bool svm_supported(void);
// The guest's translations go before its next run, as when the hypervisor
// changes its paging state or nested page tables (APM 24593 rev. 3.45, 15.16).
void svm_flush_guest_tlb(struct vcpu *v);
bool svm_huge_pages(void);
void svm_cpu_init(void);
void svm_vcpu_init(struct vcpu *v);
// The guest's CR0 as it has it: the processor's but for NE, which the guest's
// x87 errors follow (ferr.c), and which the processor keeps set, the guest's
// own being in cr0_shadow; and the loading of CR0 so.
uint64_t svm_guest_cr0(struct vcpu *v);
void svm_set_cr0(struct vcpu *v, uint64_t value);
noreturn void svm_run(struct vcpu *v);
// The guest got where the processor cannot go on from, as a triple fault does.
noreturn void svm_shutdown(struct vcpu *v, const char *why);

#endif
