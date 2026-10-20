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

#ifndef VIRT__VMX_H
#define VIRT__VMX_H

#include <stdint.h>
#include <stdbool.h>

// VMCS field encodings (SDM 325462-093 Vol. 3D, Appendix B).
#define VMCS_VPID 0x0000
#define VMCS_POSTED_INTR_NV 0x0002
#define VMCS_LAST_PID_POINTER_INDEX 0x0008
#define VMCS_GUEST_ES_SEL 0x0800
#define VMCS_GUEST_CS_SEL 0x0802
#define VMCS_GUEST_SS_SEL 0x0804
#define VMCS_GUEST_DS_SEL 0x0806
#define VMCS_GUEST_FS_SEL 0x0808
#define VMCS_GUEST_GS_SEL 0x080a
#define VMCS_GUEST_LDTR_SEL 0x080c
#define VMCS_GUEST_TR_SEL 0x080e
#define VMCS_GUEST_INTR_STATUS 0x0810
#define VMCS_GUEST_PML_INDEX 0x0812
#define VMCS_HOST_ES_SEL 0x0c00
#define VMCS_HOST_CS_SEL 0x0c02
#define VMCS_HOST_SS_SEL 0x0c04
#define VMCS_HOST_DS_SEL 0x0c06
#define VMCS_HOST_FS_SEL 0x0c08
#define VMCS_HOST_GS_SEL 0x0c0a
#define VMCS_HOST_TR_SEL 0x0c0c

#define VMCS_IO_BITMAP_A 0x2000
#define VMCS_IO_BITMAP_B 0x2002
#define VMCS_MSR_BITMAP 0x2004
#define VMCS_EXIT_MSR_STORE_ADDR 0x2006
#define VMCS_EXIT_MSR_LOAD_ADDR 0x2008
#define VMCS_ENTRY_MSR_LOAD_ADDR 0x200a
#define VMCS_PML_ADDRESS 0x200e
#define VMCS_TSC_OFFSET 0x2010
#define VMCS_VIRTUAL_APIC_ADDR 0x2012
#define VMCS_APIC_ACCESS_ADDR 0x2014
#define VMCS_POSTED_INTR_DESC 0x2016
#define VMCS_VMFUNC_CONTROLS 0x2018
#define VMCS_EPT_POINTER 0x201a
#define VMCS_EOI_EXIT0 0x201c
#define VMCS_EOI_EXIT1 0x201e
#define VMCS_EOI_EXIT2 0x2020
#define VMCS_EOI_EXIT3 0x2022
#define VMCS_EPTP_LIST 0x2024
#define VMCS_VMREAD_BITMAP 0x2026
#define VMCS_VMWRITE_BITMAP 0x2028
#define VMCS_XSS_EXIT_BITMAP 0x202c
#define VMCS_TSC_MULTIPLIER 0x2032
#define VMCS_TERTIARY_CONTROLS 0x2034
#define VMCS_PID_POINTER_TABLE 0x2042
#define VMCS_GUEST_PHYS_ADDR 0x2400
#define VMCS_LINK_POINTER 0x2800
#define VMCS_GUEST_DEBUGCTL 0x2802
#define VMCS_GUEST_PAT 0x2804
#define VMCS_GUEST_EFER 0x2806
#define VMCS_GUEST_PERF_GLOBAL_CTRL 0x2808
#define VMCS_GUEST_PDPTE0 0x280a
#define VMCS_GUEST_PDPTE1 0x280c
#define VMCS_GUEST_PDPTE2 0x280e
#define VMCS_GUEST_PDPTE3 0x2810
#define VMCS_GUEST_BNDCFGS 0x2812
#define VMCS_GUEST_RTIT_CTL 0x2814
#define VMCS_HOST_PAT 0x2c00
#define VMCS_HOST_EFER 0x2c02
#define VMCS_HOST_PERF_GLOBAL_CTRL 0x2c04

