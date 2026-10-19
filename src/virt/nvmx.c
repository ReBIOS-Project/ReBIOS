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

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <memory.h>
#include <core.h>
#include <handoff.h>
#include <lib/heap.h>
#include <lib/log.h>
#include <sys/cpu.h>
#include <sys/mm.h>
#include <sys/smp.h>
#include <sys/x86.h>
#include <virt/event.h>
#include <virt/exit.h>
#include <virt/gpt.h>
#include <virt/irq.h>
#include <virt/mmio.h>
#include <virt/nvmx.h>
#include <virt/vcpu.h>
#include <virt/virt.h>
#include <virt/vlapic.h>
#include <virt/vmx.h>

// The guest's VMCS regions hold the revision identifier and the VMX-abort
// indicator where the processor's do (SDM 325462-093 Vol. 3C, 27.2), then
// ReBIOS's own: the launch state, and the fields in the order of the table
// below. Software is not to read or write them otherwise.
#define VMCS12_REVISION 0x52424e56u
#define VMCS12_SHADOW (1u << 31)
#define VMCS12_LAUNCHED 8
#define VMCS12_FIELDS 16

// VM-instruction error numbers (SDM 325462-093 Vol. 3C, Table 33-1).
#define ERROR_VMCLEAR_ADDRESS 2
#define ERROR_VMCLEAR_VMXON 3
#define ERROR_VMLAUNCH_NOT_CLEAR 4
#define ERROR_VMRESUME_NOT_LAUNCHED 5
#define ERROR_ENTRY_CONTROLS 7
#define ERROR_VMPTRLD_ADDRESS 9
#define ERROR_VMPTRLD_VMXON 10
#define ERROR_VMPTRLD_REVISION 11
#define ERROR_UNSUPPORTED_FIELD 12
#define ERROR_ENTRY_HOST_STATE 8
#define ERROR_VMXON_IN_ROOT 15
#define ERROR_ENTRY_MOV_SS 26
#define ERROR_INVALID_OPERAND 28

// Qualifications of entries failing on the guest state (SDM 325462-093 Vol.
// 3C, 29.8): the default, and the VMCS link pointer.
#define QUAL_GUEST_STATE 0
#define QUAL_LINK_POINTER 4

#define MSR_LIST_MAX 512
#define MSR_IA32_TSC 0x10
#define MSR_IA32_SYSENTER_CS 0x174
#define MSR_IA32_SYSENTER_ESP 0x175
#define MSR_IA32_SYSENTER_EIP 0x176
#define MSR_IA32_DEBUGCTL 0x1d9
#define MSR_IA32_PAT 0x277
#define MSR_IA32_SMM_MONITOR_CTL 0x9b
#define MSR_FS_BASE 0xc0000100
#define MSR_GS_BASE 0xc0000101
#define MSR_X2APIC_FIRST 0x800
#define MSR_X2APIC_LAST 0x8ff
#define MSR_X2APIC_TPR 0x808
#define MSR_X2APIC_EOI 0x80b
#define MSR_X2APIC_SELF_IPI 0x83f

#define VMX_RESULT_FLAGS (RFLAGS_CF | RFLAGS_PF | RFLAGS_AF | RFLAGS_ZF | RFLAGS_SF | RFLAGS_OF)

#define NO_VMCS (~0ull)

// Field encodings: access type, index, type, and width (SDM 325462-093 Vol.
// 3C, 27.11.2).
#define FIELD_HIGH 1u
#define FIELD_INDEX(field) ((field) >> 1 & 0x1ff)
#define FIELD_TYPE(field) ((field) >> 10 & 3)
#define FIELD_WIDTH(field) ((field) >> 13 & 3)
#define FIELD_RESERVED 0xffff9000u
#define FIELD_TYPE_EXIT_INFO 1
#define FIELD_TYPE_GUEST 2
#define FIELD_WIDTH_16 0
#define FIELD_WIDTH_64 1
#define FIELD_WIDTH_32 2

#define VMCS_CR3_TARGET0 0x6008

// The access-rights bits that are not reserved (SDM 325462-093 Vol. 3C, Table
// 27-2).
#define AR_WRITABLE 0x1f0ffull

// The fields the guest's VMCSs have: the controls of what it is offered, and
// all of the guest state, host state, and exit information.
static const uint16_t fields[] = {
    0x0000, 0x0002,
    0x0800, 0x0802, 0x0804, 0x0806, 0x0808, 0x080a, 0x080c, 0x080e, 0x0810, 0x0812,
    0x0c00, 0x0c02, 0x0c04, 0x0c06, 0x0c08, 0x0c0a, 0x0c0c,
    0x2000, 0x2002, 0x2004, 0x2006, 0x2008, 0x200a, 0x200e, 0x2010, 0x2012, 0x2014, 0x2016,
    0x2018, 0x201a, 0x202c, 0x2032,
    0x201c, 0x201e, 0x2020, 0x2022, 0x2024, 0x2026, 0x2028,
    0x2400,
    0x2800, 0x2802, 0x2804, 0x2806, 0x280a, 0x280c, 0x280e, 0x2810,
    0x2c00, 0x2c02,
    0x4000, 0x4002, 0x4004, 0x4006, 0x4008, 0x400a, 0x400c, 0x400e, 0x4010, 0x4012, 0x4014,
    0x4016, 0x4018, 0x401a, 0x401c, 0x401e, 0x4020, 0x4022,
    0x4400, 0x4402, 0x4404, 0x4406, 0x4408, 0x440a, 0x440c, 0x440e,
    0x4800, 0x4802, 0x4804, 0x4806, 0x4808, 0x480a, 0x480c, 0x480e, 0x4810, 0x4812,
    0x4814, 0x4816, 0x4818, 0x481a, 0x481c, 0x481e, 0x4820, 0x4822,
    0x4824, 0x4826, 0x4828, 0x482a, 0x482e,
    0x4c00,
    0x6000, 0x6002, 0x6004, 0x6006, 0x6008, 0x600a, 0x600c, 0x600e,
    0x6400, 0x6402, 0x6404, 0x6406, 0x6408, 0x640a,
    0x6800, 0x6802, 0x6804, 0x6806, 0x6808, 0x680a, 0x680c, 0x680e, 0x6810, 0x6812, 0x6814,
    0x6816, 0x6818, 0x681a, 0x681c, 0x681e, 0x6820, 0x6822, 0x6824, 0x6826,
    0x6c00, 0x6c02, 0x6c04, 0x6c06, 0x6c08, 0x6c0a, 0x6c0c, 0x6c0e, 0x6c10, 0x6c12, 0x6c14, 0x6c16,
};

#define FIELD_COUNT (sizeof(fields) / sizeof(fields[0]))
#define MAX_INDEX 64

_Static_assert(VMCS12_FIELDS + FIELD_COUNT * 8 <= 4096, "VMCS12 layout");

// By width, type, and index: the slot plus one, or 0 where there is no such
// field.
static uint8_t field_slots[4][4][MAX_INDEX];

// A shadow of one of the guest's EPTs: what it and the hypervisor's EPT make
// of the guest's guest's addresses, filled in as the processor misses them.
struct ept02 {
    uint64_t eptp12;
    uint64_t *root;
};

#define EPT02_ROOTS 4

// VMCS01's VPID is 1. VPIDs tag translations per logical processor, so every
// vCPU's VMCS02 may share one.
#define VPID02 2

// VMX as the guest gets it.
struct nvmx {
    bool vmxon;
    uint64_t vmxon_ptr;
    uint64_t current;
    uint8_t *vmcs12;
    // Whether the current VMCS is a shadow VMCS, which no entry takes.
    bool current_shadow;
    // VMCS02, which runs the guest's guest, while VMCS01 is the vCPU's, and
    // VMCS01 while VMCS02 is.
    struct vmx_vmcs other;
    bool created;
    bool in_l2;
    // Whether the entry under way is a VMLAUNCH, which only launches the VMCS
    // once it gets past the guest state.
    bool launching;
    // Whether the guest's VMLAUNCH or VMRESUME waits for the entry into
    // VMCS02.
    bool run_pending;
    // The shadow VMCS that serves the guest's VMREADs and VMWRITEs.
    uint64_t shadow;
    // VMCS02's VM-exit MSR-store list: IA32_TSC, while the guest's own list has
    // it.
    uint64_t tsc_store[2] __attribute__((aligned(16)));
    bool tsc_stored;
    // While the guest's guest runs with the VMX-preemption timer: the TSC,
    // shifted by the timer's rate, at which it expires.
    uint64_t timer_deadline;
    // The guest's MSR bitmap with the hypervisor's intercepts added.
    uint8_t *msr_bitmap;
    // The guest's virtual-APIC page and posted-interrupt descriptor for its
    // guest, the APIC-access address VMCS02 has for it, and whether EPT02
    // maps the page behind the local APICs' pages, which only that address
    // then gives a meaning.
    uint8_t *vapic;
    uint32_t *pid;
    uint64_t apic_access02;
    bool ept02_apic;
    // While the guest's guest processes posted interrupts, their notification
    // vector, else -1, and whether a notification came for the hypervisor to
    // process.
    int pi_vector;
    bool pi_pending;
    // The activity state the guest's guest waits in here, and the vector of
    // the start-up IPI that ends the wait for one, else -1.
    int inactive;
    int sipi;
    struct ept02 ept02[EPT02_ROOTS];
    struct ept02 *current02;
    unsigned ept02_pages;
    unsigned ept02_next;
    // The guest's VPID whose translations VPID02 holds.
    uint16_t vpid12;
    // An exit of the guest's guest that what the hypervisor carried out for it
    // raised, for the next entry.
    bool pending;
    uint32_t pending_reason;
    uint32_t pending_info;
    uint32_t pending_error;
    uint64_t pending_qual;
    uint64_t pending_gpa;
    uint64_t pending_linear;
};

bool nvmx_enabled;

// The guest's VMX capability MSRs: the processor's allowed settings where
// ReBIOS emulates them. Their allowed 0-settings are the processor's, so that
// the guest's controls are the processor's to take.
static uint64_t cap_basic;
static uint64_t cap_pin, cap_proc, cap_exit, cap_entry;
static uint64_t cap_true_pin, cap_true_proc, cap_true_exit, cap_true_entry;
static uint64_t cap_proc2, cap_misc;
static uint64_t cap_cr0_fixed0, cap_cr0_fixed1, cap_cr4_fixed0, cap_cr4_fixed1;
static uint64_t cap_vmcs_enum;
static uint64_t cap_ept_vpid;
static uint64_t cap_vmfunc;
// The CR3-target values the processor has, of the four the guest may get.
static unsigned cr3_targets;
// The VMX-preemption timer counts TSC >> timer_rate.
static unsigned timer_rate;
// Whether VMCS shadowing serves the guest, and its bitmaps.
static bool shadowing;
static uint8_t *vmread_bitmap;
static uint8_t *vmwrite_bitmap;
// MAXPHYADDR (SDM 325462-093 Vol. 3C, 31.3.1).
static unsigned phys_width;
// An APIC-access address nothing maps, for where the guest's maps to nothing.
static uint64_t access_nowhere;
// The largest pages EPT02 maps: 2 MiB, or 1 GiB.
static int ept02_level_max;
// The EPT02 tables each vCPU may have, of the heap's NESTED_HEAP_SIZE.
static unsigned ept02_budget;

// What the default1 classes of the controls the old capability MSRs report
// as fixed (SDM 325462-093 Vol. 3D, A.2).
#define PIN_DEFAULT1 0x00000016u
#define PROC_DEFAULT1 0x0401e172u
#define EXIT_DEFAULT1 0x00036dffu
#define ENTRY_DEFAULT1 0x000011ffu

#define PIN_EMULATED \
    (PIN_EXT_INTR_EXITING | PIN_NMI_EXITING | PIN_VIRTUAL_NMIS | PIN_PREEMPTION_TIMER \
     | PIN_POSTED_INTR | PIN_DEFAULT1)
#define PROC_EMULATED                                                                         \
    (PROC_INTR_WINDOW_EXITING | PROC_TSC_OFFSETTING | PROC_HLT_EXITING | PROC_INVLPG_EXITING \
     | PROC_MWAIT_EXITING | PROC_RDPMC_EXITING | PROC_RDTSC_EXITING | PROC_CR3_LOAD_EXITING   \
     | PROC_CR3_STORE_EXITING | PROC_CR8_LOAD_EXITING | PROC_CR8_STORE_EXITING                 \
     | PROC_TPR_SHADOW | PROC_NMI_WINDOW_EXITING | PROC_MOV_DR_EXITING | PROC_UNCOND_IO_EXITING \
     | PROC_IO_BITMAPS | PROC_MONITOR_TRAP_FLAG | PROC_MSR_BITMAPS | PROC_MONITOR_EXITING        \
     | PROC_PAUSE_EXITING | PROC_SECONDARY_CONTROLS | PROC_DEFAULT1)
#define PROC2_EMULATED                                                                         \
    (PROC2_VIRT_APIC_ACCESSES | PROC2_EPT | PROC2_DESC_TABLE_EXITING | PROC2_RDTSCP             \
     | PROC2_VIRT_X2APIC | PROC2_VPID | PROC2_WBINVD_EXITING | PROC2_UNRESTRICTED_GUEST         \
     | PROC2_APIC_REG_VIRT | PROC2_VIRT_INTR_DELIVERY | PROC2_PAUSE_LOOP_EXITING               \
     | PROC2_RDRAND_EXITING | PROC2_INVPCID | PROC2_VMFUNC | PROC2_VMCS_SHADOWING              \
     | PROC2_RDSEED_EXITING | PROC2_PML | PROC2_XSAVES | PROC2_MBEC | PROC2_TSC_SCALING          \
     | PROC2_USER_WAIT_PAUSE)
#define EXIT_EMULATED                                                                        \
    (EXIT_SAVE_DEBUG_CONTROLS | EXIT_HOST_ADDR_SPACE_SIZE | EXIT_ACK_INTR_ON_EXIT | EXIT_SAVE_PAT \
     | EXIT_LOAD_PAT | EXIT_SAVE_EFER | EXIT_LOAD_EFER | EXIT_SAVE_PREEMPTION_TIMER \
     | EXIT_DEFAULT1)
#define ENTRY_EMULATED \
    (ENTRY_LOAD_DEBUG_CONTROLS | ENTRY_IA32E_MODE_GUEST | ENTRY_LOAD_PAT | ENTRY_LOAD_EFER \
     | ENTRY_DEFAULT1)

// IA32_VMX_BASIC (SDM 325462-093 Vol. 3D, A.1): the VMCS size, write-back
// memory, the information of INS and OUTS exits, the TRUE capability MSRs,
// and whether exceptions can be injected without their error codes.
#define BASIC_SIZE_SHIFT 32
#define BASIC_MEMTYPE_WB (6ull << 50)
#define BASIC_INS_OUTS (1ull << 54)
#define BASIC_TRUE_CONTROLS (1ull << 55)
#define BASIC_NO_ERROR_CODE (1ull << 56)

// IA32_VMX_MISC (SDM 325462-093 Vol. 3D, A.6): the VMX-preemption timer's
// rate, exits store EFER.LMA in the IA-32e mode guest control, the HLT
// activity state, the CR3-target values, and VMWRITE writes the exit
// information too.
#define MISC_TIMER_RATE 0x1full
#define MISC_STORE_LMA (1ull << 5)
#define MISC_ACTIVITY_HLT (1ull << 6)
#define MISC_ACTIVITY_SHUTDOWN (1ull << 7)
#define MISC_ACTIVITY_WAIT_SIPI (1ull << 8)
#define MISC_PT (1ull << 14)
#define MISC_CR3_TARGETS_SHIFT 16
#define MISC_CR3_TARGETS_MASK 0x1ffull
#define MISC_VMWRITE_ANY (1ull << 29)
#define MISC_ZERO_LENGTH_INJECTION (1ull << 30)

#define FEATURE_CONTROL_GUEST (FEATURE_CONTROL_LOCK | FEATURE_CONTROL_VMX_OUTSIDE_SMX)

// VM function 0 (SDM 325462-093 Vol. 3C, 28.5.7.3) and its EPTP list's size.
#define VMFUNC_EPTP_SWITCHING (1ull << 0)
#define EPTP_LIST_ENTRIES 512

#define CPUID_7_1_EAX_LAM (1u << 26)

// IA32_VMX_EPT_VPID_CAP (SDM 325462-093 Vol. 3D, A.10). The guest does
// without supervisor shadow-stack control and the HLAT prefix size, whose
// controls it is not offered.
#define CAP_EXEC_ONLY (1ull << 0)
#define CAP_WALK_4 (1ull << 6)
#define CAP_WALK_5 (1ull << 7)
#define CAP_UC (1ull << 8)
#define CAP_WB (1ull << 14)
#define CAP_2M (1ull << 16)
#define CAP_1G (1ull << 17)
#define CAP_INVEPT (1ull << 20)
#define CAP_AD (1ull << 21)
#define CAP_ADVANCED_INFO (1ull << 22)
#define CAP_INVEPT_SINGLE (1ull << 25)
#define CAP_INVEPT_ALL (1ull << 26)
#define CAP_INVVPID (1ull << 32)
#define CAP_INVVPID_ADDRESS (1ull << 40)
#define CAP_INVVPID_SINGLE (1ull << 41)
#define CAP_INVVPID_ALL (1ull << 42)
#define CAP_INVVPID_GLOBALS (1ull << 43)
#define CAP_EPT_OFFERED                                                                       \
    (CAP_EXEC_ONLY | CAP_WALK_4 | CAP_WALK_5 | CAP_UC | CAP_WB | CAP_2M | CAP_1G | CAP_INVEPT \
     | CAP_AD | CAP_ADVANCED_INFO | CAP_INVEPT_SINGLE | CAP_INVEPT_ALL)
#define CAP_VPID_OFFERED                                                                      \
    (CAP_INVVPID | CAP_INVVPID_ADDRESS | CAP_INVVPID_SINGLE | CAP_INVVPID_ALL                 \
     | CAP_INVVPID_GLOBALS)

static int field_slot(uint32_t field);

static void shadowing_init(uint64_t misc);
static void shadow_load(struct vcpu *v, struct nvmx *n, bool all);
static void shadow_store(struct vcpu *v, struct nvmx *n);
static void shadow_unload(void);
static void shadow_update(struct vcpu *v, struct nvmx *n, uint32_t field);

// A capability MSR: allowed 0-settings low, allowed 1-settings high.
static uint64_t capability(uint64_t hardware, uint32_t emulated, uint32_t required) {
    uint32_t allowed1 = (uint32_t)(hardware >> 32) & emulated;
    uint32_t allowed0 = ((uint32_t)hardware | required) & allowed1;
    return (uint64_t)allowed1 << 32 | allowed0;
}