#define VMCS_PIN_CONTROLS 0x4000
#define VMCS_PROC_CONTROLS 0x4002
#define VMCS_EXCEPTION_BITMAP 0x4004
#define VMCS_PF_ERROR_MASK 0x4006
#define VMCS_PF_ERROR_MATCH 0x4008
#define VMCS_CR3_TARGET_COUNT 0x400a
#define VMCS_EXIT_CONTROLS 0x400c
#define VMCS_EXIT_MSR_STORE_COUNT 0x400e
#define VMCS_EXIT_MSR_LOAD_COUNT 0x4010
#define VMCS_ENTRY_CONTROLS 0x4012
#define VMCS_ENTRY_MSR_LOAD_COUNT 0x4014
#define VMCS_ENTRY_INTR_INFO 0x4016
#define VMCS_ENTRY_EXCEPTION_ERROR 0x4018
#define VMCS_ENTRY_INSTR_LEN 0x401a
#define VMCS_TPR_THRESHOLD 0x401c
#define VMCS_PROC_CONTROLS2 0x401e
#define VMCS_PLE_GAP 0x4020
#define VMCS_PLE_WINDOW 0x4022
#define VMCS_VM_INSTR_ERROR 0x4400
#define VMCS_EXIT_REASON 0x4402
#define VMCS_EXIT_INTR_INFO 0x4404
#define VMCS_EXIT_INTR_ERROR 0x4406
#define VMCS_IDT_VECTORING_INFO 0x4408
#define VMCS_IDT_VECTORING_ERROR 0x440a
#define VMCS_EXIT_INSTR_LEN 0x440c
#define VMCS_EXIT_INSTR_INFO 0x440e
#define VMCS_GUEST_ES_LIMIT 0x4800
#define VMCS_GUEST_CS_LIMIT 0x4802
#define VMCS_GUEST_SS_LIMIT 0x4804
#define VMCS_GUEST_DS_LIMIT 0x4806
#define VMCS_GUEST_FS_LIMIT 0x4808
#define VMCS_GUEST_GS_LIMIT 0x480a
#define VMCS_GUEST_LDTR_LIMIT 0x480c
#define VMCS_GUEST_TR_LIMIT 0x480e
#define VMCS_GUEST_GDTR_LIMIT 0x4810
#define VMCS_GUEST_IDTR_LIMIT 0x4812
#define VMCS_GUEST_ES_AR 0x4814
#define VMCS_GUEST_CS_AR 0x4816
#define VMCS_GUEST_SS_AR 0x4818
#define VMCS_GUEST_DS_AR 0x481a
#define VMCS_GUEST_FS_AR 0x481c
#define VMCS_GUEST_GS_AR 0x481e
#define VMCS_GUEST_LDTR_AR 0x4820
#define VMCS_GUEST_TR_AR 0x4822
#define VMCS_GUEST_INTERRUPTIBILITY 0x4824
#define VMCS_GUEST_ACTIVITY_STATE 0x4826
#define VMCS_GUEST_SMBASE 0x4828
#define VMCS_GUEST_SYSENTER_CS 0x482a
#define VMCS_PREEMPTION_TIMER 0x482e
#define VMCS_HOST_SYSENTER_CS 0x4c00

#define VMCS_CR0_MASK 0x6000
#define VMCS_CR4_MASK 0x6002
#define VMCS_CR0_SHADOW 0x6004
#define VMCS_CR4_SHADOW 0x6006
#define VMCS_EXIT_QUALIFICATION 0x6400
#define VMCS_GUEST_LINEAR_ADDR 0x640a
#define VMCS_GUEST_CR0 0x6800
#define VMCS_GUEST_CR3 0x6802
#define VMCS_GUEST_CR4 0x6804
#define VMCS_GUEST_ES_BASE 0x6806
#define VMCS_GUEST_CS_BASE 0x6808
#define VMCS_GUEST_SS_BASE 0x680a
#define VMCS_GUEST_DS_BASE 0x680c
#define VMCS_GUEST_FS_BASE 0x680e
#define VMCS_GUEST_GS_BASE 0x6810
#define VMCS_GUEST_LDTR_BASE 0x6812
#define VMCS_GUEST_TR_BASE 0x6814
#define VMCS_GUEST_GDTR_BASE 0x6816
#define VMCS_GUEST_IDTR_BASE 0x6818
#define VMCS_GUEST_DR7 0x681a
#define VMCS_GUEST_RSP 0x681c
#define VMCS_GUEST_RIP 0x681e
#define VMCS_GUEST_RFLAGS 0x6820
#define VMCS_GUEST_PENDING_DBG 0x6822
#define VMCS_GUEST_SYSENTER_ESP 0x6824
#define VMCS_GUEST_SYSENTER_EIP 0x6826
#define VMCS_HOST_CR0 0x6c00
#define VMCS_HOST_CR3 0x6c02
#define VMCS_HOST_CR4 0x6c04
#define VMCS_HOST_FS_BASE 0x6c06
#define VMCS_HOST_GS_BASE 0x6c08
#define VMCS_HOST_TR_BASE 0x6c0a
#define VMCS_HOST_GDTR_BASE 0x6c0c
#define VMCS_HOST_IDTR_BASE 0x6c0e
#define VMCS_HOST_SYSENTER_ESP 0x6c10
#define VMCS_HOST_SYSENTER_EIP 0x6c12
#define VMCS_HOST_RSP 0x6c14
#define VMCS_HOST_RIP 0x6c16

// Pin-based controls (SDM 325462-093 Vol. 3C, Table 27-5).
#define PIN_EXT_INTR_EXITING (1u << 0)
#define PIN_NMI_EXITING (1u << 3)
#define PIN_VIRTUAL_NMIS (1u << 5)
#define PIN_PREEMPTION_TIMER (1u << 6)
#define PIN_POSTED_INTR (1u << 7)

// Primary processor-based controls (SDM 325462-093 Vol. 3C, Table 27-6).
#define PROC_INTR_WINDOW_EXITING (1u << 2)
#define PROC_TSC_OFFSETTING (1u << 3)
#define PROC_HLT_EXITING (1u << 7)
#define PROC_INVLPG_EXITING (1u << 9)
#define PROC_MWAIT_EXITING (1u << 10)
#define PROC_RDPMC_EXITING (1u << 11)
#define PROC_RDTSC_EXITING (1u << 12)
#define PROC_CR3_LOAD_EXITING (1u << 15)
#define PROC_CR3_STORE_EXITING (1u << 16)
#define PROC_TERTIARY_CONTROLS (1u << 17)
#define PROC_CR8_LOAD_EXITING (1u << 19)
#define PROC_CR8_STORE_EXITING (1u << 20)
#define PROC_TPR_SHADOW (1u << 21)
#define PROC_NMI_WINDOW_EXITING (1u << 22)
#define PROC_MOV_DR_EXITING (1u << 23)
#define PROC_UNCOND_IO_EXITING (1u << 24)
#define PROC_IO_BITMAPS (1u << 25)
#define PROC_MONITOR_TRAP_FLAG (1u << 27)
#define PROC_MSR_BITMAPS (1u << 28)
#define PROC_MONITOR_EXITING (1u << 29)
#define PROC_PAUSE_EXITING (1u << 30)
#define PROC_SECONDARY_CONTROLS (1u << 31)

// Secondary processor-based controls (SDM 325462-093 Vol. 3C, Table 27-7).
#define PROC2_VIRT_APIC_ACCESSES (1u << 0)
#define PROC2_EPT (1u << 1)
#define PROC2_DESC_TABLE_EXITING (1u << 2)
#define PROC2_RDTSCP (1u << 3)
#define PROC2_VIRT_X2APIC (1u << 4)
#define PROC2_VPID (1u << 5)
#define PROC2_WBINVD_EXITING (1u << 6)
#define PROC2_UNRESTRICTED_GUEST (1u << 7)
#define PROC2_APIC_REG_VIRT (1u << 8)
#define PROC2_VIRT_INTR_DELIVERY (1u << 9)
#define PROC2_PAUSE_LOOP_EXITING (1u << 10)
#define PROC2_RDRAND_EXITING (1u << 11)
#define PROC2_INVPCID (1u << 12)
#define PROC2_VMFUNC (1u << 13)
#define PROC2_VMCS_SHADOWING (1u << 14)
#define PROC2_RDSEED_EXITING (1u << 16)
#define PROC2_PML (1u << 17)
#define PROC2_EPT_VE (1u << 18)
#define PROC2_CONCEAL_VMX_FROM_PT (1u << 19)
#define PROC2_XSAVES (1u << 20)
#define PROC2_MBEC (1u << 22)
#define PROC2_PT_USE_GPA (1u << 24)
#define PROC2_TSC_SCALING (1u << 25)
#define PROC2_USER_WAIT_PAUSE (1u << 26)

// Tertiary processor-based VM-execution controls (SDM 325462-093 Vol. 3C,
// Table 27-8).
#define PROC3_IPI_VIRT (1ull << 4)