void nvmx_init(void) {
    if (!virt_vmx || !core_handoff->config.nested) {
        return;
    }
    nvmx_enabled = true;

    for (unsigned i = 0; i < FIELD_COUNT; i++) {
        uint32_t field = fields[i];
        field_slots[FIELD_WIDTH(field)][FIELD_TYPE(field)][FIELD_INDEX(field)] = (uint8_t)(i + 1);
    }

    uint64_t basic = vmx_capability(MSR_IA32_VMX_BASIC);
    cap_basic = VMCS12_REVISION | 4096ull << BASIC_SIZE_SHIFT | BASIC_MEMTYPE_WB | BASIC_INS_OUTS
                | BASIC_TRUE_CONTROLS | (basic & BASIC_NO_ERROR_CODE);

    cap_true_pin = capability(vmx_capability(MSR_IA32_VMX_TRUE_PINBASED_CTLS), PIN_EMULATED, 0);
    cap_true_proc = capability(vmx_capability(MSR_IA32_VMX_TRUE_PROCBASED_CTLS), PROC_EMULATED, 0);
    cap_true_exit = capability(vmx_capability(MSR_IA32_VMX_TRUE_EXIT_CTLS), EXIT_EMULATED, 0);
    cap_true_entry = capability(vmx_capability(MSR_IA32_VMX_TRUE_ENTRY_CTLS), ENTRY_EMULATED, 0);
    cap_pin = capability(cap_true_pin, PIN_EMULATED, PIN_DEFAULT1);
    cap_proc = capability(cap_true_proc, PROC_EMULATED, PROC_DEFAULT1);
    cap_exit = capability(cap_true_exit, EXIT_EMULATED, EXIT_DEFAULT1);
    cap_entry = capability(cap_true_entry, ENTRY_EMULATED, ENTRY_DEFAULT1);
    cap_proc2 = capability(vmx_capability(MSR_IA32_VMX_PROCBASED_CTLS2), PROC2_EMULATED, 0);
    uint64_t ept_vpid = vmx_capability(MSR_IA32_VMX_EPT_VPID_CAP);
    cap_ept_vpid = ept_vpid & CAP_EPT_OFFERED;
    // VMCS02 needs a VPID of its own for the guest's.
    if (vmx_vpid_usable()) {
        cap_ept_vpid |= ept_vpid & CAP_VPID_OFFERED;
    } else {
        cap_proc2 &= ~((uint64_t)PROC2_VPID << 32);
    }
    // Only the processor's advanced information tells EPT02's fetches from
    // user-mode linear addresses from others.
    if (!(ept_vpid & CAP_ADVANCED_INFO)) {
        cap_proc2 &= ~((uint64_t)PROC2_MBEC << 32);
    }
    // EPTP switching is the hypervisor's to carry out, on EPT02.
    if (cap_proc2 >> 32 & PROC2_EPT) {
        cap_vmfunc = VMFUNC_EPTP_SWITCHING;
    }
    phys_width = cpuid(0x80000008, 0).eax & 0xff;
    if (cap_proc2 >> 32 & PROC2_VIRT_APIC_ACCESSES) {
        access_nowhere = (uint64_t)(uintptr_t)page_alloc(1);
    }
    ept02_level_max = (ept_vpid & CAP_1G) ? 2 : 1;
    ept02_budget = NESTED_HEAP_SIZE / PAGE_SIZE / smp_cpu_count();

    uint64_t misc = vmx_capability(MSR_IA32_VMX_MISC);
    cr3_targets = (unsigned)(misc >> MISC_CR3_TARGETS_SHIFT & MISC_CR3_TARGETS_MASK);
    if (cr3_targets > 4) {
        cr3_targets = 4;
    }
    timer_rate = (unsigned)(misc & MISC_TIMER_RATE);
    cap_misc = timer_rate | MISC_STORE_LMA | MISC_ACTIVITY_HLT | MISC_ACTIVITY_SHUTDOWN
               | MISC_ACTIVITY_WAIT_SIPI | (uint64_t)cr3_targets << MISC_CR3_TARGETS_SHIFT
               | MISC_VMWRITE_ANY | (misc & MISC_ZERO_LENGTH_INJECTION);
    // Its own Intel PT keeps working in VMX operation, paused while its guests
    // run.
    if (vmx_pt_passthrough()) {
        cap_misc |= MISC_PT;
    }

    cap_cr0_fixed0 = vmx_capability(MSR_IA32_VMX_CR0_FIXED0);
    cap_cr0_fixed1 = vmx_capability(MSR_IA32_VMX_CR0_FIXED1);
    cap_cr4_fixed0 = vmx_capability(MSR_IA32_VMX_CR4_FIXED0);
    cap_cr4_fixed1 = vmx_capability(MSR_IA32_VMX_CR4_FIXED1);

    shadowing_init(misc);

    uint32_t max_index = 0;
    for (unsigned i = 0; i < FIELD_COUNT; i++) {
        if (field_slot(fields[i]) >= 0 && FIELD_INDEX(fields[i]) > max_index) {
            max_index = FIELD_INDEX(fields[i]);
        }
    }
    cap_vmcs_enum = max_index << 1;

    virt_intercept_msr(MSR_IA32_FEATURE_CONTROL, true, true);
    for (uint32_t msr = MSR_IA32_VMX_BASIC; msr <= MSR_IA32_VMX_PROCBASED_CTLS3; msr++) {
        virt_intercept_msr(msr, true, true);
    }
    printf("Nested VMX for the guest\n");
}

void nvmx_vcpu_init(struct vcpu *v) {
    if (!nvmx_enabled) {
        return;
    }
    struct nvmx *n = kmalloc(sizeof(*n));
    *n = (struct nvmx){.current = NO_VMCS, .pi_vector = -1, .sipi = -1};
    v->nvmx = n;
    if (shadowing) {
        n->shadow = vmx_shadow_vmcs_create();
        vmx_write(VMCS_VMREAD_BITMAP, (uint64_t)(uintptr_t)vmread_bitmap);
        vmx_write(VMCS_VMWRITE_BITMAP, (uint64_t)(uintptr_t)vmwrite_bitmap);
    }
}

static void ept02_drop_all(struct nvmx *n);

// Fields that exist only where the guest is offered what they are for (SDM
// 325462-093 Vol. 3D, Appendix B).
static bool field_offered(uint32_t field) {
    uint32_t pin = (uint32_t)(cap_true_pin >> 32);
    uint32_t proc = (uint32_t)(cap_true_proc >> 32);
    uint32_t proc2 = (uint32_t)(cap_proc2 >> 32);
    switch (field) {
        case VMCS_PREEMPTION_TIMER: {
            return (pin & PIN_PREEMPTION_TIMER) != 0;
        }
        case VMCS_POSTED_INTR_NV:
        case VMCS_POSTED_INTR_DESC: {
            return (pin & PIN_POSTED_INTR) != 0;
        }
        case VMCS_VIRTUAL_APIC_ADDR:
        case VMCS_TPR_THRESHOLD: {
            return (proc & PROC_TPR_SHADOW) != 0;
        }
        case VMCS_APIC_ACCESS_ADDR: {
            return (proc2 & PROC2_VIRT_APIC_ACCESSES) != 0;
        }
        case VMCS_PML_ADDRESS:
        case VMCS_GUEST_PML_INDEX: {
            return (proc2 & PROC2_PML) != 0;
        }
        case VMCS_XSS_EXIT_BITMAP: {
            return (proc2 & PROC2_XSAVES) != 0;
        }
        case VMCS_VMFUNC_CONTROLS: {
            return (proc2 & PROC2_VMFUNC) != 0;
        }
        case VMCS_VMREAD_BITMAP:
        case VMCS_VMWRITE_BITMAP: {
            return (proc2 & PROC2_VMCS_SHADOWING) != 0;
        }
        case VMCS_EPTP_LIST: {
            return (proc2 & PROC2_VMFUNC) && (cap_vmfunc & VMFUNC_EPTP_SWITCHING);
        }
        case VMCS_TSC_MULTIPLIER: {
            return (proc2 & PROC2_TSC_SCALING) != 0;
        }
        case VMCS_PLE_GAP:
        case VMCS_PLE_WINDOW: {
            return (proc2 & PROC2_PAUSE_LOOP_EXITING) != 0;
        }
        case VMCS_GUEST_INTR_STATUS:
        case VMCS_EOI_EXIT0:
        case VMCS_EOI_EXIT1:
        case VMCS_EOI_EXIT2:
        case VMCS_EOI_EXIT3: {
            return (proc2 & PROC2_VIRT_INTR_DELIVERY) != 0;
        }
        default: {
            return true;
        }
    }
}

static int field_slot(uint32_t field) {
    if (field & FIELD_RESERVED) {
        return -1;
    }
    if ((field & FIELD_HIGH) && FIELD_WIDTH(field) != FIELD_WIDTH_64) {
        return -1;
    }
    if (FIELD_INDEX(field) >= MAX_INDEX) {
        return -1;
    }
    if (field >= VMCS_CR3_TARGET0 && field < VMCS_CR3_TARGET0 + 8
        && (field - VMCS_CR3_TARGET0) / 2 >= cr3_targets) {
        return -1;
    }
    if (!field_offered(field & ~FIELD_HIGH)) {
        return -1;
    }
    return field_slots[FIELD_WIDTH(field)][FIELD_TYPE(field)][FIELD_INDEX(field)] - 1;
}

static uint64_t *field_in(uint8_t *vmcs, int slot) {
    return (uint64_t *)(vmcs + VMCS12_FIELDS + (size_t)slot * 8);
}

static uint64_t *field_at(const struct nvmx *n, int slot) {
    return field_in(n->vmcs12, slot);
}

// The guest's RAM at a guest-physical page, for the regions VMX instructions
// and VMCSs name: not the page behind its local APICs' pages.
static uint8_t *guest_page(uint64_t gpa) {
    uint64_t hpa;
    uint32_t prot;
    uint8_t memtype;
    if (!gpt_translate(gpa, &hpa, &prot, &memtype) || (prot & (GPT_R | GPT_W)) != (GPT_R | GPT_W)
        || memtype != MT_WB || vlapic_access_page(hpa & ~0xfffull)) {
        return NULL;
    }
    return (uint8_t *)(uintptr_t)hpa;
}

static unsigned guest_cpl(struct vcpu *v) {
    struct segment ss;
    vcpu_get_seg(v, SEG_SS, &ss);
    return (ss.attrib >> 5) & 3;
}

static bool long_mode_64(struct vcpu *v) {
    struct segment cs;
    vcpu_get_seg(v, SEG_CS, &cs);
    return (vcpu_efer(v) & EFER_LMA) && (cs.attrib & SEG_L);
}

static uint64_t guest_reg(struct vcpu *v, unsigned reg) {
    return reg == 4 ? vcpu_rsp(v) : ((const uint64_t *)&v->gprs)[reg];
}

static void set_guest_reg(struct vcpu *v, unsigned reg, uint64_t value) {
    if (reg == 4) {
        vcpu_set_rsp(v, value);
    } else {
        ((uint64_t *)&v->gprs)[reg] = value;
    }
}

// Instruction information (SDM 325462-093 Vol. 3C, Tables 30-14 and 30-15).
#define INFO_SCALE(info) ((info) & 3)
#define INFO_REG1(info) ((info) >> 3 & 15)
#define INFO_ADDRESS_SIZE(info) ((info) >> 7 & 7)
#define INFO_REGISTER (1u << 10)
#define INFO_SEGMENT(info) ((info) >> 15 & 7)
#define INFO_INDEX(info) ((info) >> 18 & 15)
#define INFO_NO_INDEX (1u << 22)
#define INFO_BASE(info) ((info) >> 23 & 15)
#define INFO_NO_BASE (1u << 27)
#define INFO_REG2(info) ((info) >> 28 & 15)

// The linear address of a VMX instruction's memory operand, or false with
// the fault raised.
static bool operand_address(struct vcpu *v, uint32_t info, unsigned size, uint64_t *linear) {
    uint64_t address = vmx_read(VMCS_EXIT_QUALIFICATION);
    if (!(info & INFO_NO_BASE)) {
        address += guest_reg(v, INFO_BASE(info));
    }
    if (!(info & INFO_NO_INDEX)) {
        address += guest_reg(v, INFO_INDEX(info)) << INFO_SCALE(info);
    }
    switch (INFO_ADDRESS_SIZE(info)) {
        case 0: {
            address &= 0xffff;
            break;
        }
        case 1: {
            address &= 0xffffffff;
            break;
        }
        default: {
            break;
        }
    }
    enum seg_reg reg = (enum seg_reg)INFO_SEGMENT(info);
    struct segment seg;
    vcpu_get_seg(v, reg, &seg);
    uint8_t fault = reg == SEG_SS ? 12 : 13;
    if (long_mode_64(v)) {
        if (reg == SEG_FS || reg == SEG_GS) {
            address += seg.base;
        }
        address = vcpu_lam(v, address);
        unsigned shift = (vcpu_cr4(v) & CR4_LA57) ? 7 : 16;
        if ((uint64_t)((int64_t)(address << shift) >> shift) != address) {
            vcpu_inject_exception(v, fault, true, 0);
            return false;
        }
    } else {
        if ((seg.attrib & SEG_UNUSABLE) || address + size - 1 > seg.limit) {
            vcpu_inject_exception(v, fault, true, 0);
            return false;
        }
        address = (address + seg.base) & 0xffffffff;
    }
    *linear = address;
    return true;
}

static bool read_operand64(struct vcpu *v, uint32_t info, uint64_t *value) {
    uint64_t linear;
    return operand_address(v, info, 8, &linear) && mmio_read_linear(v, linear, value, 8);
}

// VMsucceed, VMfailInvalid, and VMfailValid (SDM 325462-093 Vol. 3C, 33.2).
static void result(struct vcpu *v, uint64_t flags) {
    vcpu_set_rflags(v, (vcpu_rflags(v) & ~(uint64_t)VMX_RESULT_FLAGS) | flags);
    vcpu_skip_instruction(v);
}

static void succeed(struct vcpu *v) {
    result(v, 0);
}

static void fail(struct vcpu *v, uint32_t error) {
    struct nvmx *n = v->nvmx;
    if (n->vmcs12 == NULL) {
        result(v, RFLAGS_CF);
        return;
    }
    *field_at(n, field_slot(VMCS_VM_INSTR_ERROR)) = error;
    result(v, RFLAGS_ZF);
}

static void fail_invalid(struct vcpu *v) {
    result(v, RFLAGS_CF);
}

// The physical addresses a VMX instruction takes, 4 KiB-aligned within the
// processor's physical-address width.
static bool valid_address(uint64_t addr) {
    return (addr & 0xfff) == 0 && (addr >> phys_width) == 0;
}

static void vmxon(struct vcpu *v, struct nvmx *n, uint32_t info) {
    if (n->vmxon) {
        fail(v, ERROR_VMXON_IN_ROOT);
        return;
    }
    uint64_t cr0 = vcpu_cr0(v);
    uint64_t cr4 = vcpu_cr4(v);
    if ((cr0 & cap_cr0_fixed0) != cap_cr0_fixed0 || (cr0 & ~cap_cr0_fixed1)
        || (cr4 & cap_cr4_fixed0) != cap_cr4_fixed0 || (cr4 & ~cap_cr4_fixed1)) {
        vcpu_inject_exception(v, 13, true, 0);
        return;
    }
    uint64_t addr;
    if (!read_operand64(v, info, &addr)) {
        return;
    }
    uint8_t *region = valid_address(addr) ? guest_page(addr) : NULL;
    if (region == NULL || *(volatile uint32_t *)region != VMCS12_REVISION) {
        fail_invalid(v);
        return;
    }
    n->vmxon = true;
    n->vmxon_ptr = addr;
    n->current = NO_VMCS;
    n->vmcs12 = NULL;
    // Outside VMX operation, the guest's VMREAD and VMWRITE are to fault.
    if (shadowing) {
        vmx_write(VMCS_PROC_CONTROLS2, vmx_read(VMCS_PROC_CONTROLS2) | PROC2_VMCS_SHADOWING);
    }
    succeed(v);
}

static void vmxoff(struct vcpu *v, struct nvmx *n) {
    shadow_store(v, n);
    shadow_unload();
    if (shadowing) {
        vmx_write(VMCS_PROC_CONTROLS2,
                  vmx_read(VMCS_PROC_CONTROLS2) & ~(uint64_t)PROC2_VMCS_SHADOWING);
    }
    n->vmxon = false;
    n->current = NO_VMCS;
    n->vmcs12 = NULL;
    ept02_drop_all(n);
    vmx_invept_all();
    succeed(v);
}

static void vmclear(struct vcpu *v, struct nvmx *n, uint32_t info) {
    uint64_t addr;
    if (!read_operand64(v, info, &addr)) {
        return;
    }
    uint8_t *region = valid_address(addr) ? guest_page(addr) : NULL;
    if (region == NULL) {
        fail(v, ERROR_VMCLEAR_ADDRESS);
        return;
    }
    if (addr == n->vmxon_ptr) {
        fail(v, ERROR_VMCLEAR_VMXON);
        return;
    }
    *(volatile uint32_t *)(region + VMCS12_LAUNCHED) = 0;
    if (addr == n->current) {
        shadow_store(v, n);
        shadow_unload();
        n->current = NO_VMCS;
        n->vmcs12 = NULL;
    }
    succeed(v);
}

static void vmptrld(struct vcpu *v, struct nvmx *n, uint32_t info) {
    uint64_t addr;
    if (!read_operand64(v, info, &addr)) {
        return;
    }
    uint8_t *region = valid_address(addr) ? guest_page(addr) : NULL;
    if (region == NULL) {
        fail(v, ERROR_VMPTRLD_ADDRESS);
        return;
    }
    if (addr == n->vmxon_ptr) {
        fail(v, ERROR_VMPTRLD_VMXON);
        return;
    }
    // Shadow VMCSs need VMCS shadowing (SDM 325462-093 Vol. 3C, 27.2).
    uint32_t revision = *(volatile uint32_t *)region;
    bool shadow = (revision & VMCS12_SHADOW) != 0;
    if ((revision & ~VMCS12_SHADOW) != VMCS12_REVISION
        || (shadow && !(cap_proc2 >> 32 & PROC2_VMCS_SHADOWING))) {
        fail(v, ERROR_VMPTRLD_REVISION);
        return;
    }
    if (addr != n->current) {
        shadow_store(v, n);
        n->current = addr;
        n->vmcs12 = region;
        shadow_load(v, n, true);
    }
    n->current_shadow = shadow;
    succeed(v);
}

static void vmptrst(struct vcpu *v, struct nvmx *n, uint32_t info) {
    uint64_t linear;
    if (!operand_address(v, info, 8, &linear) || !mmio_write_linear(v, linear, &n->current, 8)) {
        return;
    }
    succeed(v);
}

// Outside 64-bit mode, VMREAD and VMWRITE take 32-bit operands (SDM
// 325462-093 Vol. 3C, 33.3). They access a VMCS region given, while their
// failures go to the current VMCS.
static void vmread_in(struct vcpu *v, uint8_t *vmcs, uint32_t info) {
    bool wide = long_mode_64(v);
    uint32_t field = (uint32_t)guest_reg(v, INFO_REG2(info));
    int slot = wide && (guest_reg(v, INFO_REG2(info)) >> 32) ? -1 : field_slot(field);
    if (slot < 0) {
        fail(v, ERROR_UNSUPPORTED_FIELD);
        return;
    }
    uint64_t value = *field_in(vmcs, slot);
    if (field & FIELD_HIGH) {
        value >>= 32;
    }
    if (!wide) {
        value &= 0xffffffff;
    }
    if (info & INFO_REGISTER) {
        set_guest_reg(v, INFO_REG1(info), value);
    } else {
        uint64_t linear;
        unsigned size = wide ? 8 : 4;
        if (!operand_address(v, info, size, &linear) || !mmio_write_linear(v, linear, &value, size)) {
            return;
        }
    }
    succeed(v);
}