// Primary VM-exit controls (SDM 325462-093 Vol. 3C, Table 27-14).
#define EXIT_SAVE_DEBUG_CONTROLS (1u << 2)
#define EXIT_HOST_ADDR_SPACE_SIZE (1u << 9)
#define EXIT_LOAD_PERF_GLOBAL_CTRL (1u << 12)
#define EXIT_ACK_INTR_ON_EXIT (1u << 15)
#define EXIT_SAVE_PAT (1u << 18)
#define EXIT_LOAD_PAT (1u << 19)
#define EXIT_SAVE_EFER (1u << 20)
#define EXIT_LOAD_EFER (1u << 21)
#define EXIT_SAVE_PREEMPTION_TIMER (1u << 22)
#define EXIT_CLEAR_BNDCFGS (1u << 23)
#define EXIT_CONCEAL_VMX_FROM_PT (1u << 24)
#define EXIT_CLEAR_RTIT_CTL (1u << 25)
#define EXIT_SAVE_PERF_GLOBAL_CTRL (1u << 30)

// VM-entry controls (SDM 325462-093 Vol. 3C, Table 27-17).
#define ENTRY_LOAD_DEBUG_CONTROLS (1u << 2)
#define ENTRY_IA32E_MODE_GUEST (1u << 9)
#define ENTRY_SMM (1u << 10)
#define ENTRY_LOAD_PERF_GLOBAL_CTRL (1u << 13)
#define ENTRY_LOAD_PAT (1u << 14)
#define ENTRY_LOAD_EFER (1u << 15)
#define ENTRY_LOAD_BNDCFGS (1u << 16)
#define ENTRY_CONCEAL_VMX_FROM_PT (1u << 17)
#define ENTRY_LOAD_RTIT_CTL (1u << 18)

// Guest segment access rights (SDM 325462-093 Vol. 3C, Table 27-2).
#define VMX_AR_L (1u << 13)

// Basic exit reasons (SDM 325462-093 Vol. 3D, Appendix C).
#define EXIT_REASON_EXCEPTION_NMI 0
#define EXIT_REASON_EXTERNAL_INTR 1
#define EXIT_REASON_TRIPLE_FAULT 2
#define EXIT_REASON_INIT 3
#define EXIT_REASON_SIPI 4
#define EXIT_REASON_IO_SMI 5
#define EXIT_REASON_OTHER_SMI 6
#define EXIT_REASON_INTR_WINDOW 7
#define EXIT_REASON_NMI_WINDOW 8
#define EXIT_REASON_TASK_SWITCH 9
#define EXIT_REASON_CPUID 10
#define EXIT_REASON_GETSEC 11
#define EXIT_REASON_HLT 12
#define EXIT_REASON_INVD 13
#define EXIT_REASON_INVLPG 14
#define EXIT_REASON_RDPMC 15
#define EXIT_REASON_RDTSC 16
#define EXIT_REASON_RSM 17
#define EXIT_REASON_VMCALL 18
#define EXIT_REASON_VMCLEAR 19
#define EXIT_REASON_VMLAUNCH 20
#define EXIT_REASON_VMPTRLD 21
#define EXIT_REASON_VMPTRST 22
#define EXIT_REASON_VMREAD 23
#define EXIT_REASON_VMRESUME 24
#define EXIT_REASON_VMWRITE 25
#define EXIT_REASON_VMXOFF 26
#define EXIT_REASON_VMXON 27
#define EXIT_REASON_CR_ACCESS 28
#define EXIT_REASON_DR_ACCESS 29
#define EXIT_REASON_IO 30
#define EXIT_REASON_RDMSR 31
#define EXIT_REASON_WRMSR 32
#define EXIT_REASON_INVALID_GUEST_STATE 33
#define EXIT_REASON_MSR_LOADING 34
#define EXIT_REASON_MWAIT 36
#define EXIT_REASON_MONITOR_TRAP 37
#define EXIT_REASON_MONITOR 39
#define EXIT_REASON_PAUSE 40
#define EXIT_REASON_MACHINE_CHECK 41
#define EXIT_REASON_TPR_BELOW_THRESHOLD 43
#define EXIT_REASON_APIC_ACCESS 44
#define EXIT_REASON_VIRTUALIZED_EOI 45
#define EXIT_REASON_GDTR_IDTR 46
#define EXIT_REASON_LDTR_TR 47
#define EXIT_REASON_EPT_VIOLATION 48
#define EXIT_REASON_EPT_MISCONFIG 49
#define EXIT_REASON_INVEPT 50
#define EXIT_REASON_RDTSCP 51
#define EXIT_REASON_PREEMPTION_TIMER 52
#define EXIT_REASON_INVVPID 53
#define EXIT_REASON_WBINVD 54
#define EXIT_REASON_XSETBV 55
#define EXIT_REASON_APIC_WRITE 56
#define EXIT_REASON_RDRAND 57
#define EXIT_REASON_INVPCID 58
#define EXIT_REASON_VMFUNC 59
#define EXIT_REASON_RDSEED 61
#define EXIT_REASON_PML_FULL 62
#define EXIT_REASON_XSAVES 63
#define EXIT_REASON_XRSTORS 64

#define EXIT_REASON_ENTRY_FAILURE (1u << 31)

// EPT violation exit qualification (SDM 325462-093 Vol. 3C, Table 30-7): the
// access, the rights the EPT entries gave (bits 6:3), whether the guest-linear
// address is known and was translated rather than a paging-structure entry's,
// what paging gave the linear address, IRET's NMI unblocking, and whether the
// processor made the access outside of any instruction.
#define EPT_QUAL_READ (1ull << 0)
#define EPT_QUAL_WRITE (1ull << 1)
#define EPT_QUAL_FETCH (1ull << 2)
#define EPT_QUAL_RIGHTS_SHIFT 3
#define EPT_QUAL_LINEAR (1ull << 7)
#define EPT_QUAL_TRANSLATION (1ull << 8)
#define EPT_QUAL_USER (1ull << 9)
#define EPT_QUAL_WRITABLE (1ull << 10)
#define EPT_QUAL_XD (1ull << 11)
#define EPT_QUAL_NMI_UNBLOCKED (1ull << 12)
#define EPT_QUAL_ASYNC (1ull << 16)

// APIC-access exit qualification: the page offset, and the access type (SDM
// 325462-093 Vol. 3C, Table 30-6).
#define APIC_ACCESS_OFFSET_MASK 0xfffull
#define APIC_ACCESS_TYPE(qual) (((qual) >> 12) & 0xf)
#define APIC_ACCESS_DATA_READ 0
#define APIC_ACCESS_DATA_WRITE 1
#define APIC_ACCESS_LINEAR_ASYNC 4
#define APIC_ACCESS_PHYSICAL_ASYNC 11

// Event injection and exit interruption information fields.
#define INTR_INFO_VALID (1u << 31)
#define INTR_INFO_ERROR_CODE (1u << 11)
#define INTR_TYPE_EXT_INTR (0u << 8)
#define INTR_TYPE_NMI (2u << 8)
#define INTR_TYPE_HW_EXCEPTION (3u << 8)
#define INTR_TYPE_SW_INTR (4u << 8)
#define INTR_TYPE_PRIV_SW_EXCEPTION (5u << 8)
#define INTR_TYPE_SW_EXCEPTION (6u << 8)

// Guest interruptibility state.
#define INTERRUPTIBILITY_STI (1u << 0)
#define INTERRUPTIBILITY_MOV_SS (1u << 1)
#define INTERRUPTIBILITY_SMI (1u << 2)
#define INTERRUPTIBILITY_NMI (1u << 3)

// Guest pending debug exceptions.
#define PENDING_DBG_ENABLED (1ull << 12)
#define PENDING_DBG_BS (1ull << 14)

// Guest activity states.
#define ACTIVITY_ACTIVE 0
#define ACTIVITY_HLT 1
#define ACTIVITY_SHUTDOWN 2
#define ACTIVITY_WAIT_SIPI 3

// Capability MSRs (SDM 325462-093 Vol. 3D, Appendix A).
#define MSR_IA32_VMX_BASIC 0x480
#define MSR_IA32_VMX_PINBASED_CTLS 0x481
#define MSR_IA32_VMX_PROCBASED_CTLS 0x482
#define MSR_IA32_VMX_EXIT_CTLS 0x483
#define MSR_IA32_VMX_ENTRY_CTLS 0x484
#define MSR_IA32_VMX_MISC 0x485
#define MSR_IA32_VMX_CR0_FIXED0 0x486
#define MSR_IA32_VMX_CR0_FIXED1 0x487
#define MSR_IA32_VMX_CR4_FIXED0 0x488
#define MSR_IA32_VMX_CR4_FIXED1 0x489
#define MSR_IA32_VMX_VMCS_ENUM 0x48a
#define MSR_IA32_VMX_PROCBASED_CTLS2 0x48b
#define MSR_IA32_VMX_EPT_VPID_CAP 0x48c
#define MSR_IA32_VMX_TRUE_PINBASED_CTLS 0x48d
#define MSR_IA32_VMX_TRUE_PROCBASED_CTLS 0x48e
#define MSR_IA32_VMX_TRUE_EXIT_CTLS 0x48f
#define MSR_IA32_VMX_TRUE_ENTRY_CTLS 0x490
#define MSR_IA32_VMX_VMFUNC 0x491
#define MSR_IA32_VMX_PROCBASED_CTLS3 0x492