// Returns whether it wrote the field, then in *written.
static bool vmwrite_in(struct vcpu *v, uint8_t *vmcs, uint32_t info, uint32_t *written) {
    bool wide = long_mode_64(v);
    uint64_t value = 0;
    if (info & INFO_REGISTER) {
        value = guest_reg(v, INFO_REG1(info));
    } else {
        uint64_t linear;
        unsigned size = wide ? 8 : 4;
        if (!operand_address(v, info, size, &linear) || !mmio_read_linear(v, linear, &value, size)) {
            return false;
        }
    }
    if (!wide) {
        value &= 0xffffffff;
    }
    uint32_t field = (uint32_t)guest_reg(v, INFO_REG2(info));
    int slot = wide && (guest_reg(v, INFO_REG2(info)) >> 32) ? -1 : field_slot(field);
    if (slot < 0) {
        fail(v, ERROR_UNSUPPORTED_FIELD);
        return false;
    }
    // Some processors drop the access rights' reserved bits on VMWRITE, as
    // Linux 7.3's KVM finds, which the shadow VMCS serving the guest's VMREADs
    // of them then does: so do its other reads.
    if (field >= VMCS_GUEST_ES_AR && field <= VMCS_GUEST_TR_AR) {
        value &= AR_WRITABLE;
    }
    uint64_t *at = field_in(vmcs, slot);
    if (field & FIELD_HIGH) {
        *at = (*at & 0xffffffff) | value << 32;
    } else {
        switch (FIELD_WIDTH(field)) {
            case FIELD_WIDTH_16: {
                value &= 0xffff;
                break;
            }
            case FIELD_WIDTH_32: {
                value &= 0xffffffff;
                break;
            }
            default: {
                break;
            }
        }
        *at = value;
    }
    *written = field;
    succeed(v);
    return true;
}

static void vmread(struct vcpu *v, struct nvmx *n, uint32_t info) {
    if (n->vmcs12 == NULL) {
        fail_invalid(v);
        return;
    }
    vmread_in(v, n->vmcs12, info);
}

static void vmwrite(struct vcpu *v, struct nvmx *n, uint32_t info) {
    if (n->vmcs12 == NULL) {
        fail_invalid(v);
        return;
    }
    uint32_t field;
    if (vmwrite_in(v, n->vmcs12, info, &field)) {
        shadow_update(v, n, field);
    }
}

static uint64_t get12(const struct nvmx *n, uint32_t field) {
    return *field_at(n, field_slot(field));
}

static void set12(struct nvmx *n, uint32_t field, uint64_t value) {
    *field_at(n, field_slot(field)) = value;
}

// The guest state VMCS12 and VMCS02 hold alike, but for DR7, IA32_DEBUGCTL,
// IA32_PAT, and IA32_EFER, which the controls decide on.
static const uint16_t guest_fields[] = {
    VMCS_GUEST_ES_SEL, VMCS_GUEST_CS_SEL, VMCS_GUEST_SS_SEL, VMCS_GUEST_DS_SEL, VMCS_GUEST_FS_SEL,
    VMCS_GUEST_GS_SEL, VMCS_GUEST_LDTR_SEL, VMCS_GUEST_TR_SEL,
    VMCS_GUEST_ES_LIMIT, VMCS_GUEST_CS_LIMIT, VMCS_GUEST_SS_LIMIT, VMCS_GUEST_DS_LIMIT,
    VMCS_GUEST_FS_LIMIT, VMCS_GUEST_GS_LIMIT, VMCS_GUEST_LDTR_LIMIT, VMCS_GUEST_TR_LIMIT,
    VMCS_GUEST_GDTR_LIMIT, VMCS_GUEST_IDTR_LIMIT,
    VMCS_GUEST_ES_AR, VMCS_GUEST_CS_AR, VMCS_GUEST_SS_AR, VMCS_GUEST_DS_AR, VMCS_GUEST_FS_AR,
    VMCS_GUEST_GS_AR, VMCS_GUEST_LDTR_AR, VMCS_GUEST_TR_AR,
    VMCS_GUEST_INTERRUPTIBILITY, VMCS_GUEST_ACTIVITY_STATE, VMCS_GUEST_SYSENTER_CS,
    VMCS_GUEST_CR0, VMCS_GUEST_CR3, VMCS_GUEST_CR4,
    VMCS_GUEST_ES_BASE, VMCS_GUEST_CS_BASE, VMCS_GUEST_SS_BASE, VMCS_GUEST_DS_BASE,
    VMCS_GUEST_FS_BASE, VMCS_GUEST_GS_BASE, VMCS_GUEST_LDTR_BASE, VMCS_GUEST_TR_BASE,
    VMCS_GUEST_GDTR_BASE, VMCS_GUEST_IDTR_BASE,
    VMCS_GUEST_RSP, VMCS_GUEST_RIP, VMCS_GUEST_RFLAGS, VMCS_GUEST_PENDING_DBG,
    VMCS_GUEST_SYSENTER_ESP, VMCS_GUEST_SYSENTER_EIP,
};

// The controls VMCS02 takes from VMCS12 as they are.
static const uint16_t control_fields[] = {
    VMCS_EXCEPTION_BITMAP, VMCS_PF_ERROR_MASK, VMCS_PF_ERROR_MATCH, VMCS_CR3_TARGET_COUNT,
    VMCS_ENTRY_INTR_INFO, VMCS_ENTRY_EXCEPTION_ERROR, VMCS_ENTRY_INSTR_LEN,
    VMCS_CR0_MASK, VMCS_CR4_MASK, VMCS_CR0_SHADOW, VMCS_CR4_SHADOW,
    VMCS_TSC_OFFSET,
};

// The exit information VMCS12 gets from VMCS02 for exits the processor
// itself made; the I/O RCX, RSI, RDI, and RIP fields, which only SMIs fill
// (SDM 325462-093 Vol. 3C, 27.9.1), stay zero.
static const uint16_t exit_fields[] = {
    VMCS_EXIT_INTR_INFO, VMCS_EXIT_INTR_ERROR, VMCS_IDT_VECTORING_INFO, VMCS_IDT_VECTORING_ERROR,
    VMCS_EXIT_INSTR_LEN, VMCS_EXIT_INSTR_INFO, VMCS_GUEST_LINEAR_ADDR, VMCS_GUEST_PHYS_ADDR,
};

#define ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))

// VMCS shadowing for the guest (SDM 325462-093 Vol. 3C, 27.10): where the
// processor has it, the guest's VMREADs and VMWRITEs of the fields its
// hypervisor uses most, as Linux 7.3's KVM chooses them, reach a shadow VMCS
// holding VMCS12's values instead of exiting. Writes of the access rights still
// exit, and the exit information is read there only where the processor's
// VMWRITE can load it (IA32_VMX_MISC bit 29).
static const uint16_t shadow_read_write[] = {
    VMCS_GUEST_INTR_STATUS, VMCS_GUEST_PML_INDEX, VMCS_HOST_FS_SEL, VMCS_HOST_GS_SEL, VMCS_PIN_CONTROLS,
    VMCS_PROC_CONTROLS, VMCS_EXCEPTION_BITMAP, VMCS_ENTRY_INTR_INFO, VMCS_ENTRY_EXCEPTION_ERROR,
    VMCS_ENTRY_INSTR_LEN, VMCS_TPR_THRESHOLD, VMCS_GUEST_INTERRUPTIBILITY, VMCS_PREEMPTION_TIMER,
    VMCS_GUEST_RIP, VMCS_GUEST_RSP, VMCS_GUEST_RFLAGS, VMCS_GUEST_CR0, VMCS_GUEST_CR3,
    VMCS_GUEST_CR4, VMCS_CR0_MASK, VMCS_CR0_SHADOW, VMCS_CR4_SHADOW, VMCS_HOST_FS_BASE,
    VMCS_HOST_GS_BASE,
};
static const uint16_t shadow_read_only[] = {VMCS_GUEST_CS_AR, VMCS_GUEST_SS_AR};
static const uint16_t shadow_exit_information[] = {
    VMCS_EXIT_REASON, VMCS_EXIT_QUALIFICATION, VMCS_EXIT_INTR_INFO, VMCS_EXIT_INTR_ERROR,
    VMCS_IDT_VECTORING_INFO, VMCS_IDT_VECTORING_ERROR, VMCS_EXIT_INSTR_LEN,
    VMCS_GUEST_LINEAR_ADDR, VMCS_GUEST_PHYS_ADDR,
};

// What the shadow VMCS holds, and of it what the guest writes there.
static uint16_t shadow_loaded[48];
static unsigned shadow_loaded_count;
static uint16_t shadow_stored[48];
static unsigned shadow_stored_count;

static void shadow_field(uint16_t field, bool written) {
    if (field_slot(field) < 0) {
        return;
    }
    vmread_bitmap[field >> 3] &= (uint8_t)~(1u << (field & 7));
    shadow_loaded[shadow_loaded_count++] = field;
    if (FIELD_WIDTH(field) == FIELD_WIDTH_64) {
        vmread_bitmap[(field | FIELD_HIGH) >> 3] &= (uint8_t)~(1u << ((field | FIELD_HIGH) & 7));
    }
    if (written) {
        vmwrite_bitmap[field >> 3] &= (uint8_t)~(1u << (field & 7));
        shadow_stored[shadow_stored_count++] = field;
    }
}

static void shadowing_init(uint64_t misc) {
    shadowing = vmx_secondary_allowed(PROC2_VMCS_SHADOWING);
    if (!shadowing) {
        return;
    }
    vmread_bitmap = page_alloc(1);
    vmwrite_bitmap = page_alloc(1);
    memset(vmread_bitmap, 0xff, PAGE_SIZE);
    memset(vmwrite_bitmap, 0xff, PAGE_SIZE);
    for (unsigned i = 0; i < ARRAY_COUNT(shadow_read_write); i++) {
        shadow_field(shadow_read_write[i], true);
    }
    for (unsigned i = 0; i < ARRAY_COUNT(shadow_read_only); i++) {
        shadow_field(shadow_read_only[i], false);
    }
    for (unsigned i = 0; (misc & MISC_VMWRITE_ANY) && i < ARRAY_COUNT(shadow_exit_information);
         i++) {
        shadow_field(shadow_exit_information[i], false);
    }
}

static bool field_shadowed(uint32_t field) {
    return shadowing && !(vmread_bitmap[(field & 0x7fff) >> 3] & (1u << (field & 7)));
}

// The guest's current VMCS for VMCS shadowing: its fields in the shadow VMCS,
// which the link pointer then names. After an exit, only those exits change:
// the rest change by the guest's own VMWRITEs, which go there.
static void shadow_load(struct vcpu *v, struct nvmx *n, bool all) {
    if (!shadowing) {
        return;
    }
    vmx_other_vmcs(v, n->shadow);
    for (unsigned i = 0; i < shadow_loaded_count; i++) {
        uint16_t field = shadow_loaded[i];
        if (all || FIELD_TYPE(field) == FIELD_TYPE_EXIT_INFO
            || FIELD_TYPE(field) == FIELD_TYPE_GUEST || field == VMCS_ENTRY_INTR_INFO) {
            vmx_other_write(field, get12(n, field));
        }
    }
    vmx_own_vmcs(v);
    vmx_write(VMCS_LINK_POINTER, n->shadow);
}

// What the guest wrote through VMCS shadowing, into VMCS12.
static void shadow_store(struct vcpu *v, struct nvmx *n) {
    if (!shadowing || n->vmcs12 == NULL) {
        return;
    }
    vmx_other_vmcs(v, n->shadow);
    for (unsigned i = 0; i < shadow_stored_count; i++) {
        set12(n, shadow_stored[i], vmx_other_read(shadow_stored[i]));
    }
    vmx_own_vmcs(v);
}

// No current VMCS: VMCS shadowing then fails VMREAD and VMWRITE as they
// should (SDM 325462-093 Vol. 3C, 33.3).
static void shadow_unload(void) {
    if (shadowing) {
        vmx_write(VMCS_LINK_POINTER, ~0ull);
    }
}

static void shadow_update(struct vcpu *v, struct nvmx *n, uint32_t field) {
    field &= ~FIELD_HIGH;
    if (field_shadowed(field)) {
        vmx_other_vmcs(v, n->shadow);
        vmx_other_write(field, get12(n, field));
        vmx_own_vmcs(v);
    }
}

static bool allowed(uint64_t value, uint64_t cap) {
    uint32_t required = (uint32_t)cap;
    uint32_t permitted = (uint32_t)(cap >> 32);
    return (value & required) == required && !(value & ~(uint64_t)permitted);
}

static bool canonical(uint64_t address) {
    unsigned bits = (cpuid(0x80000008, 0).eax >> 8) & 0xff;
    return (uint64_t)((int64_t)(address << (64 - bits)) >> (64 - bits)) == address;
}

static bool pat_valid(uint64_t pat) {
    for (int i = 0; i < 8; i++) {
        uint8_t type = pat >> (i * 8);
        if (type > 7 || type == 2 || type == 3) {
            return false;
        }
    }
    return true;
}

// A list of MSR entries, 16 bytes each, its last byte within the address
// width too (SDM 325462-093 Vol. 3C, 29.2.1.2, 29.2.1.3).
static bool msr_list_valid(uint64_t address, uint64_t count) {
    return count == 0
           || ((address & 15) == 0 && (address >> phys_width) == 0
               && ((address + count * 16 - 1) >> phys_width) == 0);
}

// An entry in the guest's RAM, or NULL.
static uint8_t *msr_entry(uint64_t address, uint64_t index) {
    uint64_t at = address + index * 16;
    uint8_t *page = guest_page(at & ~0xfffull);
    return page == NULL ? NULL : page + (at & 0xfff);
}

static uint64_t proc2_12(const struct nvmx *n) {
    if (!(get12(n, VMCS_PROC_CONTROLS) & PROC_SECONDARY_CONTROLS)) {
        return 0;
    }
    return get12(n, VMCS_PROC_CONTROLS2);
}

static bool ept12(const struct nvmx *n) {
    return (proc2_12(n) & PROC2_EPT) != 0;
}

// EPT paging-structure entries (SDM 325462-093 Vol. 3C, 31.3.2).
#define EPT_R (1ull << 0)
#define EPT_W (1ull << 1)
#define EPT_X (1ull << 2)
#define EPT_MEMTYPE_SHIFT 3
#define EPT_IPAT (1ull << 6)
#define EPT_LARGE (1ull << 7)
#define EPT_ACCESSED (1ull << 8)
#define EPT_DIRTY (1ull << 9)
#define EPT_XU (1ull << 10)
#define EPT_RIGHTS (EPT_R | EPT_W | EPT_X | EPT_XU)
#define EPT_ADDRESS 0x000ffffffffff000ull
// Reserved in entries that reference tables, bit 7 aside.
#define EPT_TABLE_RESERVED 0x78ull

// The page-modification log's entries (SDM 325462-093 Vol. 3C, 31.3.6).
#define PML_ENTRIES 512

// The EPTP (SDM 325462-093 Vol. 3C, Table 27-9).
#define EPTP_LEVELS(eptp) ((int)((eptp) >> 3 & 7) + 1)
#define EPTP_AD (1ull << 6)
#define EPTP_RESERVED 0xf80ull
#define EPTP_UC 0
#define EPTP_WB 6

static uint64_t level_size(int level) {
    return 1ull << (12 + 9 * level);
}

// Whether VM entry takes the EPTP (SDM 325462-093 Vol. 3C, 29.2.1.1).
static bool eptp_valid(uint64_t eptp) {
    uint64_t type = eptp & 7;
    int levels = EPTP_LEVELS(eptp);
    return ((type == EPTP_UC && (cap_ept_vpid & CAP_UC)) || (type == EPTP_WB && (cap_ept_vpid & CAP_WB)))
           && ((levels == 4 && (cap_ept_vpid & CAP_WALK_4))
               || (levels == 5 && (cap_ept_vpid & CAP_WALK_5)))
           && (!(eptp & EPTP_AD) || (cap_ept_vpid & CAP_AD)) && !(eptp & EPTP_RESERVED)
           && (eptp >> phys_width) == 0;
}

// SDM 325462-093 Vol. 3C, 31.3.3.1.
static bool ept12_misconfigured(uint64_t e, int level, bool leaf, bool mbec) {
    if (!(e & EPT_R)) {
        if (e & EPT_W) {
            return true;
        }
        if (!(cap_ept_vpid & CAP_EXEC_ONLY) && ((e & EPT_X) || (mbec && (e & EPT_XU)))) {
            return true;
        }
    }
    if (e & EPT_ADDRESS & ~((1ull << phys_width) - 1)) {
        return true;
    }
    if (!leaf) {
        return (e & (EPT_TABLE_RESERVED | EPT_LARGE)) != 0;
    }
    uint64_t type = (e >> EPT_MEMTYPE_SHIFT) & 7;
    if (type == 2 || type == 3 || type == 7) {
        return true;
    }
    if ((level == 1 && !(cap_ept_vpid & CAP_2M)) || (level == 2 && !(cap_ept_vpid & CAP_1G))) {
        return true;
    }
    return (e & EPT_ADDRESS & (level_size(level) - 1)) != 0;
}

// What a walk of the guest's EPT made of an address of its guest.
struct ept12_walk {
    uint64_t gpa;
    int level;
    uint64_t leaf;
    // The rights of every entry used, ANDed.
    uint64_t rights;
    // The access, as an EPT violation's qualification gives it, the way the
    // processor treats it.
    uint64_t access;
    // The entries used, by level, as the walk left them.
    uint64_t entry[5];
};

enum ept12_result {
    EPT12_MAPPED,
    EPT12_VIOLATION,
    EPT12_MISCONFIG,
    EPT12_PML_FULL,
};

// Where nothing decodes, an entry reads as all ones, which is misconfigured.
static uint64_t *ept12_entry(uint64_t gpa) {
    uint8_t *page = guest_page(gpa & ~0xfffull);
    return page == NULL ? NULL : (uint64_t *)(page + (gpa & 0xff8));
}

// Page-modification logging (SDM 325462-093 Vol. 3C, 31.3.6) of an address of
// the guest's guest whose dirty flag the guest's EPT got.
static void pml_log(struct nvmx *n, uint64_t gpa) {
    uint64_t index = get12(n, VMCS_GUEST_PML_INDEX);
    uint64_t at = get12(n, VMCS_PML_ADDRESS) + index * 8;
    uint8_t *page = guest_page(at & ~0xfffull);
    if (page != NULL) {
        *(volatile uint64_t *)(page + (at & 0xfff)) = gpa & ~0xfffull;
    }
    set12(n, VMCS_GUEST_PML_INDEX, (index - 1) & 0xffff);
}

// SDM 325462-093 Vol. 3C, 31.3.2, 31.3.3, and 31.3.5: on an access that the
// guest's EPT allows, the walk sets its accessed and dirty flags, but where the
// page-modification log has no room left (SDM 325462-093 Vol. 3C, 31.3.6).
static enum ept12_result ept12_walk(struct nvmx *n, uint64_t gpa, uint64_t access,
                                    struct ept12_walk *w) {
    uint64_t eptp = get12(n, VMCS_EPT_POINTER);
    bool mbec = (proc2_12(n) & PROC2_MBEC) != 0;
    bool ad = (eptp & EPTP_AD) != 0;
    int levels = EPTP_LEVELS(eptp);
    // With accessed and dirty flags, accesses to the guest's paging
    // structures are writes.
    if (ad && (access & EPT_QUAL_LINEAR) && !(access & EPT_QUAL_TRANSLATION)) {
        access |= EPT_QUAL_READ | EPT_QUAL_WRITE;
    }
    w->access = access;
    w->rights = 0;
    if (levels == 4 && (gpa >> 48) != 0) {
        return EPT12_VIOLATION;
    }
    uint64_t *used[5] = {0};
    uint64_t rights = EPT_RIGHTS;
    uint64_t table = eptp & EPT_ADDRESS;
    int l;
    for (l = levels - 1;; l--) {
        uint64_t *p = ept12_entry(table + ((gpa >> (12 + 9 * l)) & 511) * 8);
        uint64_t e = p == NULL ? ~0ull : __atomic_load_n(p, __ATOMIC_RELAXED);
        if (!(e & (EPT_R | EPT_W | EPT_X)) && !(mbec && (e & EPT_XU))) {
            return EPT12_VIOLATION;
        }
        bool leaf = l == 0 || (l <= 2 && (e & EPT_LARGE));
        if (ept12_misconfigured(e, l, leaf, mbec)) {
            return EPT12_MISCONFIG;
        }
        rights &= e;
        used[l] = p;
        w->entry[l] = e;
        if (leaf) {
            w->gpa = (e & EPT_ADDRESS & ~(level_size(l) - 1)) | (gpa & (level_size(l) - 1));
            w->level = l;
            w->leaf = e;
            break;
        }
        table = e & EPT_ADDRESS;
    }
    w->rights = rights & (mbec ? EPT_RIGHTS : EPT_R | EPT_W | EPT_X);
    uint64_t fetch = mbec && (access & EPT_QUAL_USER) ? EPT_XU : EPT_X;
    if (((access & EPT_QUAL_READ) && !(rights & EPT_R))
        || ((access & EPT_QUAL_WRITE) && !(rights & EPT_W))
        || ((access & EPT_QUAL_FETCH) && !(rights & fetch))) {
        return EPT12_VIOLATION;
    }
    if (ad) {
        bool pml = (proc2_12(n) & PROC2_PML) != 0;
        bool updates = (access & EPT_QUAL_WRITE) && !(w->entry[l] & EPT_DIRTY);
        for (int i = levels - 1; i >= l; i--) {
            updates |= !(w->entry[i] & EPT_ACCESSED);
        }
        if (pml && updates && get12(n, VMCS_GUEST_PML_INDEX) >= PML_ENTRIES) {
            return EPT12_PML_FULL;
        }
        for (int i = levels - 1; i >= l; i--) {
            __atomic_fetch_or(used[i], EPT_ACCESSED, __ATOMIC_RELAXED);
            w->entry[i] |= EPT_ACCESSED;
        }
        w->leaf |= EPT_ACCESSED;
        if (access & EPT_QUAL_WRITE) {
            uint64_t old = __atomic_fetch_or(used[l], EPT_DIRTY, __ATOMIC_RELAXED);
            w->leaf |= EPT_DIRTY;
            w->entry[l] |= EPT_DIRTY;
            if (pml && !(old & EPT_DIRTY)) {
                pml_log(n, gpa);
            }
        }
    }
    return EPT12_MAPPED;
}

// The qualification of an EPT violation of the guest's EPT: the access, the
// rights the guest's EPT gives, and what else the guest is offered of it.
static uint64_t ept12_qual(uint64_t access, const struct ept12_walk *w) {
    uint64_t kept = EPT_QUAL_READ | EPT_QUAL_WRITE | EPT_QUAL_FETCH | EPT_QUAL_LINEAR
                    | EPT_QUAL_TRANSLATION | EPT_QUAL_NMI_UNBLOCKED | EPT_QUAL_ASYNC;
    if (cap_ept_vpid & CAP_ADVANCED_INFO) {
        kept |= EPT_QUAL_USER | EPT_QUAL_WRITABLE | EPT_QUAL_XD;
    }
    uint64_t rights = (w->rights & (EPT_R | EPT_W | EPT_X)) << EPT_QUAL_RIGHTS_SHIFT;
    if (w->rights & EPT_XU) {
        rights |= 1ull << (EPT_QUAL_RIGHTS_SHIFT + 3);
    }
    return (access & kept) | rights;
}

// An EPT02 table: its page, then one with the guest's entry each of its
// entries was made from, where the guest's EPT has a table at its level.
static uint64_t *ept02_table(struct nvmx *n) {
    n->ept02_pages += 2;
    return page_alloc(2);
}

static void ept02_free(struct nvmx *n, uint64_t *table, int level) {
    for (unsigned i = 0; level > 0 && i < 512; i++) {
        uint64_t e = table[i];
        if ((e & EPT_RIGHTS) && !(e & EPT_LARGE)) {
            ept02_free(n, (uint64_t *)(uintptr_t)(e & EPT_ADDRESS), level - 1);
        }
    }
    page_free(table, 2);
    n->ept02_pages -= 2;
}

static void ept02_drop(struct nvmx *n, struct ept02 *r) {
    ept02_free(n, r->root, EPTP_LEVELS(r->eptp12) - 1);
    r->root = NULL;
    if (n->current02 == r) {
        n->current02 = NULL;
    }
}

// The processor may hold translations from the tables until an INVEPT, which
// is to come before the guest's guest runs again.
static void ept02_drop_all(struct nvmx *n) {
    for (unsigned i = 0; i < EPT02_ROOTS; i++) {
        if (n->ept02[i].root != NULL) {
            ept02_drop(n, &n->ept02[i]);
        }
    }
    n->ept02_apic = false;
}

// The EPTP of the shadow of VMCS12's EPT.
static uint64_t ept02_pointer(struct nvmx *n) {
    uint64_t eptp = get12(n, VMCS_EPT_POINTER);
    struct ept02 *r = NULL;
    for (unsigned i = 0; i < EPT02_ROOTS && r == NULL; i++) {
        if (n->ept02[i].root != NULL && n->ept02[i].eptp12 == eptp) {
            r = &n->ept02[i];
        }
    }
    for (unsigned i = 0; i < EPT02_ROOTS && r == NULL; i++) {
        if (n->ept02[i].root == NULL) {
            r = &n->ept02[i];
        }
    }
    if (r == NULL) {
        r = &n->ept02[n->ept02_next];
        n->ept02_next = (n->ept02_next + 1) % EPT02_ROOTS;
        ept02_drop(n, r);
        vmx_invept_all();
    }
    if (r->root == NULL) {
        r->eptp12 = eptp;
        r->root = ept02_table(n);
    }
    n->current02 = r;
    return (uint64_t)(uintptr_t)r->root | (uint64_t)(EPTP_LEVELS(eptp) - 1) << 3 | EPTP_WB
           | (eptp & EPTP_AD);
}

// An EPT02 leaf for the guest's mapping and the hypervisor's. With accessed
// and dirty flags, writes wait for the guest's dirty flag, which the leaf then
// takes as set until the guest's INVEPT, as a processor may (SDM 325462-093
// Vol. 3C, 31.3.5). Where the hypervisor's memory type is not write-back, it
// wins but over uncacheable.
static uint64_t ept02_leaf(const struct nvmx *n, const struct ept12_walk *w, uint64_t hpa,
                           uint32_t prot, uint8_t memtype, int level) {
    uint64_t e = hpa & EPT_ADDRESS & ~(level_size(level) - 1);
    if ((w->rights & EPT_R) && (prot & GPT_R)) {
        e |= EPT_R;
    }
    if ((w->rights & EPT_W) && (prot & GPT_W) && (e & EPT_R)
        && (!(get12(n, VMCS_EPT_POINTER) & EPTP_AD) || (w->leaf & EPT_DIRTY))) {
        e |= EPT_W | EPT_DIRTY;
    }
    if ((w->rights & EPT_X) && (prot & GPT_X)) {
        e |= EPT_X;
    }
    if ((w->rights & EPT_XU) && (prot & GPT_X)) {
        e |= EPT_XU;
    }
    if (!(e & EPT_RIGHTS)) {
        return 0;
    }
    uint64_t type = (w->leaf >> EPT_MEMTYPE_SHIFT) & 7;
    bool ipat = (w->leaf & EPT_IPAT) != 0;
    if (memtype != MT_WB && type != MT_UC) {
        type = memtype;
        ipat = false;
    }
    e |= type << EPT_MEMTYPE_SHIFT | EPT_ACCESSED;
    if (ipat) {
        e |= EPT_IPAT;
    }
    if (level > 0) {
        e |= EPT_LARGE;
    }
    return e;
}

// Maps an address of the guest's guest in VMCS02's EPT02 as the walk of the
// guest's EPT found it and the hypervisor's EPT has the guest's memory, but
// for the page behind the guest's local APICs' pages, which is the guest's
// guest's only where it is VMCS02's APIC-access page. Returns the rights it
// got.
static uint64_t ept02_fill(struct nvmx *n, uint64_t gpa, const struct ept12_walk *w) {
    uint64_t hpa;
    uint32_t prot;
    uint8_t memtype;
    int level;
    if (!gpt_translate_leaf(w->gpa, &hpa, &prot, &memtype, &level)) {
        return 0;
    }
    if (vlapic_access_page(hpa & ~0xfffull)) {
        if ((hpa & ~0xfffull) != n->apic_access02) {
            return 0;
        }
        n->ept02_apic = true;
    }
    if (level > w->level) {
        level = w->level;
    }
    if (level > ept02_level_max) {
        level = ept02_level_max;
    }
    uint64_t leaf = ept02_leaf(n, w, hpa, prot, memtype, level);
    if (leaf == 0) {
        return 0;
    }
    int top = EPTP_LEVELS(n->current02->eptp12) - 1;
    if (n->ept02_pages + 2 * top > ept02_budget) {
        ept02_drop_all(n);
        vmx_invept_all();
        vmx_write(VMCS_EPT_POINTER, ept02_pointer(n));
    }
    uint64_t *table = n->current02->root;
    for (int l = top; l > 0; l--) {
        unsigned i = (gpa >> (12 + 9 * l)) & 511;
        uint64_t *slot = &table[i];
        if (l >= w->level) {
            table[512 + i] = w->entry[l];
        }
        bool is_table = (*slot & EPT_RIGHTS) && !(*slot & EPT_LARGE);
        if (l <= level && !is_table) {
            *slot = l == level ? leaf : ept02_leaf(n, w, hpa, prot, memtype, l);
            return *slot & EPT_RIGHTS;
        }
        if (!is_table) {
            *slot = (uint64_t)(uintptr_t)ept02_table(n) | EPT_RIGHTS;
        }
        table = (uint64_t *)(uintptr_t)(*slot & EPT_ADDRESS);
    }
    leaf = ept02_leaf(n, w, hpa, prot, memtype, 0);
    table[(gpa >> 12) & 511] = leaf;
    table[512 + ((gpa >> 12) & 511)] = w->entry[0];
    return leaf & EPT_RIGHTS;
}

// Takes out of a shadow of the guest's EPT what the guest changed since it
// was made: the entries made from guest entries that read differently now,
// and what they lead to. Entries the guest added have no shadow yet.
static void ept02_sync(struct nvmx *n, uint64_t *table, uint64_t gpa12, int level) {
    const uint64_t *guest = (const uint64_t *)(uintptr_t)guest_page(gpa12);
    const uint64_t *made_from = table + 512;
    for (unsigned i = 0; i < 512; i++) {
        uint64_t e = table[i];
        if (!(e & EPT_RIGHTS)) {
            continue;
        }
        uint64_t now = guest == NULL ? ~made_from[i] : __atomic_load_n(&guest[i], __ATOMIC_RELAXED);
        bool below = level > 0 && !(e & EPT_LARGE);
        if (now != made_from[i]) {
            if (below) {
                ept02_free(n, (uint64_t *)(uintptr_t)(e & EPT_ADDRESS), level - 1);
            }
            table[i] = 0;
        } else if (below && !(now & EPT_LARGE)) {
            // Below a large page of the guest's, the shadow's tables split it:
            // its entry here covers them.
            ept02_sync(n, (uint64_t *)(uintptr_t)(e & EPT_ADDRESS), now & EPT_ADDRESS, level - 1);
        }
    }
}

static void ept02_sync_root(struct nvmx *n, struct ept02 *r) {
    ept02_sync(n, r->root, r->eptp12 & EPT_ADDRESS, EPTP_LEVELS(r->eptp12) - 1);
}

// An EPT violation of the guest's leaves nothing cached for the address it was
// for (SDM 325462-093 Vol. 3C, 31.4.3.1), which may map elsewhere once the
// guest has seen to it.
static void ept02_forget(struct nvmx *n, uint64_t gpa) {
    if (n->current02 == NULL) {
        return;
    }
    uint64_t *table = n->current02->root;
    for (int l = EPTP_LEVELS(n->current02->eptp12) - 1; l >= 0; l--) {
        uint64_t *slot = &table[(gpa >> (12 + 9 * l)) & 511];
        if (!(*slot & EPT_RIGHTS)) {
            return;
        }
        if (l == 0 || (*slot & EPT_LARGE)) {
            *slot = 0;
            return;
        }
        table = (uint64_t *)(uintptr_t)(*slot & EPT_ADDRESS);
    }
}

static bool ept02_allows(const struct nvmx *n, uint64_t rights, uint64_t access) {
    uint64_t fetch = (proc2_12(n) & PROC2_MBEC) && (access & EPT_QUAL_USER) ? EPT_XU : EPT_X;
    return (!(access & EPT_QUAL_READ) || (rights & EPT_R))
           && (!(access & EPT_QUAL_WRITE) || (rights & EPT_W))
           && (!(access & EPT_QUAL_FETCH) || (rights & fetch));
}

// The checks on the controls ReBIOS does not leave to the processor's on
// VMCS02 (SDM 325462-093 Vol. 3C, 29.2.1).
static bool controls_valid(const struct nvmx *n) {
    uint64_t pin = get12(n, VMCS_PIN_CONTROLS);
    uint64_t proc = get12(n, VMCS_PROC_CONTROLS);
    uint64_t proc2 = (proc & PROC_SECONDARY_CONTROLS) ? get12(n, VMCS_PROC_CONTROLS2) : 0;
    uint64_t exit = get12(n, VMCS_EXIT_CONTROLS);
    if (!allowed(pin, cap_true_pin) || !allowed(proc, cap_true_proc) || !allowed(exit, cap_true_exit)
        || !allowed(get12(n, VMCS_ENTRY_CONTROLS), cap_true_entry)
        || ((proc & PROC_SECONDARY_CONTROLS) && !allowed(proc2, cap_proc2))) {
        return false;
    }
    if (get12(n, VMCS_CR3_TARGET_COUNT) > cr3_targets
        || ((pin & PIN_VIRTUAL_NMIS) && !(pin & PIN_NMI_EXITING))
        || ((proc & PROC_NMI_WINDOW_EXITING) && !(pin & PIN_VIRTUAL_NMIS))
        || ((exit & EXIT_SAVE_PREEMPTION_TIMER) && !(pin & PIN_PREEMPTION_TIMER))) {
        return false;
    }
    // VMCS02 has addresses and a notification vector of the hypervisor's,
    // and exits on and acknowledges interrupts for it anyway.
    if ((proc & PROC_TPR_SHADOW)
        && (!valid_address(get12(n, VMCS_VIRTUAL_APIC_ADDR))
            || (!(proc2 & PROC2_VIRT_INTR_DELIVERY) && (get12(n, VMCS_TPR_THRESHOLD) >> 4) != 0))) {
        return false;
    }
    if ((proc2 & PROC2_VIRT_APIC_ACCESSES) && !valid_address(get12(n, VMCS_APIC_ACCESS_ADDR))) {
        return false;
    }
    if ((proc2 & PROC2_VIRT_INTR_DELIVERY) && !(pin & PIN_EXT_INTR_EXITING)) {
        return false;
    }
    if (pin & PIN_POSTED_INTR) {
        uint64_t desc = get12(n, VMCS_POSTED_INTR_DESC);
        if (!(proc2 & PROC2_VIRT_INTR_DELIVERY) || !(exit & EXIT_ACK_INTR_ON_EXIT)
            || (get12(n, VMCS_POSTED_INTR_NV) >> 8) != 0 || (desc & 0x3f) != 0
            || (desc >> phys_width) != 0) {
            return false;
        }
    }
    if (proc2 & PROC2_EPT) {
        if (!eptp_valid(get12(n, VMCS_EPT_POINTER))) {
            return false;
        }
    } else if (proc2 & (PROC2_UNRESTRICTED_GUEST | PROC2_PML | PROC2_MBEC)) {
        return false;
    }
    if ((proc2 & PROC2_PML) && !valid_address(get12(n, VMCS_PML_ADDRESS))) {
        return false;
    }
    if ((proc2 & PROC2_VMCS_SHADOWING)
        && (!valid_address(get12(n, VMCS_VMREAD_BITMAP))
            || !valid_address(get12(n, VMCS_VMWRITE_BITMAP)))) {
        return false;
    }
    if (proc2 & PROC2_VMFUNC) {
        uint64_t functions = get12(n, VMCS_VMFUNC_CONTROLS);
        if ((functions & ~cap_vmfunc)
            || ((functions & VMFUNC_EPTP_SWITCHING)
                && (!(proc2 & PROC2_EPT) || !valid_address(get12(n, VMCS_EPTP_LIST))))) {
            return false;
        }
    }
    if ((proc2 & PROC2_VPID) && get12(n, VMCS_VPID) == 0) {
        return false;
    }
    if (proc & PROC_IO_BITMAPS) {
        uint64_t a = get12(n, VMCS_IO_BITMAP_A);
        uint64_t b = get12(n, VMCS_IO_BITMAP_B);
        if (!valid_address(a) || !valid_address(b)) {
            return false;
        }
    }
    if (proc & PROC_MSR_BITMAPS) {
        uint64_t bitmap = get12(n, VMCS_MSR_BITMAP);
        if (!valid_address(bitmap)) {
            return false;
        }
    }
    return msr_list_valid(get12(n, VMCS_EXIT_MSR_STORE_ADDR), get12(n, VMCS_EXIT_MSR_STORE_COUNT))
           && msr_list_valid(get12(n, VMCS_EXIT_MSR_LOAD_ADDR), get12(n, VMCS_EXIT_MSR_LOAD_COUNT))
           && msr_list_valid(get12(n, VMCS_ENTRY_MSR_LOAD_ADDR), get12(n, VMCS_ENTRY_MSR_LOAD_COUNT));
}