// IA32_VMX_MISC: Intel PT can run in VMX operation (SDM 325462-093 Vol. 3D,
// A.6).
#define VMX_MISC_PT (1ull << 14)

#define FEATURE_CONTROL_LOCK (1u << 0)
#define FEATURE_CONTROL_VMX_OUTSIDE_SMX (1u << 2)

struct vcpu;
struct vmcs_cache;

// A VMCS of the vCPU's but its current one, as the vCPU keeps that.
struct vmx_vmcs {
    void *vmcs;
    uint64_t pa;
    bool launched;
    struct vmcs_cache *cache;
};

bool vmx_supported(void);
void vmx_cpu_init(void);
void vmx_vcpu_init(struct vcpu *v);
__attribute__((noreturn)) void vmx_run(struct vcpu *v);
// Whether the guest's VMCS gets the secondary processor-based control.
bool vmx_secondary_allowed(uint32_t control);
// Whether the guest's VMCS gets the pin-based control.
bool vmx_pin_allowed(uint32_t control);
// Whether it gets the tertiary processor-based control.
bool vmx_tertiary_allowed(uint64_t control);
// A VMX capability MSR, the TRUE ones as the processor's controls are where it
// has none.
uint64_t vmx_capability(uint32_t msr);
// Another VMCS for the vCPU, with the hypervisor's host state alone.
void vmx_vmcs_create(struct vcpu *v, struct vmx_vmcs *vmcs);
// A shadow VMCS (SDM 325462-093 Vol. 3C, 27.10), cleared: its address.
uint64_t vmx_shadow_vmcs_create(void);
// Accesses to a VMCS other than the vCPU's, which is current again after.
void vmx_other_vmcs(struct vcpu *v, uint64_t pa);
uint64_t vmx_other_read(uint32_t field);
void vmx_other_write(uint32_t field, uint64_t value);
void vmx_own_vmcs(struct vcpu *v);
// Makes another VMCS the vCPU's current one, the current one going into it.
void vmx_vmcs_swap(struct vcpu *v, struct vmx_vmcs *other);

uint64_t vmx_read(uint32_t field);

// The EPT the vCPU's current VMCS takes: the hypervisor's, or the shadow of
// the guest's EPT that runs its guest's guest.
void vmx_load_gpt_root(struct vcpu *v);
// INVEPT of every EPTP, and INVVPID of the type, or of all VPIDs but 0 where
// the processor lacks that type.
void vmx_invept_all(void);
void vmx_invvpid(unsigned type, uint16_t vpid, uint64_t address);
// Whether the hypervisor tags the guest's translations with VPIDs.
bool vmx_vpid_usable(void);
// Whether the guest gets Intel PT: its output through EPT, its tracing stopped
// at exits and resumed at entries.
bool vmx_pt_passthrough(void);
bool vmx_mpx_passthrough(void);
// Whether the guest's performance counters count only while it runs: exits
// clear IA32_PERF_GLOBAL_CTRL and entries load the guest's, which exits save
// where vmx_perf_global_saved().
bool vmx_perf_global_switched(void);
bool vmx_perf_global_saved(void);
// An access the processor made for the guest outside of any instruction that
// reached gpa, where the guest has no RAM.
void vmx_async_access(uint64_t gpa);
// Loads the guest's CR3 as a MOV to CR3 would, for what the hypervisor carries
// out for the guest: the PDPTEs of PAE paging, and the guest's translations.
void vmx_load_guest_cr3(uint64_t cr3);
// PAE paging's PDPTEs from what the guest's CR3 points to, as VM entries load
// them without EPT.
void vmx_load_guest_pdptes(void);
// What a task switch does besides loading the new task: CR0.TS, DR7's local
// enables, and, for an NMI delivered through a task gate, blocking NMIs.
void vmx_task_switched(struct vcpu *v, bool nmi);
void vmx_write(uint32_t field, uint64_t value);

#endif