// The checks on the host state (SDM 325462-093 Vol. 3C, 29.2.2, 29.2.3),
// which the exits of the guest's guest load into VMCS01.
static bool host_state_valid(const struct nvmx *n) {
    uint64_t exit = get12(n, VMCS_EXIT_CONTROLS);
    bool host64 = (exit & EXIT_HOST_ADDR_SPACE_SIZE) != 0;
    uint64_t cr0 = get12(n, VMCS_HOST_CR0);
    uint64_t cr3 = get12(n, VMCS_HOST_CR3);
    uint64_t cr4 = get12(n, VMCS_HOST_CR4);
    unsigned width = cpuid(0x80000008, 0).eax & 0xff;
    // CR3 has LAM's bits where the processor has LAM (SDM 325462-093 Vol. 3A,
    // 4.4.1).
    if (cpuid(7, 1).eax & CPUID_7_1_EAX_LAM) {
        cr3 &= ~(CR3_LAM_U57 | CR3_LAM_U48);
    }
    if ((cr0 & cap_cr0_fixed0) != cap_cr0_fixed0 || (cr0 & ~cap_cr0_fixed1)
        || (cr4 & cap_cr4_fixed0) != cap_cr4_fixed0 || (cr4 & ~cap_cr4_fixed1)
        || ((cr4 & CR4_CET) && !(cr0 & CR0_WP)) || (cr3 >> width) != 0) {
        return false;
    }
    static const uint16_t selectors[] = {
        VMCS_HOST_ES_SEL, VMCS_HOST_CS_SEL, VMCS_HOST_SS_SEL, VMCS_HOST_DS_SEL, VMCS_HOST_FS_SEL,
        VMCS_HOST_GS_SEL, VMCS_HOST_TR_SEL,
    };
    for (unsigned i = 0; i < ARRAY_COUNT(selectors); i++) {
        if (get12(n, selectors[i]) & 7) {
            return false;
        }
    }
    if (get12(n, VMCS_HOST_CS_SEL) == 0 || get12(n, VMCS_HOST_TR_SEL) == 0
        || (!host64 && get12(n, VMCS_HOST_SS_SEL) == 0)) {
        return false;
    }
    uint64_t rip = get12(n, VMCS_HOST_RIP);
    if (host64) {
        if (!(cr4 & CR4_PAE) || !canonical(rip)) {
            return false;
        }
    } else if ((get12(n, VMCS_ENTRY_CONTROLS) & ENTRY_IA32E_MODE_GUEST) || (cr4 & CR4_PCIDE)
               || (rip >> 32) != 0) {
        return false;
    }
    static const uint16_t bases[] = {
        VMCS_HOST_FS_BASE, VMCS_HOST_GS_BASE, VMCS_HOST_TR_BASE, VMCS_HOST_GDTR_BASE,
        VMCS_HOST_IDTR_BASE, VMCS_HOST_SYSENTER_ESP, VMCS_HOST_SYSENTER_EIP,
    };
    for (unsigned i = 0; i < ARRAY_COUNT(bases); i++) {
        if (!canonical(get12(n, bases[i]))) {
            return false;
        }
    }
    if (exit & EXIT_LOAD_EFER) {
        uint64_t efer = get12(n, VMCS_HOST_EFER);
        bool lma = (efer & EFER_LMA) != 0;
        bool lme = (efer & EFER_LME) != 0;
        if ((efer & ~(uint64_t)(EFER_SCE | EFER_LME | EFER_LMA | EFER_NXE)) || lma != host64
            || lme != host64) {
            return false;
        }
    }
    return !(exit & EXIT_LOAD_PAT) || pat_valid(get12(n, VMCS_HOST_PAT));
}

// The MSRs a VMCS switches are the current VMCS's, the others as RDMSR and
// WRMSR have them.
static bool msr_read(struct vcpu *v, uint32_t msr, uint64_t *value) {
    switch (msr) {
        case MSR_EFER: {
            *value = vmx_read(VMCS_GUEST_EFER);
            return true;
        }
        case MSR_IA32_PAT: {
            *value = vmx_read(VMCS_GUEST_PAT);
            return true;
        }
        case MSR_IA32_DEBUGCTL: {
            *value = vmx_read(VMCS_GUEST_DEBUGCTL);
            return true;
        }
        case MSR_IA32_SYSENTER_CS: {
            *value = vmx_read(VMCS_GUEST_SYSENTER_CS);
            return true;
        }
        case MSR_IA32_SYSENTER_ESP: {
            *value = vmx_read(VMCS_GUEST_SYSENTER_ESP);
            return true;
        }
        case MSR_IA32_SYSENTER_EIP: {
            *value = vmx_read(VMCS_GUEST_SYSENTER_EIP);
            return true;
        }
        default: {
            return exit_msr_read(v, msr, value);
        }
    }
}

static bool msr_write(struct vcpu *v, uint32_t msr, uint64_t value) {
    switch (msr) {
        case MSR_EFER: {
            // WRMSR leaves LMA alone.
            uint64_t old = vmx_read(VMCS_GUEST_EFER);
            if (value & ~(uint64_t)(EFER_SCE | EFER_LME | EFER_LMA | EFER_NXE)) {
                return false;
            }
            vmx_write(VMCS_GUEST_EFER, (value & ~(uint64_t)EFER_LMA) | (old & EFER_LMA));
            return true;
        }
        case MSR_IA32_PAT: {
            if (!pat_valid(value)) {
                return false;
            }
            vmx_write(VMCS_GUEST_PAT, value);
            return true;
        }
        case MSR_IA32_DEBUGCTL: {
            vmx_write(VMCS_GUEST_DEBUGCTL, value);
            return true;
        }
        case MSR_IA32_SYSENTER_CS: {
            vmx_write(VMCS_GUEST_SYSENTER_CS, value & 0xffffffff);
            return true;
        }
        case MSR_IA32_SYSENTER_ESP: {
            vmx_write(VMCS_GUEST_SYSENTER_ESP, value);
            return true;
        }
        case MSR_IA32_SYSENTER_EIP: {
            vmx_write(VMCS_GUEST_SYSENTER_EIP, value);
            return true;
        }
        default: {
            return exit_msr_write(v, msr, value);
        }
    }
}

// The entries an MSR-load list may not name (SDM 325462-093 Vol. 3C, 29.4).
static bool msr_loadable(uint32_t msr, uint32_t reserved) {
    return reserved == 0 && msr != MSR_FS_BASE && msr != MSR_GS_BASE && (msr >> 8) != 8
           && msr != MSR_IA32_SMM_MONITOR_CTL;
}

// The number of the entry that failed, from 1, or 0; past IA32_VMX_MISC's
// maximum, as Linux 7.3's KVM has it.
static uint64_t load_msr_list(struct vcpu *v, uint64_t address, uint64_t count) {
    for (uint64_t i = 0; i < count; i++) {
        const uint8_t *entry = msr_entry(address, i);
        if (i == MSR_LIST_MAX || entry == NULL) {
            return i + 1;
        }
        uint32_t msr = *(const volatile uint32_t *)entry;
        uint32_t reserved = *(const volatile uint32_t *)(entry + 4);
        uint64_t value = *(const volatile uint64_t *)(entry + 8);
        if (!msr_loadable(msr, reserved) || !msr_write(v, msr, value)) {
            return i + 1;
        }
    }
    return 0;
}

// IA32_TSC as it was at the exit, the rest as they are. Longer lists than
// IA32_VMX_MISC's maximum are undefined (SDM 325462-093 Vol. 3C, A.6).
static void store_msr_list(struct vcpu *v, uint64_t address, uint64_t count, uint64_t tsc) {
    for (uint64_t i = 0; i < count && i < MSR_LIST_MAX; i++) {
        uint8_t *entry = msr_entry(address, i);
        if (entry == NULL) {
            continue;
        }
        uint32_t msr = *(volatile uint32_t *)entry;
        uint64_t value = 0;
        if (msr == MSR_IA32_TSC) {
            value = tsc;
        } else {
            msr_read(v, msr, &value);
        }
        *(volatile uint64_t *)(entry + 8) = value;
    }
}

static bool msr_list_has(uint64_t address, uint64_t count, uint32_t msr) {
    for (uint64_t i = 0; i < count && i < MSR_LIST_MAX; i++) {
        const uint8_t *entry = msr_entry(address, i);
        if (entry != NULL && *(const volatile uint32_t *)entry == msr) {
            return true;
        }
    }
    return false;
}

// The guest's bit for an MSR up to 1FFFH in VMX's layout (SDM 325462-093 Vol.
// 3C, 27.6.9), in place of the hypervisor's.
static void guest_msr_bit(uint8_t *merged, const uint8_t *bitmap, uint32_t msr, bool write) {
    unsigned bit = (write ? 0x800 * 8 : 0) + msr;
    uint8_t mask = (uint8_t)(1u << (bit % 8));
    merged[bit / 8] = (uint8_t)((merged[bit / 8] & ~mask) | (bitmap[bit / 8] & mask));
}

// The MSR bitmap of the guest's guest: where the guest has its own, that with
// the hypervisor's intercepts added, as either may want the exits.
static void merge_msr_bitmap(struct nvmx *n, uint64_t proc, uint64_t proc2) {
    if (n->msr_bitmap == NULL) {
        n->msr_bitmap = page_alloc(1);
    }
    if (!(proc & PROC_MSR_BITMAPS)) {
        memset(n->msr_bitmap, 0xff, PAGE_SIZE);
        return;
    }
    const uint8_t *own_msr = (const uint8_t *)(uintptr_t)virt_msr_bitmap();
    const uint8_t *bitmap = guest_page(get12(n, VMCS_MSR_BITMAP));
    for (unsigned i = 0; i < PAGE_SIZE; i++) {
        n->msr_bitmap[i] = bitmap == NULL ? 0xff : bitmap[i] | own_msr[i];
    }
    // The base bitmap intercepts the x2APIC MSRs, for the guest's local APIC,
    // but for those whose accesses the processor virtualises for the guest (SDM
    // 325462-093 Vol. 3C, 32.5), as Linux 7.3's KVM has them.
    if (bitmap == NULL || !(proc2 & PROC2_VIRT_X2APIC)) {
        return;
    }
    if (proc2 & PROC2_APIC_REG_VIRT) {
        memcpy(n->msr_bitmap + MSR_X2APIC_FIRST / 8, bitmap + MSR_X2APIC_FIRST / 8,
               (MSR_X2APIC_LAST + 1 - MSR_X2APIC_FIRST) / 8);
    }
    guest_msr_bit(n->msr_bitmap, bitmap, MSR_X2APIC_TPR, false);
    guest_msr_bit(n->msr_bitmap, bitmap, MSR_X2APIC_TPR, true);
    if (proc2 & PROC2_VIRT_INTR_DELIVERY) {
        guest_msr_bit(n->msr_bitmap, bitmap, MSR_X2APIC_EOI, true);
        guest_msr_bit(n->msr_bitmap, bitmap, MSR_X2APIC_SELF_IPI, true);
    }
}

// What VMCS02 takes from VMCS01, read while that is the current one.
struct l1_values {
    uint64_t exit_controls;
    uint64_t entry_controls;
    uint64_t dr7;
    uint64_t debugctl;
    uint64_t pat;
    uint64_t efer;
    uint64_t bndcfgs;
};

// The controls VMCS02 virtualises the local APIC with: the guest's, on its
// pages, but for the vector its posted interrupts' notifications reach the
// processor with.
struct apic_controls {
    uint64_t pin;
    uint64_t proc;
    uint64_t proc2;
};

static void apic_virtualization(struct nvmx *n, struct apic_controls *c) {
    uint64_t pin = get12(n, VMCS_PIN_CONTROLS);
    uint64_t proc = get12(n, VMCS_PROC_CONTROLS);
    uint64_t proc2 = proc2_12(n);
    n->vapic = NULL;
    n->pid = NULL;
    n->apic_access02 = 0;
    int pi_vector = -1;
    if (proc & PROC_TPR_SHADOW) {
        n->vapic = guest_page(get12(n, VMCS_VIRTUAL_APIC_ADDR));
        // One the processor cannot take, to fail the entry.
        uint64_t address = ~0ull;
        if (n->vapic != NULL) {
            address = (uint64_t)(uintptr_t)n->vapic;
        } else if ((proc & (PROC_CR8_LOAD_EXITING | PROC_CR8_STORE_EXITING))
                       == (PROC_CR8_LOAD_EXITING | PROC_CR8_STORE_EXITING)
                   && !(proc2 & (PROC2_VIRT_APIC_ACCESSES | PROC2_VIRT_X2APIC
                                 | PROC2_VIRT_INTR_DELIVERY))) {
            // Nothing uses the page then, which need not be RAM, as Linux 7.3's
            // KVM has it.
            c->proc &= ~(uint64_t)PROC_TPR_SHADOW;
        }
        vmx_write(VMCS_VIRTUAL_APIC_ADDR, address);
        vmx_write(VMCS_TPR_THRESHOLD, get12(n, VMCS_TPR_THRESHOLD));
    } else {
        // CR8 is the TPR of the guest's local APIC, which only the hypervisor
        // has.
        c->proc |= PROC_CR8_LOAD_EXITING | PROC_CR8_STORE_EXITING;
    }
    if (proc2 & PROC2_VIRT_APIC_ACCESSES) {
        uint64_t hpa;
        uint32_t prot;
        uint8_t memtype;
        n->apic_access02 = gpt_translate(get12(n, VMCS_APIC_ACCESS_ADDR), &hpa, &prot, &memtype)
                               ? hpa
                               : access_nowhere;
        vmx_write(VMCS_APIC_ACCESS_ADDR, n->apic_access02);
    } else if (!(proc & PROC_TPR_SHADOW) && !(proc2 & PROC2_EPT) && vlapic_access_address() != 0) {
        // On the hypervisor's EPT, the guest's local APIC's page is the page
        // VMCS01 virtualises it on, which VMCS02 has every access to exit
        // from.
        c->proc2 |= PROC2_VIRT_APIC_ACCESSES;
        vmx_write(VMCS_APIC_ACCESS_ADDR, vlapic_access_address());
    }
    if (n->ept02_apic && !vlapic_access_page(n->apic_access02)) {
        ept02_drop_all(n);
        vmx_invept_all();
    }
    if (proc2 & PROC2_VIRT_INTR_DELIVERY) {
        vmx_write(VMCS_GUEST_INTR_STATUS, get12(n, VMCS_GUEST_INTR_STATUS));
        for (unsigned i = 0; i < 4; i++) {
            vmx_write(VMCS_EOI_EXIT0 + 2 * i, get12(n, VMCS_EOI_EXIT0 + 2 * i));
        }
    }
    if (pin & PIN_POSTED_INTR) {
        uint64_t desc = get12(n, VMCS_POSTED_INTR_DESC);
        uint8_t *page = guest_page(desc & ~0xfffull);
        if (page != NULL && n->vapic != NULL) {
            n->pid = (uint32_t *)(page + (desc & 0xfff));
            vmx_write(VMCS_POSTED_INTR_NV, VLAPIC_NESTED_POSTED_VECTOR);
            vmx_write(VMCS_POSTED_INTR_DESC, (uint64_t)(uintptr_t)n->pid);
            pi_vector = (int)get12(n, VMCS_POSTED_INTR_NV);
        } else {
            // Its notifications are the guest's interrupts then, as Linux 7.3's
            // KVM has it.
            c->pin &= ~(uint64_t)PIN_POSTED_INTR;
        }
    }
    __atomic_store_n(&n->pi_vector, pi_vector, __ATOMIC_SEQ_CST);
}

static void write_vmcs02(struct vcpu *v, struct nvmx *n, const struct l1_values *l1) {
    uint64_t pin = get12(n, VMCS_PIN_CONTROLS);
    uint64_t proc = get12(n, VMCS_PROC_CONTROLS);
    uint64_t proc2 = (proc & PROC_SECONDARY_CONTROLS) ? get12(n, VMCS_PROC_CONTROLS2) : 0;
    uint64_t entry = get12(n, VMCS_ENTRY_CONTROLS);

    merge_msr_bitmap(n, proc, proc2);
    // The hypervisor takes the interrupts and NMIs, as it does for the guest.
    // The processor consults the guest's I/O bitmaps at each access (SDM
    // 325462-093 Vol. 3C, 27.6.4), which may change meanwhile: every access
    // exits, to have them read then.
    struct apic_controls c = {
        .pin = pin | PIN_EXT_INTR_EXITING | PIN_NMI_EXITING | PIN_VIRTUAL_NMIS,
        .proc = (proc & ~(uint64_t)PROC_IO_BITMAPS) | PROC_UNCOND_IO_EXITING | PROC_MSR_BITMAPS
                | PROC_SECONDARY_CONTROLS,
        // EPT02's dirty flags are not the guest's: its walks log for it. The
        // guest's shadow VMCS is in its own format, which the hypervisor reads.
        .proc2 = (proc2 & ~(uint64_t)(PROC2_PML | PROC2_VMCS_SHADOWING)) | PROC2_EPT,
    };
    // The guest's guest's trace output goes through EPT02, into its own memory.
    if (vmx_pt_passthrough()) {
        c.proc2 |= PROC2_PT_USE_GPA;
        if (vmx_secondary_allowed(PROC2_CONCEAL_VMX_FROM_PT)) {
            c.proc2 |= PROC2_CONCEAL_VMX_FROM_PT;
        }
    }
    apic_virtualization(n, &c);
    vmx_write(VMCS_PIN_CONTROLS, c.pin);
    vmx_write(VMCS_PROC_CONTROLS, c.proc);
    vmx_write(VMCS_PROC_CONTROLS2, c.proc2);
    vmx_write(VMCS_EXIT_CONTROLS, l1->exit_controls);
    vmx_write(VMCS_ENTRY_CONTROLS, (l1->entry_controls & ~(uint64_t)ENTRY_IA32E_MODE_GUEST)
                                       | (entry & ENTRY_IA32E_MODE_GUEST));
    for (unsigned i = 0; i < ARRAY_COUNT(control_fields); i++) {
        vmx_write(control_fields[i], get12(n, control_fields[i]));
    }
    for (unsigned i = 0; i < cr3_targets; i++) {
        vmx_write(VMCS_CR3_TARGET0 + 2 * i, get12(n, VMCS_CR3_TARGET0 + 2 * i));
    }
    // The guest sees the processor's TSC: its guest's is that scaled and offset
    // as the guest's VMCS has it.
    if (proc2 & PROC2_TSC_SCALING) {
        vmx_write(VMCS_TSC_MULTIPLIER, get12(n, VMCS_TSC_MULTIPLIER));
    }
    if (proc2 & PROC2_PAUSE_LOOP_EXITING) {
        vmx_write(VMCS_PLE_GAP, get12(n, VMCS_PLE_GAP));
        vmx_write(VMCS_PLE_WINDOW, get12(n, VMCS_PLE_WINDOW));
    }
    // VMCS01's own bitmap has nothing for VMCS02's to add.
    if (proc2 & PROC2_XSAVES) {
        vmx_write(VMCS_XSS_EXIT_BITMAP, get12(n, VMCS_XSS_EXIT_BITMAP));
    }
    // The guest's VM functions all exit, for the hypervisor to carry out.
    if (proc2 & PROC2_VMFUNC) {
        vmx_write(VMCS_VMFUNC_CONTROLS, 0);
    }
    vmx_write(VMCS_MSR_BITMAP, (uint64_t)(uintptr_t)n->msr_bitmap);
    n->tsc_stored = msr_list_has(get12(n, VMCS_EXIT_MSR_STORE_ADDR),
                                 get12(n, VMCS_EXIT_MSR_STORE_COUNT), MSR_IA32_TSC);
    n->tsc_store[0] = MSR_IA32_TSC;
    vmx_write(VMCS_EXIT_MSR_STORE_ADDR, (uint64_t)(uintptr_t)n->tsc_store);
    vmx_write(VMCS_EXIT_MSR_STORE_COUNT, n->tsc_stored ? 1 : 0);
    vmx_write(VMCS_EXIT_MSR_LOAD_COUNT, 0);
    vmx_write(VMCS_ENTRY_MSR_LOAD_COUNT, 0);
    vmx_write(VMCS_LINK_POINTER, ~0ull);
    // EPT02 shadows the guest's EPT where it has one, else the guest's guest
    // runs on the hypervisor's EPT as the guest does.
    vmx_load_gpt_root(v);
    // Without VPIDs of the guest's, the processor drops the guest's guest's
    // translations with each entry and exit, as VPID 0's; VPID02 holds those
    // of one of the guest's VPIDs at a time.
    if (proc2 & PROC2_VPID) {
        uint16_t vpid = (uint16_t)get12(n, VMCS_VPID);
        vmx_write(VMCS_VPID, VPID02);
        if (vpid != n->vpid12) {
            vmx_invvpid(1, VPID02, 0);
            n->vpid12 = vpid;
        }
    }

    for (unsigned i = 0; i < ARRAY_COUNT(guest_fields); i++) {
        vmx_write(guest_fields[i], get12(n, guest_fields[i]));
    }
    // What entries load without the controls for it (SDM 325462-093 Vol. 3C,
    // 29.3.2.1).
    bool debug = (entry & ENTRY_LOAD_DEBUG_CONTROLS) != 0;
    vmx_write(VMCS_GUEST_DR7, debug ? get12(n, VMCS_GUEST_DR7) : l1->dr7);
    vmx_write(VMCS_GUEST_DEBUGCTL, debug ? get12(n, VMCS_GUEST_DEBUGCTL) : l1->debugctl);
    vmx_write(VMCS_GUEST_PAT, (entry & ENTRY_LOAD_PAT) ? get12(n, VMCS_GUEST_PAT) : l1->pat);
    // The guest has no controls for IA32_BNDCFGS: its guest runs with its
    // value, and leaves it its own.
    if (vmx_mpx_passthrough()) {
        vmx_write(VMCS_GUEST_BNDCFGS, l1->bndcfgs);
    }
    uint64_t efer = get12(n, VMCS_GUEST_EFER);
    if (!(entry & ENTRY_LOAD_EFER)) {
        efer = l1->efer & ~(uint64_t)(EFER_LMA | EFER_LME);
        if (entry & ENTRY_IA32E_MODE_GUEST) {
            efer |= EFER_LMA;
            if (get12(n, VMCS_GUEST_CR0) & CR0_PG) {
                efer |= EFER_LME;
            }
        }
    }
    vmx_write(VMCS_GUEST_EFER, efer);
    // The processor takes PAE paging's PDPTEs from VMCS02, which has EPT: the
    // guest's own with its EPT, else what CR3 points to (SDM 325462-093 Vol.
    // 3C, 29.3.2.4).
    vmx_write(VMCS_GUEST_CR3, get12(n, VMCS_GUEST_CR3));
    if (proc2 & PROC2_EPT) {
        for (unsigned i = 0; i < 4; i++) {
            vmx_write(VMCS_GUEST_PDPTE0 + 2 * i, get12(n, VMCS_GUEST_PDPTE0 + 2 * i));
        }
    } else {
        vmx_load_guest_pdptes();
    }
}

static void save_l2_state(struct nvmx *n) {
    for (unsigned i = 0; i < ARRAY_COUNT(guest_fields); i++) {
        set12(n, guest_fields[i], vmx_read(guest_fields[i]));
    }
    uint64_t exit = get12(n, VMCS_EXIT_CONTROLS);
    if (exit & EXIT_SAVE_DEBUG_CONTROLS) {
        set12(n, VMCS_GUEST_DR7, vmx_read(VMCS_GUEST_DR7));
        set12(n, VMCS_GUEST_DEBUGCTL, vmx_read(VMCS_GUEST_DEBUGCTL));
    }
    if (exit & EXIT_SAVE_PAT) {
        set12(n, VMCS_GUEST_PAT, vmx_read(VMCS_GUEST_PAT));
    }
    uint64_t efer = vmx_read(VMCS_GUEST_EFER);
    if (exit & EXIT_SAVE_EFER) {
        set12(n, VMCS_GUEST_EFER, efer);
    }
    // SDM 325462-093 Vol. 3C, 30.3.4.
    if (ept12(n)) {
        for (unsigned i = 0; i < 4; i++) {
            set12(n, VMCS_GUEST_PDPTE0 + 2 * i, vmx_read(VMCS_GUEST_PDPTE0 + 2 * i));
        }
    }
    if (proc2_12(n) & PROC2_VIRT_INTR_DELIVERY) {
        set12(n, VMCS_GUEST_INTR_STATUS, vmx_read(VMCS_GUEST_INTR_STATUS));
    }
    // As IA32_VMX_MISC bit 5 has it.
    uint64_t entry = get12(n, VMCS_ENTRY_CONTROLS) & ~(uint64_t)ENTRY_IA32E_MODE_GUEST;
    set12(n, VMCS_ENTRY_CONTROLS, entry | ((efer & EFER_LMA) ? ENTRY_IA32E_MODE_GUEST : 0));
}

static struct segment host_segment(const struct nvmx *n, uint32_t selector_field, uint32_t attrib,
                                   uint64_t base) {
    struct segment seg = {
        .selector = (uint16_t)get12(n, selector_field),
        .attrib = attrib,
        .limit = 0xffffffff,
        .base = base,
    };
    if (seg.selector == 0) {
        seg.attrib = SEG_UNUSABLE;
    }
    return seg;
}

// SDM 325462-093 Vol. 3C, 30.5.
static void load_l1_host_state(struct vcpu *v, struct nvmx *n, bool nmi) {
    uint64_t exit = get12(n, VMCS_EXIT_CONTROLS);
    bool host64 = (exit & EXIT_HOST_ADDR_SPACE_SIZE) != 0;
    uint64_t nmi_blocked = vmx_read(VMCS_GUEST_INTERRUPTIBILITY) & INTERRUPTIBILITY_NMI;

    struct vcpu_mode mode;
    // The bits a VM exit leaves alone.
    uint64_t kept = CR0_ET | CR0_CD | CR0_NW | 0xffffffff00000000ull | 0x1ff80000ull
                    | 0x20000ull | 0xffc0ull;
    mode.cr0 = (vcpu_cr0(v) & kept) | (get12(n, VMCS_HOST_CR0) & ~kept);
    mode.cr4 = get12(n, VMCS_HOST_CR4);
    if (host64) {
        mode.cr4 |= CR4_PAE;
    } else {
        mode.cr4 &= ~(uint64_t)CR4_PCIDE;
    }
    mode.cr3 = get12(n, VMCS_HOST_CR3);
    mode.efer = (exit & EXIT_LOAD_EFER) ? get12(n, VMCS_HOST_EFER) : vcpu_efer(v);
    mode.efer &= ~(uint64_t)(EFER_LMA | EFER_LME);
    if (host64) {
        mode.efer |= EFER_LMA | EFER_LME;
    }
    mode.rflags = RFLAGS_FIXED;

    uint32_t code = 0x9b | (1u << 11) | (host64 ? SEG_L : SEG_DB);
    uint32_t data = 0x93 | (1u << 11) | SEG_DB;
    mode.segs[SEG_CS] = host_segment(n, VMCS_HOST_CS_SEL, code, 0);
    mode.segs[SEG_SS] = host_segment(n, VMCS_HOST_SS_SEL, data, 0);
    mode.segs[SEG_DS] = host_segment(n, VMCS_HOST_DS_SEL, data, 0);
    mode.segs[SEG_ES] = host_segment(n, VMCS_HOST_ES_SEL, data, 0);
    mode.segs[SEG_FS] = host_segment(n, VMCS_HOST_FS_SEL, data, get12(n, VMCS_HOST_FS_BASE));
    mode.segs[SEG_GS] = host_segment(n, VMCS_HOST_GS_SEL, data, get12(n, VMCS_HOST_GS_BASE));
    // 64-bit mode uses FS and GS bases however unusable.
    mode.segs[SEG_FS].base = get12(n, VMCS_HOST_FS_BASE);
    mode.segs[SEG_GS].base = get12(n, VMCS_HOST_GS_BASE);
    mode.segs[SEG_TR] = (struct segment){
        .selector = (uint16_t)get12(n, VMCS_HOST_TR_SEL),
        .attrib = 0x8b,
        .limit = 0x67,
        .base = get12(n, VMCS_HOST_TR_BASE),
    };
    mode.segs[SEG_LDTR] = (struct segment){.attrib = SEG_UNUSABLE};
    mode.segs[SEG_GDTR] = (struct segment){.limit = 0xffff, .base = get12(n, VMCS_HOST_GDTR_BASE)};
    mode.segs[SEG_IDTR] = (struct segment){.limit = 0xffff, .base = get12(n, VMCS_HOST_IDTR_BASE)};
    vcpu_load_mode(v, &mode);
    vmx_load_guest_cr3(mode.cr3);

    vcpu_set_rip(v, get12(n, VMCS_HOST_RIP));
    vcpu_set_rsp(v, get12(n, VMCS_HOST_RSP));
    vmx_write(VMCS_GUEST_DR7, 0x400);
    vmx_write(VMCS_GUEST_DEBUGCTL, 0);
    vmx_write(VMCS_GUEST_SYSENTER_CS, get12(n, VMCS_HOST_SYSENTER_CS));
    vmx_write(VMCS_GUEST_SYSENTER_ESP, get12(n, VMCS_HOST_SYSENTER_ESP));
    vmx_write(VMCS_GUEST_SYSENTER_EIP, get12(n, VMCS_HOST_SYSENTER_EIP));
    if (exit & EXIT_LOAD_PAT) {
        vmx_write(VMCS_GUEST_PAT, get12(n, VMCS_HOST_PAT));
    }
    // Exits keep NMI blocking but where NMIs cause them (SDM 325462-093 Vol.
    // 3C, 30.5.5).
    vmx_write(VMCS_GUEST_INTERRUPTIBILITY, nmi ? INTERRUPTIBILITY_NMI : nmi_blocked);
    vmx_write(VMCS_GUEST_ACTIVITY_STATE, ACTIVITY_ACTIVE);
    vmx_write(VMCS_GUEST_PENDING_DBG, 0);
}

// The posted-interrupt descriptor's requests, then its notification bits
// (SDM 325462-093 Vol. 3C, Table 32-2), and the virtual-APIC page's VIRR.
#define PID_NOTIFICATION 8
#define PID_ON 1u
#define VAPIC_IRR 0x200

// Posted-interrupt processing of a notification the guest sent its guest's
// processor, as the processor does it (SDM 325462-093 Vol. 3C, 32.6), with
// VMCS02 current.
static void posted_process(struct nvmx *n) {
    __atomic_and_fetch(&n->pid[PID_NOTIFICATION], ~PID_ON, __ATOMIC_SEQ_CST);
    int highest = -1;
    for (unsigned i = 0; i < 8; i++) {
        uint32_t bits = __atomic_exchange_n(&n->pid[i], 0, __ATOMIC_SEQ_CST);
        if (bits != 0) {
            uint32_t *irr = (uint32_t *)(n->vapic + VAPIC_IRR + i * 0x10);
            __atomic_or_fetch(irr, bits, __ATOMIC_SEQ_CST);
            highest = (int)(i * 32 + 31) - __builtin_clz(bits);
        }
    }
    uint64_t status = vmx_read(VMCS_GUEST_INTR_STATUS);
    if (highest > (int)(status & 0xff)) {
        vmx_write(VMCS_GUEST_INTR_STATUS, (status & ~0xffull) | (uint64_t)highest);
    }
}

// The guest's guest no longer processes posted interrupts. A notification
// the hypervisor has yet to process came before an exit, or, where the guest's
// guest could not take it, after an entry that failed or in an inactive state,
// is the guest's interrupt.
static void posted_leave(struct vcpu *v, struct nvmx *n, bool entered) {
    int vector = __atomic_exchange_n(&n->pi_vector, -1, __ATOMIC_SEQ_CST);
    if (!__atomic_exchange_n(&n->pi_pending, false, __ATOMIC_SEQ_CST)) {
        return;
    }
    if (entered && n->pid != NULL) {
        posted_process(n);
    } else if (vector >= 0) {
        vlapic_accept(v, (uint8_t)vector, false);
    }
}

// What is left of the VMX-preemption timer of the guest's guest.
static uint64_t timer_left(const struct nvmx *n) {
    uint64_t now = rdtsc() >> timer_rate;
    return n->timer_deadline > now ? n->timer_deadline - now : 0;
}

// The exit of the guest's guest to the guest, VMCS02 being the vCPU's
// current VMCS: what the processor recorded where it made the exit, else the
// exit's reason, qualification, and interruption information. An entry that
// fails on the guest state leaves the guest state, the rest of the exit
// information, the MSR-store area, and the event to inject alone (SDM
// 325462-093 Vol. 3C, 27.8.3, 29.8).
static void exit_to_l1(struct vcpu *v, struct nvmx *n, uint32_t reason, uint64_t qual,
                       uint32_t intr_info, uint32_t intr_error, bool processor) {
    bool nmi = (reason & 0xffff) == EXIT_REASON_EXCEPTION_NMI
               && (intr_info & (7u << 8)) == INTR_TYPE_NMI;
    bool failed = (reason & EXIT_REASON_ENTRY_FAILURE) != 0;
    // The inactive states block the notification's interrupt as any other.
    posted_leave(v, n, !failed && n->inactive == ACTIVITY_ACTIVE);
    if (!failed) {
        save_l2_state(n);
        set12(n, VMCS_ENTRY_INTR_INFO, get12(n, VMCS_ENTRY_INTR_INFO) & ~(uint64_t)INTR_INFO_VALID);
        // An expired timer saves 0 (SDM 325462-093 Vol. 3C, 30.3.4).
        if (get12(n, VMCS_EXIT_CONTROLS) & EXIT_SAVE_PREEMPTION_TIMER) {
            set12(n, VMCS_PREEMPTION_TIMER,
                  (reason & 0xffff) == EXIT_REASON_PREEMPTION_TIMER ? 0 : timer_left(n));
        }
    }
    set12(n, VMCS_EXIT_REASON, reason);
    set12(n, VMCS_EXIT_QUALIFICATION, qual);
    if (!failed && processor) {
        for (unsigned i = 0; i < ARRAY_COUNT(exit_fields); i++) {
            set12(n, exit_fields[i], vmx_read(exit_fields[i]));
        }
    } else if (!failed) {
        for (unsigned i = 0; i < ARRAY_COUNT(exit_fields); i++) {
            set12(n, exit_fields[i], 0);
        }
        set12(n, VMCS_EXIT_INTR_INFO, intr_info);
        set12(n, VMCS_EXIT_INTR_ERROR, intr_error);
        // An event the entry was to inject, or one an exit cut short, is the
        // guest's to inject again, as Linux 7.3's KVM has it.
        uint32_t injected = (uint32_t)vmx_read(VMCS_ENTRY_INTR_INFO);
        if (injected & INTR_INFO_VALID) {
            set12(n, VMCS_IDT_VECTORING_INFO, injected);
            set12(n, VMCS_IDT_VECTORING_ERROR, vmx_read(VMCS_ENTRY_EXCEPTION_ERROR));
            set12(n, VMCS_EXIT_INSTR_LEN, vmx_read(VMCS_ENTRY_INSTR_LEN));
            vmx_write(VMCS_ENTRY_INTR_INFO, 0);
        } else if (v->redeliver & INTR_INFO_VALID) {
            set12(n, VMCS_IDT_VECTORING_INFO, v->redeliver);
            set12(n, VMCS_IDT_VECTORING_ERROR, v->redeliver_error);
            set12(n, VMCS_EXIT_INSTR_LEN, v->redeliver_length);
        }
    }
    v->redeliver = 0;
    v->injecting = false;
    n->run_pending = false;
    __atomic_store_n(&n->inactive, ACTIVITY_ACTIVE, __ATOMIC_SEQ_CST);
    if (!failed) {
        uint64_t tsc = processor && n->tsc_stored ? n->tsc_store[1] : rdtsc();
        store_msr_list(v, get12(n, VMCS_EXIT_MSR_STORE_ADDR), get12(n, VMCS_EXIT_MSR_STORE_COUNT),
                       tsc);
    }

    uint64_t bndcfgs = vmx_mpx_passthrough() ? vmx_read(VMCS_GUEST_BNDCFGS) : 0;
    vmx_vmcs_swap(v, &n->other);
    n->in_l2 = false;
    n->launching = false;
    if (vmx_mpx_passthrough()) {
        vmx_write(VMCS_GUEST_BNDCFGS, bndcfgs);
    }
    load_l1_host_state(v, n, nmi);
    // A failure here would be a VMX abort, which shuts the processor down.
    if (load_msr_list(v, get12(n, VMCS_EXIT_MSR_LOAD_ADDR), get12(n, VMCS_EXIT_MSR_LOAD_COUNT))) {
        printf("The guest's VM-exit MSR-load list failed\n");
    }
    vmx_load_gpt_root(v);
    shadow_load(v, n, false);
}

// An entry failing on the guest state, as the guest sees it: an exit with
// VMCS02 never entered.
static void entry_failure(struct vcpu *v, struct nvmx *n, uint32_t reason, uint64_t qual) {
    if (n->launching) {
        *(volatile uint32_t *)(n->vmcs12 + VMCS12_LAUNCHED) = 0;
    }
    exit_to_l1(v, n, EXIT_REASON_ENTRY_FAILURE | reason, qual, 0, 0, false);
}

// SDM 325462-093 Vol. 3C, 29.3.1.5: a VMCS other than the current one, a
// shadow VMCS exactly where VMCS shadowing is on.
static bool link_pointer_valid(const struct nvmx *n) {
    uint64_t link = get12(n, VMCS_LINK_POINTER);
    if (link == NO_VMCS) {
        return true;
    }
    const uint8_t *region = valid_address(link) ? guest_page(link) : NULL;
    if (region == NULL || link == n->current) {
        return false;
    }
    uint32_t shadow = (proc2_12(n) & PROC2_VMCS_SHADOWING) ? VMCS12_SHADOW : 0;
    return *(const volatile uint32_t *)region == (VMCS12_REVISION | shadow);
}

// The checks of SDM 325462-093 Vol. 3C, 29.3.1.5 on the activity states VMCS02
// does not enter, and so the processor's do not make: the guest's guest waits
// in them here, as they would block the hypervisor's interrupts too (SDM
// 325462-093 Vol. 3C, 28.2).
static bool inactive_valid(const struct nvmx *n, uint64_t activity) {
    if (activity < ACTIVITY_SHUTDOWN) {
        return true;
    }
    if (get12(n, VMCS_GUEST_INTERRUPTIBILITY) & (INTERRUPTIBILITY_STI | INTERRUPTIBILITY_MOV_SS)) {
        return false;
    }
    uint32_t injected = (uint32_t)get12(n, VMCS_ENTRY_INTR_INFO);
    if (!(injected & INTR_INFO_VALID)) {
        return true;
    }
    uint32_t type = injected & (7u << 8);
    return activity == ACTIVITY_SHUTDOWN
           && (type == INTR_TYPE_NMI || (type == INTR_TYPE_HW_EXCEPTION && (injected & 0xff) == 18));
}

static void vmlaunch(struct vcpu *v, struct nvmx *n, bool resume) {
    // Nor with a shadow VMCS (SDM 325462-093 Vol. 3C, 29.1).
    if (n->vmcs12 == NULL || n->current_shadow) {
        fail_invalid(v);
        return;
    }
    shadow_store(v, n);
    if (vmx_read(VMCS_GUEST_INTERRUPTIBILITY) & INTERRUPTIBILITY_MOV_SS) {
        fail(v, ERROR_ENTRY_MOV_SS);
        return;
    }
    uint32_t launched = *(volatile uint32_t *)(n->vmcs12 + VMCS12_LAUNCHED);
    if (!resume && launched) {
        fail(v, ERROR_VMLAUNCH_NOT_CLEAR);
        return;
    }
    if (resume && !launched) {
        fail(v, ERROR_VMRESUME_NOT_LAUNCHED);
        return;
    }
    if (!controls_valid(n)) {
        fail(v, ERROR_ENTRY_CONTROLS);
        return;
    }
    if (!host_state_valid(n)) {
        fail(v, ERROR_ENTRY_HOST_STATE);
        return;
    }

    struct l1_values l1 = {
        .exit_controls = vmx_read(VMCS_EXIT_CONTROLS),
        .entry_controls = vmx_read(VMCS_ENTRY_CONTROLS),
        .dr7 = vmx_read(VMCS_GUEST_DR7),
        .debugctl = vmx_read(VMCS_GUEST_DEBUGCTL),
        .pat = vmx_read(VMCS_GUEST_PAT),
        .efer = vmx_read(VMCS_GUEST_EFER),
        .bndcfgs = vmx_mpx_passthrough() ? vmx_read(VMCS_GUEST_BNDCFGS) : 0,
    };
    if (!n->created) {
        vmx_vmcs_create(v, &n->other);
        n->created = true;
    }
    vmx_vmcs_swap(v, &n->other);
    n->in_l2 = true;
    n->launching = !resume;
    if (!resume) {
        *(volatile uint32_t *)(n->vmcs12 + VMCS12_LAUNCHED) = 1;
    }
    write_vmcs02(v, n, &l1);
    if (get12(n, VMCS_PIN_CONTROLS) & PIN_PREEMPTION_TIMER) {
        n->timer_deadline = (rdtsc() >> timer_rate) + (uint32_t)get12(n, VMCS_PREEMPTION_TIMER);
    }
    v->redeliver = 0;
    // What the guest injects goes before the hypervisor's own deliveries.
    v->injecting = (get12(n, VMCS_ENTRY_INTR_INFO) & INTR_INFO_VALID) != 0;
    n->run_pending = true;

    // The processor checks VMCS02's guest state, but for the link pointer and
    // the activity states it does not enter VMCS02 in.
    if (!link_pointer_valid(n)) {
        entry_failure(v, n, EXIT_REASON_INVALID_GUEST_STATE, QUAL_LINK_POINTER);
        return;
    }
    uint64_t activity = get12(n, VMCS_GUEST_ACTIVITY_STATE);
    if (activity > ACTIVITY_WAIT_SIPI || !inactive_valid(n, activity)) {
        entry_failure(v, n, EXIT_REASON_INVALID_GUEST_STATE, QUAL_GUEST_STATE);
        return;
    }
    uint64_t failed = load_msr_list(v, get12(n, VMCS_ENTRY_MSR_LOAD_ADDR),
                                    get12(n, VMCS_ENTRY_MSR_LOAD_COUNT));
    if (failed) {
        entry_failure(v, n, EXIT_REASON_MSR_LOADING, failed);
        return;
    }
    if (activity < ACTIVITY_SHUTDOWN) {
        return;
    }
    // An injected event leaves the guest's guest active (SDM 325462-093 Vol.
    // 3C, 29.7.2), else it waits here.
    if (v->injecting) {
        vmx_write(VMCS_GUEST_ACTIVITY_STATE, ACTIVITY_ACTIVE);
        return;
    }
    n->run_pending = false;
    __atomic_store_n(&n->sipi, -1, __ATOMIC_SEQ_CST);
    __atomic_store_n(&n->inactive, (int)activity, __ATOMIC_SEQ_CST);
}

static uint64_t operand_register(struct vcpu *v, uint32_t info) {
    uint64_t value = guest_reg(v, INFO_REG2(info));
    return long_mode_64(v) ? value : value & 0xffffffff;
}

// The 128-bit descriptor INVEPT and INVVPID take.
static bool read_descriptor(struct vcpu *v, uint32_t info, uint64_t desc[2]) {
    uint64_t linear;
    if (!operand_address(v, info, 16, &linear) || !mmio_read_linear(v, linear, &desc[0], 8)) {
        return false;
    }
    uint64_t high = linear + 8;
    if (!long_mode_64(v)) {
        high &= 0xffffffff;
    }
    return mmio_read_linear(v, high, &desc[1], 8);
}

static void invept(struct vcpu *v, struct nvmx *n, uint32_t info) {
    uint64_t type = operand_register(v, info);
    if (!(type == 1 && (cap_ept_vpid & CAP_INVEPT_SINGLE))
        && !(type == 2 && (cap_ept_vpid & CAP_INVEPT_ALL))) {
        fail(v, ERROR_INVALID_OPERAND);
        return;
    }
    uint64_t desc[2];
    if (!read_descriptor(v, info, desc)) {
        return;
    }
    if (type == 1) {
        if (!eptp_valid(desc[0])) {
            fail(v, ERROR_INVALID_OPERAND);
            return;
        }
        for (unsigned i = 0; i < EPT02_ROOTS; i++) {
            struct ept02 *r = &n->ept02[i];
            if (r->root != NULL && ((r->eptp12 ^ desc[0]) & EPT_ADDRESS) == 0) {
                ept02_sync_root(n, r);
            }
        }
    } else {
        for (unsigned i = 0; i < EPT02_ROOTS; i++) {
            if (n->ept02[i].root != NULL) {
                ept02_sync_root(n, &n->ept02[i]);
            }
        }
    }
    vmx_invept_all();
    succeed(v);
}

static void invvpid(struct vcpu *v, struct nvmx *n, uint32_t info) {
    static const uint64_t types[] = {
        CAP_INVVPID_ADDRESS, CAP_INVVPID_SINGLE, CAP_INVVPID_ALL, CAP_INVVPID_GLOBALS,
    };
    uint64_t type = operand_register(v, info);
    if (type > 3 || !(cap_ept_vpid & types[type])) {
        fail(v, ERROR_INVALID_OPERAND);
        return;
    }
    uint64_t desc[2];
    if (!read_descriptor(v, info, desc)) {
        return;
    }
    uint16_t vpid = (uint16_t)desc[0];
    if ((desc[0] >> 16) != 0 || (type != 2 && vpid == 0) || (type == 0 && !canonical(desc[1]))) {
        fail(v, ERROR_INVALID_OPERAND);
        return;
    }
    if (type == 2 || vpid == n->vpid12) {
        vmx_invvpid(type == 2 ? 1 : (unsigned)type, VPID02, desc[1]);
    }
    succeed(v);
}

void nvmx_instruction(struct vcpu *v, uint32_t reason) {
    struct nvmx *n = v->nvmx;
    // The processor raises these itself in real mode, virtual-8086 mode and
    // compatibility mode, but not for the guest's CR4.VMXE, which only the
    // shadow has, nor for what the guest is not offered (SDM 325462-093 Vol.
    // 3C, 33.3).
    bool invept_offered = (cap_proc2 >> 32 & PROC2_EPT) && (cap_ept_vpid & CAP_INVEPT);
    bool invvpid_offered = (cap_proc2 >> 32 & PROC2_VPID) && (cap_ept_vpid & CAP_INVVPID);
    if (n == NULL || !(vcpu_cr4(v) & CR4_VMXE) || (reason != EXIT_REASON_VMXON && !n->vmxon)
        || (reason == EXIT_REASON_INVEPT && !invept_offered)
        || (reason == EXIT_REASON_INVVPID && !invvpid_offered) || reason == EXIT_REASON_VMFUNC
        || reason == EXIT_REASON_GETSEC) {
        vcpu_inject_exception(v, 6, false, 0);
        return;
    }
    if (guest_cpl(v) > 0) {
        vcpu_inject_exception(v, 13, true, 0);
        return;
    }
    uint32_t info = (uint32_t)vmx_read(VMCS_EXIT_INSTR_INFO);
    switch (reason) {
        case EXIT_REASON_VMXON: {
            vmxon(v, n, info);
            break;
        }
        case EXIT_REASON_VMXOFF: {
            vmxoff(v, n);
            break;
        }
        case EXIT_REASON_VMCLEAR: {
            vmclear(v, n, info);
            break;
        }
        case EXIT_REASON_VMPTRLD: {
            vmptrld(v, n, info);
            break;
        }
        case EXIT_REASON_VMPTRST: {
            vmptrst(v, n, info);
            break;
        }
        case EXIT_REASON_VMREAD: {
            vmread(v, n, info);
            break;
        }
        case EXIT_REASON_VMWRITE: {
            vmwrite(v, n, info);
            break;
        }
        case EXIT_REASON_VMLAUNCH:
        case EXIT_REASON_VMRESUME: {
            vmlaunch(v, n, reason == EXIT_REASON_VMRESUME);
            break;
        }
        case EXIT_REASON_INVEPT: {
            invept(v, n, info);
            break;
        }
        case EXIT_REASON_INVVPID: {
            invvpid(v, n, info);
            break;
        }
        default: {
            vcpu_inject_exception(v, 6, false, 0);
            break;
        }
    }
}

bool nvmx_in_l2(struct vcpu *v) {
    return v->nvmx != NULL && v->nvmx->in_l2;
}

bool nvmx_holds_init(struct vcpu *v) {
    return v->nvmx != NULL && v->nvmx->vmxon;
}

// What ends the guest's guest's wait: a start-up IPI, else INIT, the
// VMX-preemption timer, or an NMI not blocked (SDM 325462-093 Vol. 3C, 28.2,
// 29.7.1).
static bool inactive_ends(struct vcpu *v) {
    struct nvmx *n = v->nvmx;
    if (n->inactive == ACTIVITY_WAIT_SIPI) {
        return __atomic_load_n(&n->sipi, __ATOMIC_ACQUIRE) >= 0;
    }
    uint64_t pin = get12(n, VMCS_PIN_CONTROLS);
    bool nmi = v->nmi_pending || __atomic_load_n(&v->cpu->nmi_pending, __ATOMIC_ACQUIRE);
    bool nmi_blocked = !(pin & PIN_VIRTUAL_NMIS)
                       && (get12(n, VMCS_GUEST_INTERRUPTIBILITY) & INTERRUPTIBILITY_NMI);
    return __atomic_load_n(&v->cpu->init_pending, __ATOMIC_ACQUIRE) || (nmi && !nmi_blocked)
           || ((pin & PIN_PREEMPTION_TIMER) && timer_left(n) == 0);
}

void nvmx_wait(struct vcpu *v) {
    struct nvmx *n = v->nvmx;
    if (n == NULL || !n->in_l2 || n->inactive == ACTIVITY_ACTIVE || inactive_ends(v)) {
        return;
    }
    // Nothing interrupts a halt when the timer expires.
    bool timer = n->inactive == ACTIVITY_SHUTDOWN
                 && (get12(n, VMCS_PIN_CONTROLS) & PIN_PREEMPTION_TIMER);
    vcpu_wait(v, inactive_ends, timer);
}

bool nvmx_sipi(struct vcpu *v, uint8_t vector) {
    struct nvmx *n = v->nvmx;
    int none = -1;
    return n != NULL && __atomic_load_n(&n->inactive, __ATOMIC_ACQUIRE) == ACTIVITY_WAIT_SIPI
           && __atomic_compare_exchange_n(&n->sipi, &none, vector, false, __ATOMIC_ACQ_REL,
                                          __ATOMIC_RELAXED);
}

// The guest's I/O bitmaps, or else its unconditional I/O exiting, decide on
// I/O; an access wrapping past port FFFFh always exits (SDM 325462-093 Vol.
// 3C, 28.1.3).
static bool io_wanted(const struct nvmx *n, uint64_t proc, uint64_t qual) {
    if (!(proc & PROC_IO_BITMAPS)) {
        return (proc & PROC_UNCOND_IO_EXITING) != 0;
    }
    unsigned size = (unsigned)(qual & 7) + 1;
    unsigned port = (unsigned)(qual >> 16) & 0xffff;
    if (port + size > 0x10000) {
        return true;
    }
    for (unsigned p = port; p < port + size; p++) {
        uint64_t bitmap = get12(n, p < 0x8000 ? VMCS_IO_BITMAP_A : VMCS_IO_BITMAP_B);
        const uint8_t *page = guest_page(bitmap);
        if (page == NULL || (page[(p & 0x7fff) / 8] >> (p % 8) & 1)) {
            return true;
        }
    }
    return false;
}

// MSRs outside the bitmap's ranges always exit (SDM 325462-093 Vol. 3C,
// 27.6.9).
static bool msr_wanted(const struct nvmx *n, uint64_t proc, uint32_t msr, bool write) {
    if (!(proc & PROC_MSR_BITMAPS)) {
        return true;
    }
    unsigned offset;
    if (msr <= 0x1fff) {
        offset = 0;
    } else if (msr >= 0xc0000000 && msr <= 0xc0001fff) {
        offset = 0x400;
    } else {
        return true;
    }
    const uint8_t *page = guest_page(get12(n, VMCS_MSR_BITMAP));
    if (page == NULL) {
        return true;
    }
    unsigned bit = msr & 0x1fff;
    return page[(write ? 0x800 : 0) + offset + bit / 8] >> (bit % 8) & 1;
}

// Whether the guest asked for an exit of its guest's: VMCS02's controls are
// its own but for interrupts, NMIs, I/O, MSRs, and CR8, whose exits are the
// hypervisor's where the guest's controls do not take them, and EPT and the
// local APIC's page, which are the hypervisor's own.
static bool l1_wants(struct vcpu *v, const struct nvmx *n, uint32_t reason, uint64_t qual) {
    uint64_t proc = get12(n, VMCS_PROC_CONTROLS);
    switch (reason) {
        case EXIT_REASON_EXCEPTION_NMI: {
            uint32_t info = (uint32_t)vmx_read(VMCS_EXIT_INTR_INFO);
            return (info & (7u << 8)) != INTR_TYPE_NMI;
        }
        case EXIT_REASON_EXTERNAL_INTR:
        case EXIT_REASON_EPT_VIOLATION:
        case EXIT_REASON_EPT_MISCONFIG: {
            return false;
        }
        case EXIT_REASON_APIC_ACCESS: {
            return (proc2_12(n) & PROC2_VIRT_APIC_ACCESSES) != 0;
        }
        case EXIT_REASON_INTR_WINDOW: {
            return (proc & PROC_INTR_WINDOW_EXITING) != 0;
        }
        case EXIT_REASON_NMI_WINDOW: {
            return (proc & PROC_NMI_WINDOW_EXITING) != 0;
        }
        case EXIT_REASON_MONITOR_TRAP: {
            // The guest's by its control or by the pending MTF exit it
            // injected (SDM 325462-093 Vol. 3C, 29.6.2).
            return !v->stepping;
        }
        case EXIT_REASON_IO: {
            return io_wanted(n, proc, qual);
        }
        case EXIT_REASON_RDMSR:
        case EXIT_REASON_WRMSR: {
            return msr_wanted(n, proc, (uint32_t)v->gprs.rcx, reason == EXIT_REASON_WRMSR);
        }
        case EXIT_REASON_CR_ACCESS: {
            if ((qual & 15) != 8) {
                return true;
            }
            return (proc & (((qual >> 4) & 3) == 0 ? PROC_CR8_LOAD_EXITING
                                                   : PROC_CR8_STORE_EXITING)) != 0;
        }
        default: {
            return true;
        }
    }
}

// The guest's guest's MOV to and from CR8, where the guest does not take them:
// the guest's local APIC's TPR.
static void cr8_access(struct vcpu *v, uint64_t qual) {
    unsigned reg = (qual >> 8) & 15;
    if (((qual >> 4) & 3) == 0) {
        uint64_t value = guest_reg(v, reg);
        if (value >> 4) {
            vcpu_inject_exception(v, 13, true, 0);
            return;
        }
        vlapic_set_cr8(v, (uint32_t)value);
    } else {
        set_guest_reg(v, reg, vlapic_cr8(v));
    }
    vcpu_skip_instruction(v);
}

// What the processor found missing in EPT02: the guest's to have exit where
// its EPT does not allow it, else mapped as the hypervisor's EPT has the
// guest's memory, or emulated where that is the hypervisor's own device.
static void ept_violation(struct vcpu *v, struct nvmx *n, uint64_t qual) {
    uint64_t gpa = vmx_read(VMCS_GUEST_PHYS_ADDR);
    struct ept12_walk w;
    enum ept12_result result = ept12_walk(n, gpa, qual, &w);
    if (result == EPT12_VIOLATION) {
        ept02_forget(n, gpa);
        exit_to_l1(v, n, EXIT_REASON_EPT_VIOLATION, ept12_qual(qual, &w), 0, 0, true);
        return;
    }
    if (result == EPT12_PML_FULL) {
        exit_to_l1(v, n, EXIT_REASON_PML_FULL, qual & EPT_QUAL_NMI_UNBLOCKED, 0, 0, true);
        return;
    }
    // An IRET that started with NMIs blocked unblocked them before the exit:
    // they are blocked again for it to run again (SDM 325462-093 Vol. 3C,
    // 30.2.3).
    if (qual & EPT_QUAL_NMI_UNBLOCKED) {
        vmx_write(VMCS_GUEST_INTERRUPTIBILITY,
                  vmx_read(VMCS_GUEST_INTERRUPTIBILITY) | INTERRUPTIBILITY_NMI);
    }
    if (result == EPT12_MISCONFIG) {
        exit_to_l1(v, n, EXIT_REASON_EPT_MISCONFIG, 0, 0, 0, true);
        return;
    }
    if (ept02_allows(n, ept02_fill(n, gpa, &w), w.access)) {
        return;
    }
    if (qual & EPT_QUAL_ASYNC) {
        vmx_async_access(w.gpa);
        return;
    }
    // An event the hypervisor cannot deliver itself ends the guest's guest as
    // a triple fault would.
    if (event_cut_short(v)) {
        if (!event_deliver_real_mode(v)) {
            exit_to_l1(v, n, EXIT_REASON_TRIPLE_FAULT, 0, 0, 0, true);
        }
        return;
    }
    if (qual & EPT_QUAL_FETCH) {
        mmio_fetch(v, w.gpa);
    } else {
        mmio_fault(v, w.gpa);
    }
}

// The guest's guest's VMFUNC, which exits for every function: EPTP switching
// carried out where the guest enables it and it makes no exit itself (SDM
// 325462-093 Vol. 3C, 28.5.7.3), else the guest's exit.
static bool eptp_switch(struct vcpu *v, struct nvmx *n) {
    uint64_t function = v->gprs.rax & 0xffffffff;
    uint64_t index = v->gprs.rcx & 0xffffffff;
    if (function != 0 || !(get12(n, VMCS_VMFUNC_CONTROLS) & VMFUNC_EPTP_SWITCHING)
        || index >= EPTP_LIST_ENTRIES) {
        return false;
    }
    uint64_t at = get12(n, VMCS_EPTP_LIST) + index * 8;
    const uint8_t *page = guest_page(at & ~0xfffull);
    // Where nothing decodes, it reads as all ones, which is invalid.
    uint64_t eptp = page == NULL ? ~0ull : *(const volatile uint64_t *)(page + (at & 0xfff));
    uint64_t current = get12(n, VMCS_EPT_POINTER);
    if (!eptp_valid(eptp) || EPTP_LEVELS(eptp) != EPTP_LEVELS(current)) {
        return false;
    }
    set12(n, VMCS_EPT_POINTER, eptp);
    vmx_load_gpt_root(v);
    vcpu_skip_instruction(v);
    return true;
}

// The guest's guest's VMREAD or VMWRITE, which reaches the VMCS the link
// pointer names where the guest's VMCS shadowing and bitmaps let it (SDM
// 325462-093 Vol. 3C, 33.3), else exits.
static bool shadow_access(struct vcpu *v, struct nvmx *n, uint32_t reason) {
    if (!(proc2_12(n) & PROC2_VMCS_SHADOWING)) {
        return false;
    }
    uint32_t info = (uint32_t)vmx_read(VMCS_EXIT_INSTR_INFO);
    uint64_t field = operand_register(v, info);
    uint64_t bitmap = get12(n, reason == EXIT_REASON_VMREAD ? VMCS_VMREAD_BITMAP
                                                           : VMCS_VMWRITE_BITMAP);
    const uint8_t *page = guest_page(bitmap);
    if ((field >> 15) != 0 || page == NULL || (page[field >> 3] >> (field & 7) & 1)) {
        return false;
    }
    if (guest_cpl(v) > 0) {
        vcpu_inject_exception(v, 13, true, 0);
        return true;
    }
    uint64_t link = get12(n, VMCS_LINK_POINTER);
    uint8_t *shadow = link == NO_VMCS ? NULL : guest_page(link);
    if (shadow == NULL) {
        fail_invalid(v);
        return true;
    }
    uint32_t written;
    if (reason == EXIT_REASON_VMREAD) {
        vmread_in(v, shadow, info);
    } else {
        vmwrite_in(v, shadow, info, &written);
    }
    return true;
}

bool nvmx_l2_exit(struct vcpu *v, uint32_t reason, uint64_t qual) {
    struct nvmx *n = v->nvmx;
    if (n == NULL || !n->in_l2) {
        return false;
    }
    n->run_pending = false;
    if (reason & EXIT_REASON_ENTRY_FAILURE) {
        entry_failure(v, n, reason & 0xffff, qual);
        return true;
    }
    n->launching = false;
    if ((reason & 0xffff) == EXIT_REASON_EPT_VIOLATION && ept12(n)) {
        ept_violation(v, n, qual);
        return true;
    }
    // EPT02 is only a cache, made again from the guest's EPT and the
    // hypervisor's.
    if ((reason & 0xffff) == EXIT_REASON_EPT_MISCONFIG && ept12(n)) {
        printf("EPT02 is misconfigured at %#llx, made again\n",
               (unsigned long long)vmx_read(VMCS_GUEST_PHYS_ADDR));
        ept02_drop_all(n);
        vmx_load_gpt_root(v);
        vmx_invept_all();
        return true;
    }
    if ((reason & 0xffff) == EXIT_REASON_VMFUNC && eptp_switch(v, n)) {
        return true;
    }
    if (((reason & 0xffff) == EXIT_REASON_VMREAD || (reason & 0xffff) == EXIT_REASON_VMWRITE)
        && shadow_access(v, n, reason & 0xffff)) {
        return true;
    }
    if (!l1_wants(v, n, reason & 0xffff, qual)) {
        if ((reason & 0xffff) == EXIT_REASON_CR_ACCESS) {
            cr8_access(v, qual);
            return true;
        }
        return false;
    }
    uint32_t info = (uint32_t)vmx_read(VMCS_EXIT_INTR_INFO);
    exit_to_l1(v, n, reason, qual, info, 0, true);
    return true;
}

bool nvmx_entry_failed(struct vcpu *v, uint32_t error) {
    struct nvmx *n = v->nvmx;
    if (n == NULL || !n->in_l2) {
        return false;
    }
    if (n->launching) {
        *(volatile uint32_t *)(n->vmcs12 + VMCS12_LAUNCHED) = 0;
    }
    posted_leave(v, n, false);
    vmx_vmcs_swap(v, &n->other);
    n->in_l2 = false;
    n->launching = false;
    n->run_pending = false;
    vmx_load_gpt_root(v);
    fail(v, error);
    return true;
}

// The guest's INIT, and its NMIs and interrupts where VMCS12 exits on them,
// are exits of its guest's; else the guest's guest goes on, taking what the
// guest does not, but after the windows the guest asks for, as those exits come
// before NMIs and interrupts (SDM 325462-093 Vol. 3C, 28.2).
bool nvmx_before_entry(struct vcpu *v) {
    struct nvmx *n = v->nvmx;
    if (n == NULL || !n->in_l2) {
        return false;
    }
    // The instruction it came from is what the guest's guest was running.
    if (n->pending) {
        n->pending = false;
        exit_to_l1(v, n, n->pending_reason, n->pending_qual, n->pending_info, n->pending_error,
                   false);
        set12(n, VMCS_GUEST_PHYS_ADDR, n->pending_gpa);
        set12(n, VMCS_GUEST_LINEAR_ADDR, n->pending_linear);
        return false;
    }
    // A start-up IPI ends the wait for one (SDM 325462-093 Vol. 3C, 28.2).
    if (n->inactive == ACTIVITY_WAIT_SIPI) {
        int vector = __atomic_exchange_n(&n->sipi, -1, __ATOMIC_SEQ_CST);
        exit_to_l1(v, n, EXIT_REASON_SIPI, (uint64_t)vector, 0, 0, false);
        return false;
    }
    // The guest's events wait for an instruction boundary: past the delivery of
    // what its entry injects, or of what an exit cut short, as on hardware and
    // in Linux 7.3's KVM. An interrupt of the hypervisor's own then ends the
    // entry.
    bool waiting = n->run_pending || (v->redeliver & INTR_INFO_VALID);
    if (!waiting && __atomic_exchange_n(&v->cpu->init_pending, false, __ATOMIC_ACQUIRE)) {
        exit_to_l1(v, n, EXIT_REASON_INIT, 0, 0, 0, false);
        return false;
    }
    uint64_t pin = get12(n, VMCS_PIN_CONTROLS);
    uint64_t proc = get12(n, VMCS_PROC_CONTROLS);
    if (__atomic_exchange_n(&v->cpu->nmi_pending, false, __ATOMIC_ACQUIRE)) {
        v->nmi_pending = true;
    }
    // Without virtual NMIs, the guest's guest's blocking by NMI holds them
    // back, until an NMI window.
    bool nmi_held = false;
    if (!waiting && v->nmi_pending && (pin & PIN_NMI_EXITING)) {
        if (!(pin & PIN_VIRTUAL_NMIS)
            && (vmx_read(VMCS_GUEST_INTERRUPTIBILITY) & INTERRUPTIBILITY_NMI)) {
            nmi_held = true;
        } else {
            v->nmi_pending = false;
            exit_to_l1(v, n, EXIT_REASON_EXCEPTION_NMI, 0, INTR_INFO_VALID | INTR_TYPE_NMI | 2, 0,
                       false);
            return false;
        }
    }
    // The shutdown state blocks interrupts, but not the VMX-preemption timer's
    // exit, and an NMI the guest's guest takes leaves it (SDM 325462-093 Vol.
    // 3C, 28.2).
    if (n->inactive == ACTIVITY_SHUTDOWN) {
        if ((pin & PIN_PREEMPTION_TIMER) && timer_left(n) == 0) {
            exit_to_l1(v, n, EXIT_REASON_PREEMPTION_TIMER, 0, 0, 0, false);
            return false;
        }
        // Woken for nothing it takes, it waits on.
        if (!v->nmi_pending) {
            nvmx_wait(v);
            return nvmx_before_entry(v);
        }
        __atomic_store_n(&n->inactive, ACTIVITY_ACTIVE, __ATOMIC_SEQ_CST);
        vmx_write(VMCS_GUEST_ACTIVITY_STATE, ACTIVITY_ACTIVE);
    }
    bool extint = (pin & PIN_EXT_INTR_EXITING) && irq_extint_pending(v);
    int vector = (!(pin & PIN_EXT_INTR_EXITING) || extint) ? -1 : vlapic_pending(v);
    // The notification of the guest's posted interrupts, which the guest's
    // guest's processor takes instead of exiting (SDM 325462-093 Vol. 3C,
    // 32.6).
    if (vector >= 0 && vector == n->pi_vector) {
        vlapic_dismiss(v, (uint8_t)vector);
        __atomic_store_n(&n->pi_pending, true, __ATOMIC_SEQ_CST);
        vector = vlapic_pending(v);
    }
    if (waiting) {
        if (extint || vector >= 0 || (v->nmi_pending && (pin & PIN_NMI_EXITING))
            || __atomic_load_n(&v->cpu->init_pending, __ATOMIC_ACQUIRE)) {
            vcpu_exit_soon(v);
        }
    } else if (extint || vector >= 0) {
        uint32_t info = 0;
        if (get12(n, VMCS_EXIT_CONTROLS) & EXIT_ACK_INTR_ON_EXIT) {
            if (extint) {
                vector = irq_extint_acknowledge();
            } else {
                vlapic_take(v, (uint8_t)vector);
            }
            info = INTR_INFO_VALID | INTR_TYPE_EXT_INTR | (uint32_t)vector;
        }
        exit_to_l1(v, n, EXIT_REASON_EXTERNAL_INTR, 0, info, 0, false);
        return false;
    }
    if (n->pid != NULL && __atomic_exchange_n(&n->pi_pending, false, __ATOMIC_SEQ_CST)) {
        posted_process(n);
    }
    uint64_t windows = PROC_INTR_WINDOW_EXITING | PROC_NMI_WINDOW_EXITING;
    vmx_write(VMCS_PROC_CONTROLS, vmx_read(VMCS_PROC_CONTROLS) & ~windows);
    // A stepped instruction is to run, not an event's handler.
    if (v->stepping) {
        event_hold(v);
        return true;
    }
    event_entry_for(v, !(pin & PIN_NMI_EXITING),
                    !(pin & PIN_EXT_INTR_EXITING) && !(proc & PROC_INTR_WINDOW_EXITING));
    uint64_t wanted = proc & windows;
    if (nmi_held) {
        wanted |= PROC_NMI_WINDOW_EXITING;
    }
    vmx_write(VMCS_PROC_CONTROLS, vmx_read(VMCS_PROC_CONTROLS) | wanted);
    return true;
}

bool nvmx_rdmsr(struct vcpu *v, uint32_t msr, uint64_t *value, bool *fault) {
    (void)v;
    *fault = false;
    if (!nvmx_enabled) {
        return false;
    }
    switch (msr) {
        case MSR_IA32_FEATURE_CONTROL: {
            *value = FEATURE_CONTROL_GUEST;
            return true;
        }
        case MSR_IA32_VMX_BASIC: {
            *value = cap_basic;
            return true;
        }
        case MSR_IA32_VMX_PINBASED_CTLS: {
            *value = cap_pin;
            return true;
        }
        case MSR_IA32_VMX_PROCBASED_CTLS: {
            *value = cap_proc;
            return true;
        }
        case MSR_IA32_VMX_EXIT_CTLS: {
            *value = cap_exit;
            return true;
        }
        case MSR_IA32_VMX_ENTRY_CTLS: {
            *value = cap_entry;
            return true;
        }
        case MSR_IA32_VMX_MISC: {
            *value = cap_misc;
            return true;
        }
        case MSR_IA32_VMX_CR0_FIXED0: {
            *value = cap_cr0_fixed0;
            return true;
        }
        case MSR_IA32_VMX_CR0_FIXED1: {
            *value = cap_cr0_fixed1;
            return true;
        }
        case MSR_IA32_VMX_CR4_FIXED0: {
            *value = cap_cr4_fixed0;
            return true;
        }
        case MSR_IA32_VMX_CR4_FIXED1: {
            *value = cap_cr4_fixed1;
            return true;
        }
        case MSR_IA32_VMX_VMCS_ENUM: {
            *value = cap_vmcs_enum;
            return true;
        }
        case MSR_IA32_VMX_PROCBASED_CTLS2: {
            *value = cap_proc2;
            return true;
        }
        case MSR_IA32_VMX_TRUE_PINBASED_CTLS: {
            *value = cap_true_pin;
            return true;
        }
        case MSR_IA32_VMX_TRUE_PROCBASED_CTLS: {
            *value = cap_true_proc;
            return true;
        }
        case MSR_IA32_VMX_TRUE_EXIT_CTLS: {
            *value = cap_true_exit;
            return true;
        }
        case MSR_IA32_VMX_TRUE_ENTRY_CTLS: {
            *value = cap_true_entry;
            return true;
        }
        // It exists only with EPT or VPIDs (SDM 325462-093 Vol. 3D, A.10).
        case MSR_IA32_VMX_EPT_VPID_CAP: {
            *fault = !(cap_proc2 >> 32 & (PROC2_EPT | PROC2_VPID));
            *value = cap_ept_vpid;
            return true;
        }
        // It exists only with VM functions (SDM 325462-093 Vol. 3D, A.11).
        case MSR_IA32_VMX_VMFUNC: {
            *fault = !(cap_proc2 >> 32 & PROC2_VMFUNC);
            *value = cap_vmfunc;
            return true;
        }
        case MSR_IA32_VMX_PROCBASED_CTLS3: {
            *fault = true;
            return true;
        }
        default: {
            return false;
        }
    }
}

bool nvmx_wrmsr(struct vcpu *v, uint32_t msr, uint64_t value, bool *fault) {
    (void)v;
    (void)value;
    *fault = false;
    if (!nvmx_enabled
        || (msr != MSR_IA32_FEATURE_CONTROL
            && (msr < MSR_IA32_VMX_BASIC || msr > MSR_IA32_VMX_PROCBASED_CTLS3))) {
        return false;
    }
    // Locked, and the capability MSRs read-only.
    *fault = true;
    return true;
}

bool nvmx_cr_allowed(struct vcpu *v, unsigned cr, uint64_t value) {
    struct nvmx *n = v->nvmx;
    if (cr == 4 && (value & CR4_VMXE) && !nvmx_enabled) {
        return false;
    }
    if (n == NULL || !n->vmxon) {
        return true;
    }
    // VMX operation keeps CR0 and CR4 to the fixed bits (SDM 325462-093 Vol.
    // 3C, 26.8).
    if (cr == 0) {
        return (value & cap_cr0_fixed0) == cap_cr0_fixed0 && !(value & ~cap_cr0_fixed1);
    }
    return (value & cap_cr4_fixed0) == cap_cr4_fixed0 && !(value & ~cap_cr4_fixed1);
}

bool nvmx_ept_active(struct vcpu *v) {
    return v->nvmx != NULL && v->nvmx->in_l2 && ept12(v->nvmx);
}

bool nvmx_ept_translate(struct vcpu *v, uint64_t gpa, uint64_t access, uint64_t linear,
                        uint64_t *out) {
    struct nvmx *n = v->nvmx;
    if (n->pending) {
        return false;
    }
    struct ept12_walk w;
    enum ept12_result result = ept12_walk(n, gpa, access, &w);
    if (result == EPT12_MAPPED) {
        *out = w.gpa;
        return true;
    }
    n->pending = true;
    n->pending_info = 0;
    n->pending_error = 0;
    n->pending_gpa = gpa;
    n->pending_linear = (access & EPT_QUAL_LINEAR) ? linear : 0;
    if (result == EPT12_MISCONFIG) {
        n->pending_reason = EXIT_REASON_EPT_MISCONFIG;
        n->pending_qual = 0;
    } else if (result == EPT12_PML_FULL) {
        n->pending_reason = EXIT_REASON_PML_FULL;
        n->pending_qual = 0;
    } else {
        n->pending_reason = EXIT_REASON_EPT_VIOLATION;
        n->pending_qual = ept12_qual(w.access, &w);
    }
    return false;
}

bool nvmx_exit_pending(struct vcpu *v) {
    return v->nvmx != NULL && v->nvmx->pending;
}

// SDM 325462-093 Vol. 3C, 28.2: a page fault takes bit 14 of the bitmap where
// its error code matches, else the other way.
bool nvmx_exception(struct vcpu *v, uint8_t vector, bool has_error, uint32_t error, uint64_t qual) {
    struct nvmx *n = v->nvmx;
    if (n == NULL || !n->in_l2) {
        return false;
    }
    if (n->pending) {
        return true;
    }
    bool exits = (get12(n, VMCS_EXCEPTION_BITMAP) >> vector) & 1;
    if (vector == 14) {
        uint32_t mask = (uint32_t)get12(n, VMCS_PF_ERROR_MASK);
        uint32_t match = (uint32_t)get12(n, VMCS_PF_ERROR_MATCH);
        exits = exits == ((error & mask) == match);
    }
    if (!exits) {
        return false;
    }
    n->pending = true;
    n->pending_reason = EXIT_REASON_EXCEPTION_NMI;
    n->pending_info = INTR_INFO_VALID | INTR_TYPE_HW_EXCEPTION | vector;
    n->pending_error = 0;
    if (has_error) {
        n->pending_info |= INTR_INFO_ERROR_CODE;
        n->pending_error = error;
    }
    n->pending_qual = qual;
    n->pending_gpa = 0;
    n->pending_linear = 0;
    return true;
}

// The guest's interrupt with the notification vector of its guest's posted
// interrupts, while its guest runs, is a notification (SDM 325462-093 Vol.
// 3C, 32.6). The vCPU may leave its guest meanwhile and not see it.
bool nvmx_posted_notification(struct vcpu *v, uint8_t vector) {
    struct nvmx *n = v->nvmx;
    if (n == NULL || __atomic_load_n(&n->pi_vector, __ATOMIC_SEQ_CST) != vector) {
        return false;
    }
    __atomic_store_n(&n->pi_pending, true, __ATOMIC_SEQ_CST);
    return __atomic_load_n(&n->pi_vector, __ATOMIC_SEQ_CST) == vector
           || !__atomic_exchange_n(&n->pi_pending, false, __ATOMIC_SEQ_CST);
}

bool nvmx_monitor_trap_flag(struct vcpu *v) {
    struct nvmx *n = v->nvmx;
    return n != NULL && n->in_l2 && (get12(n, VMCS_PROC_CONTROLS) & PROC_MONITOR_TRAP_FLAG);
}

// The processor did not run the instruction, which would have made the exit
// itself, leaving a single-step trap pending (SDM 325462-093 Vol. 3C, 28.5.2).
bool nvmx_monitor_trap(struct vcpu *v, bool single_step) {
    if (!nvmx_monitor_trap_flag(v)) {
        return false;
    }
    struct nvmx *n = v->nvmx;
    if (n->pending) {
        return true;
    }
    if (single_step) {
        vmx_write(VMCS_GUEST_PENDING_DBG, vmx_read(VMCS_GUEST_PENDING_DBG) | PENDING_DBG_BS);
    }
    n->pending = true;
    n->pending_reason = EXIT_REASON_MONITOR_TRAP;
    n->pending_info = 0;
    n->pending_error = 0;
    n->pending_qual = 0;
    n->pending_gpa = 0;
    n->pending_linear = 0;
    return true;
}

void nvmx_arm_timer(struct vcpu *v) {
    struct nvmx *n = v->nvmx;
    if (n == NULL || !n->in_l2 || !(get12(n, VMCS_PIN_CONTROLS) & PIN_PREEMPTION_TIMER)) {
        return;
    }
    uint64_t pin = vmx_read(VMCS_PIN_CONTROLS) & ~(uint64_t)PIN_PREEMPTION_TIMER;
    if (!v->stepping) {
        pin |= PIN_PREEMPTION_TIMER;
        vmx_write(VMCS_PREEMPTION_TIMER, timer_left(n));
    }
    vmx_write(VMCS_PIN_CONTROLS, pin);
}

uint64_t nvmx_ept_pointer(struct vcpu *v) {
    if (!nvmx_ept_active(v)) {
        return 0;
    }
    return ept02_pointer(v->nvmx);
}

void nvmx_gpt_changed(struct vcpu *v) {
    if (v->nvmx != NULL) {
        ept02_drop_all(v->nvmx);
    }
}
