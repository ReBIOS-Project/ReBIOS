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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <virt/msr.h>
#include <core.h>
#include <handoff.h>
#include <lib/heap.h>
#include <lib/spinlock.h>
#include <sys/cpu.h>
#include <sys/fixup.h>
#include <sys/mm.h>
#include <sys/smp.h>
#include <sys/x86.h>
#include <virt/cpuid.h>
#include <virt/layout.h>
#include <virt/msr_table.h>
#include <virt/nvmx.h>
#include <virt/pci_plan.h>
#include <virt/svm.h>
#include <virt/vcpu.h>
#include <virt/virt.h>
#include <virt/vmx.h>

// SDM 325462-093 Vol. 1, Tables 21-16, 21-19, 21-23, and 21-35.
#define CPUID_6_EAX_HW_FEEDBACK ((uint32_t)1 << 19)
#define CPUID_7_EBX_MPX ((uint32_t)1 << 14)
#define CPUID_7_EBX_PT ((uint32_t)1 << 25)
#define CPUID_7_1_EBX_CPUIDMAXVAL_LIM_RMV ((uint32_t)1 << 3)
#define CPUID_D_1_EAX_XSAVES ((uint32_t)1 << 3)

// SDM 325462-093 Vol. 3B, 17.6.4 and 17.6.5.
#define MSR_IA32_HW_FEEDBACK_PTR 0x17d0
#define MSR_IA32_HW_FEEDBACK_CONFIG 0x17d1
#define HW_FEEDBACK_PTR_VALID ((uint32_t)1)
#define HW_FEEDBACK_CONFIG_ENABLE ((uint32_t)1)

// Intel PT's MSRs (SDM 325462-093 Vol. 4, Table 2-2), those from the first
// trigger's to IA32_RTIT_CR3_MATCH taking in IA32_RTIT_CTL and
// IA32_RTIT_STATUS.
#define MSR_IA32_RTIT_OUTPUT_BASE 0x560
#define MSR_IA32_RTIT_OUTPUT_MASK_PTRS 0x561
#define MSR_IA32_RTIT_TRIGGER0_CFG 0x568
#define MSR_IA32_RTIT_CR3_MATCH 0x572
#define MSR_IA32_RTIT_ADDR0_A 0x580
#define MSR_IA32_RTIT_ADDR3_B 0x587

#define MSR_IA32_BNDCFGS 0xd90
#define MSR_IA32_XSS 0xda0

// SDM 325462-093 Vol. 3B, 20.17.3, and Vol. 3A, 13.5.4.1.
#define CPUID_1_ECX_TSC_DEADLINE ((uint32_t)1 << 24)
#define CPUID_7_EBX_TSC_ADJUST ((uint32_t)1 << 1)
#define MSR_IA32_TSC 0x10
#define MSR_IA32_TSC_ADJUST 0x3b
#define MSR_IA32_TSC_DEADLINE 0x6e0

// IA32_MCG_CTL, where IA32_MCG_CAP's MCG_CTL_P says there is one: Intel
// defines only all 0s and all 1s (SDM 325462-093 Vol. 3B, 18.3.1.3), and a
// processor may refuse the rest, as Linux 7.3's KVM does Windows NT 4.0's
// 0x00000000ffffffff. The guest keeps what it wrote, and the processor gets
// the defined value meaning the same. AMD's bits each enable a bank (APM 24593
// rev. 3.45, 9.3.1.3), so there the guest's value is the processor's.
#define MSR_IA32_MCG_CAP 0x179
#define MSR_IA32_MCG_CTL 0x17b
#define MCG_CAP_CTL_P ((uint64_t)1 << 8)

// IA32_FEATURE_CONTROL as the guest sees it (SDM 325462-093 Vol. 4, Table
// 2-2): locked, as vmx_cpu_init() leaves it, VMX outside SMX where the guest
// gets VMX, and the firmware's local machine check enable; not SMX's or SGX's.
#define FEATURE_CONTROL_LMCE_ON ((uint32_t)1 << 20)

// IA32_ARCH_CAPABILITIES' bits the guest sees (SDM 325462-093 Vol. 4, Table
// 2-2): the defined ones, but for the microcode update interface's 9, 16, and
// 22, and 21, XAPIC_DISABLE_STATUS, as the guest's xAPIC mode is never off for
// good. IA32_CORE_CAPABILITIES has only model-specific ones (SDM 325462-093
// Vol. 4, Tables 2-46 and 2-56): 3, RSM_IN_CPL0_ONLY, and 4 and 5, the UC-lock
// and split-lock disables' support; 2 enumerates MSR_INTEGRITY_CAPABILITIES,
// which the guest lacks.
#define MSR_IA32_CORE_CAPABILITIES 0xcf
#define MSR_IA32_ARCH_CAPABILITIES 0x10a
#define ARCH_CAPABILITIES_KNOWN ((uint64_t)0x17f9efdff)
#define CORE_CAPABILITIES_KNOWN ((uint64_t)0x38)

// MSR_PLATFORM_INFO's bits the model tables of SDM 325462-093 Vol. 4 define:
// the ratios, what is programmable, PPIN, the 16K DCU mode, LPM, and the
// ConfigTDP levels, but not BIOS Guard's, PEG2DMIDIS', or SMM's. Bit 31, which
// those tables reserve, is where processors report CPUID faulting (Linux 7.3's
// MSR_PLATFORM_INFO_CPUID_FAULT), kept where MSR_FEATURE_ENABLES is the
// guest's.
#define MSR_PLATFORM_INFO 0xce
#define MSR_FEATURE_ENABLES 0x140
#define PLATFORM_INFO_KNOWN ((uint64_t)0x00ffff077480ff00)
#define PLATFORM_INFO_CPUID_FAULT ((uint64_t)1 << 31)

// IA32_PERF_CAPABILITIES' fields the guest sees: those SDM 325462-093 Vol. 3B,
// 22.8 defines (bit 18, TSX_ADDRESS, only SDM 325462-093 Vol. 4, Table 2-2
// names), but for the performance metrics where the guest lacks
// MSR_PERF_METRICS, and PEBS output into Intel PT where it lacks PT.
#define CPUID_1_ECX_PDCM ((uint32_t)1 << 15)
#define MSR_IA32_PERF_CAPABILITIES 0x345
#define PERF_CAPABILITIES_KNOWN ((uint64_t)0xbffff)
#define PERF_CAPABILITIES_PERF_METRICS ((uint64_t)1 << 15)
#define PERF_CAPABILITIES_PEBS_OUTPUT_PT ((uint64_t)1 << 16)
#define MSR_PERF_METRICS 0x329

// PEBS writes its records to the guest's DS area, so the guest gets it only
// where the processor's accesses to it go through EPT (SDM 325462-093 Vol. 3B,
// 22.9.5) and exits stop the counters; elsewhere IA32_MISC_ENABLE reports it
// unavailable, IA32_PERF_CAPABILITIES has none of its fields, and
// IA32_PEBS_ENABLE raises #GP. Such are the processors from Ice Lake Xeon and
// 12th generation Core on (SDM 325462-093 Vol. 3B, 22.3.9.1 and 22.9.5): those
// of record formats 5 and later (SDM 325462-093 Vol. 3B, Tables 22-57 and
// 22-59), and those of format 4 among them (SDM 325462-093 Vol. 3B, Tables
// 22-51 and 22-53), by their signatures in SDM 325462-093 Vol. 4, Table 2-1.
#define CPUID_1_EDX_DS ((uint32_t)1 << 21)
#define MISC_ENABLE_PEBS_UNAVAILABLE ((uint64_t)1 << 12)
#define PERF_CAPABILITIES_PEBS \
    ((uint64_t)3 << 6 | (uint64_t)0xf << 8 | (uint64_t)1 << 14 \
        | (uint64_t)3 << 16)
#define PERF_CAPABILITIES_PEBS_FORMAT(c) (((c) >> 8) & 0xf)
#define MSR_IA32_PEBS_ENABLE 0x3f1

// AMD's microcode patch loader, which loads the patch at the linear address
// written (Linux 7.3's MSR_AMD64_PATCH_LOADER): the guest's writes load
// nothing, as VMX has Intel's do (SDM 325462-093 Vol. 3C, 28.3), so that the
// processor does not change under the hypervisor.
#define MSR_AMD_PATCH_LOADER 0xc0010020

// AMD's hardware configuration (APM 24593 rev. 3.45, 3.2.10 and 15.32.1; PPR
// 56713 rev. 3.05, MSRC001_0015): the processor's, with SMM locked, as the
// guest has none, and SmmLock's writes ignored, as locked ones are. The guest
// may change core performance boost, the instructions retired counter, and
// machine check status writes, which Linux 7.3's machine check code turns on
// (arch/x86/kernel/cpu/mce/amd.c).
#define MSR_AMD_HWCR 0xc0010015
#define HWCR_SMM_LOCK ((uint32_t)1 << 0)
#define HWCR_MC_STATUS_WR_EN ((uint32_t)1 << 18)
#define HWCR_CPB_DIS ((uint32_t)1 << 25)
#define HWCR_IR_PERF_EN ((uint32_t)1 << 30)
#define HWCR_GUEST (HWCR_MC_STATUS_WR_EN | HWCR_CPB_DIS | HWCR_IR_PERF_EN)

// AMD's TSC counts at P0's frequency (BKDG 42301 rev. 3.14, MSR0000_0010), so
// the guest's writes to the P-state definitions would change the rate the
// hypervisor times itself by, but where Family 10h's TscFreqSel or Zen's
// LockTscToCurrentP0 holds it at the frequency P0 had when set (BKDG 31116
// rev. 3.62, PPR 55772 rev. 3.08, MSRC001_0015). Families 15h and 16h hold it
// at none: their definitions are read-only.
#define MSR_AMD_PSTATE_DEF0 0xc0010064
#define MSR_AMD_PSTATE_DEF7 0xc001006b
#define HWCR_LOCK_TSC_TO_CURRENT_P0 ((uint32_t)1 << 21)

// AMD's node ID (BKDG 31116 rev. 3.62, MSRC001_100C), read-only: the
// northbridge routes by what firmware wrote there.
#define MSR_AMD_NODE_ID 0xc001100c

// AMD's northbridge configuration (BKDG 31116 rev. 3.62, 42301 rev. 3.14,
// MSRC001_001F): the processor's, of which the guest may change only
// EnableCf8ExtCfg, which Linux 7.3 sets on every family for extended
// configuration space through CF8 and CFC (arch/x86/pci/amd_bus.c); the rest
// sets up the northbridge for all cores.
#define MSR_AMD_NB_CFG 0xc001001f
#define NB_CFG_ENABLE_CF8_EXT_CFG ((uint64_t)1 << 46)

// AMD's CPUID override MSRs (PPR 55772 rev. 3.08, MSRC001_1002 to
// MSRC001_1005): the guest's copies of the processor's, in which it may clear
// bits and set back those the processor had, for its CPUID to hide them
// (cpuid.c). The processor's would change what CPUID tells the hypervisor.
#define MSR_AMD_CPUID_7_FEATURES 0xc0011002
#define MSR_AMD_CPUID_EXT_FEATURES 0xc0011005

// The last branch records, which only LBR virtualisation keeps the guest's
// (APM 24593 rev. 3.45, 13.1.1.6, 13.1.1.7, and 15.23): hidden where SVM lacks
// it, as they would record the hypervisor's branches. DebugCtl's other bits,
// on in the hypervisor too there, act on nothing it does at CPL 0 without TF.
#define MSR_DEBUGCTL 0x1d9
#define MSR_LAST_BRANCH_FROM_IP 0x1db
#define MSR_LAST_INT_TO_IP 0x1de
#define DEBUGCTL_LBR ((uint32_t)1)

// The assignable bandwidth monitoring configuration (APM 24593 rev. 3.45,
// 19.4.5): SDCI allocation enforcement's bit as reserved, as the guest does
// not get it.
#define MSR_AMD_L3_QOS_EXT_CFG 0xc00003ff
#define L3_QOS_EXT_CFG_SDCIAE ((uint32_t)1 << 1)

// AMD's core performance event selects, whose host/guest-only field the guest
// would set to no effect, its EFER.SVME being clear (APM 24593 rev. 3.45,
// 13.2.1, Table 13-4): the processor's count in the guest alone, and the guest
// sees its own values. Counters 0 to 3 have the legacy addresses too.
#define MSR_AMD_PERF_EVT_SEL0 0xc0010000
#define MSR_AMD_PERF_EVT_SEL3 0xc0010003
#define MSR_AMD_PERF_CTL0 0xc0010200
#define MSR_AMD_PERF_CTL5 0xc001020a
#define PERF_EVT_SEL_HG_ONLY ((uint64_t)3 << 40)
#define PERF_EVT_SEL_GUEST_ONLY ((uint64_t)1 << 40)

// AMD's memory routing (APM 24593 rev. 3.45, 3.2.1, 7.9.2, and 7.9.4; PPR
// 55772 rev. 3.08, MSRC001_0058): the guest's own, from its memory map, so
// that none of its writes routes the host's memory. SMM and SVM appear locked,
// as the guest has neither (APM 24593 rev. 3.45, 15.30.1, 15.30.3, 15.31.1,
// and 15.32).
#define MSR_AMD_SYSCFG 0xc0010010
#define MSR_AMD_IORR_BASE0 0xc0010016
#define MSR_AMD_IORR_MASK1 0xc0010019
#define MSR_AMD_TOP_MEM 0xc001001a
#define MSR_AMD_TOP_MEM2 0xc001001d
#define MSR_AMD_MMIO_CFG_BASE 0xc0010058
#define MSR_AMD_SMM_ADDR 0xc0010112
#define MSR_AMD_SMM_MASK 0xc0010113
#define MSR_AMD_SMM_CTL 0xc0010116
#define MSR_AMD_SVM_KEY 0xc0010118
#define MSR_AMD_SMM_KEY 0xc0010119
#define SYSCFG_MTRR_FIX_DRAM_EN ((uint32_t)1 << 18)
#define SYSCFG_MTRR_VAR_DRAM_EN ((uint32_t)1 << 20)
#define SYSCFG_MTRR_TOM2_EN ((uint32_t)1 << 21)
#define SYSCFG_TOM2_FORCE_WB ((uint32_t)1 << 22)
// MtrrFixDramEn to Tom2ForceMemTypeWB.
#define SYSCFG_GUEST ((uint32_t)0x1f << 18)
#define IORR_BASE_RD_WR ((uint32_t)3 << 3)
#define IORR_MASK_VALID ((uint32_t)1 << 11)
#define FOUR_GIB ((uint64_t)0x100000000)
#define TOP_MEM_UNIT ((uint64_t)8 << 20)
#define MMIO_CFG_ENABLE ((uint32_t)1)
#define MMIO_CFG_BUS_RANGE ((uint32_t)0x3c)
#define MMIO_CFG_BASE_MASK ((uint64_t)0x0000fffffff00000)

// IA32_MISC_ENABLE's bits the processor's keep clear, as the CPUID limit would
// hide leaves the hypervisor reads and XD disable would make EFER.NXE, which
// the host's and the guest's EFER may have set, reserved; either may also be
// shared with other logical processors (SDM 325462-093 Vol. 4, Tables 2-2 and
// 2-3). The AP start-up code clears them on the other processors.
#define MISC_ENABLE_COPIED (MISC_ENABLE_LIMIT_CPUID | MISC_ENABLE_XD_DISABLE)

static bool misc_enable_copied;
// The firmware's settings of those bits, which the guest starts with, and
// those the processor has. The cpuid_limit setting gives the guest the limit
// even where the processor lacks it, and then the guest can neither see nor
// clear it.
static uint64_t misc_enable_firmware;
static uint64_t misc_enable_supported;
static bool mcg_ctl;

// The hardware feedback interface's table, which the processor writes to at
// the physical address a package's IA32_HW_FEEDBACK_PTR holds: the host
// address of the guest's table goes there, and what the guest wrote is kept
// for each package.
struct hfi_package {
    uint32_t id;
    uint64_t guest;
};
static bool hfi;
static size_t hfi_pages;
static uint64_t hfi_reserved;
static unsigned int package_shift;
static struct spinlock hfi_lock;
static struct hfi_package *hfi_packages;
static size_t hfi_package_count;

// Intel PT where it cannot be the guest's: its output would go to host
// addresses, and its tracing would go on through the hypervisor's own code.
static bool pt_hidden;
// MPX where its supervisor configuration would stay on in the hypervisor
// (vmx.c).
static bool mpx_hidden;
// IA32_XSS, which exists where XSAVES does.
static bool xss;
static bool lbr_hidden;
// IA32_PERF_GLOBAL_CTRL where exits clear it but cannot save the guest's
// (vmx.c): the guest's is in the VMCS for entries to load, with the
// processor's reserved bits, which a write in the hypervisor, undone at once,
// checks.
static bool perf_global_kept;
static bool perf_capabilities;
static bool pebs_hidden;
static bool pstates_read_only;
// The guest's TSC moves only by an offset of the vCPU's (vmx.c, svm.c), as the
// processor's is the hypervisor's clock: whether the processor has
// IA32_TSC_ADJUST and the TSC-deadline timer. That timer's deadline stays the
// processor's TSC's whatever the offset (SDM 325462-093 Vol. 3C, 28.3, RDMSR),
// which the guest reaches until an offset is no longer 0, and from then on
// through the hypervisor.
static bool tsc_adjust;
static bool tsc_deadline;
static bool deadline_translated;
// The tables of the processor's model-specific MSRs, where it has any.
static const struct msr_model *model;
static const struct msr_family *family_msrs;
// Physical address bits the processor lacks, for the guest's AMD addresses.
static uint64_t address_reserved;

static void misc_enable_init(void) {
    // IA32_MISC_ENABLE is Intel's, and VMX runs only on Intel's processors.
    uint64_t misc;
    if (!virt_vmx || !rdmsr_safe(MSR_IA32_MISC_ENABLE, &misc)) {
        return;
    }
    misc_enable_firmware = misc & MISC_ENABLE_COPIED;
    if (misc_enable_firmware != 0) {
        wrmsr(MSR_IA32_MISC_ENABLE, misc & ~MISC_ENABLE_COPIED);
    }
    // The limit where leaves go past 2 and it can still be set; XD disable is
    // model-specific, known to exist only where the firmware set it.
    uint32_t max = cpuid(0, 0).eax;
    if (max > 2
     && (max < 7
      || (cpuid(7, 1).ebx & CPUID_7_1_EBX_CPUIDMAXVAL_LIM_RMV) == 0)) {
        misc_enable_supported |= MISC_ENABLE_LIMIT_CPUID;
    }
    misc_enable_supported |= misc_enable_firmware & MISC_ENABLE_XD_DISABLE;
    misc_enable_copied = true;
    virt_intercept_msr(MSR_IA32_MISC_ENABLE, true, true);
}

static void mcg_ctl_init(void) {
    uint64_t cap;
    if (virt_vmx && rdmsr_safe(MSR_IA32_MCG_CAP, &cap)
     && (cap & MCG_CAP_CTL_P) != 0) {
        mcg_ctl = true;
        virt_intercept_msr(MSR_IA32_MCG_CTL, true, true);
    }
}

static void hfi_init(void) {
    if (cpuid(0, 0).eax < 6
     || (cpuid(6, 0).eax & CPUID_6_EAX_HW_FEEDBACK) == 0) {
        return;
    }
    hfi = true;
    hfi_pages = ((cpuid(6, 0).edx >> 8) & 0xf) + 1;
    hfi_reserved = 0xffe | ~(((uint64_t)1 << cpu_physical_address_bits()) - 1);
    package_shift = cpu_package_shift();
    virt_intercept_msr(MSR_IA32_HW_FEEDBACK_PTR, true, true);
    virt_intercept_msr(MSR_IA32_HW_FEEDBACK_CONFIG, false, true);
}

static bool pt_msr(uint32_t msr) {
    return msr == MSR_IA32_RTIT_OUTPUT_BASE
        || msr == MSR_IA32_RTIT_OUTPUT_MASK_PTRS
        || (msr >= MSR_IA32_RTIT_TRIGGER0_CFG
            && msr <= MSR_IA32_RTIT_CR3_MATCH)
        || (msr >= MSR_IA32_RTIT_ADDR0_A && msr <= MSR_IA32_RTIT_ADDR3_B);
}

static void pt_init(void) {
    if (!virt_vmx || vmx_pt_passthrough() || cpuid(0, 0).eax < 7
     || (cpuid(7, 0).ebx & CPUID_7_EBX_PT) == 0) {
        return;
    }
    pt_hidden = true;
    for (size_t msr = MSR_IA32_RTIT_OUTPUT_BASE; msr <= MSR_IA32_RTIT_ADDR3_B;
         msr++) {
        if (pt_msr(msr)) {
            virt_intercept_msr(msr, true, true);
        }
    }
}

static void mpx_init(void) {
    if (!virt_vmx || vmx_mpx_passthrough() || cpuid(0, 0).eax < 7
     || (cpuid(7, 0).ebx & CPUID_7_EBX_MPX) == 0) {
        return;
    }
    mpx_hidden = true;
    virt_intercept_msr(MSR_IA32_BNDCFGS, true, true);
}

static void perf_capabilities_init(void) {
    uint64_t capabilities;
    perf_capabilities = (cpuid(1, 0).ecx & CPUID_1_ECX_PDCM) != 0
        && rdmsr_safe(MSR_IA32_PERF_CAPABILITIES, &capabilities);
    if (perf_capabilities) {
        virt_intercept_msr(MSR_IA32_PERF_CAPABILITIES, true, false);
    }
}

// Ice Lake Xeon, Sapphire and Emerald Rapids, Alder and Raptor Lake, and the
// Gracemont N-series, all of family 6.
static const uint8_t pebs_format_4_models[] = {
    0x6a, 0x6c, 0x8f, 0xcf, 0x97, 0x9a, 0xb7, 0xba, 0xbf, 0xbe,
};

static bool pebs_ept_friendly(void) {
    uint64_t capabilities;
    if (perf_capabilities
     && rdmsr_safe(MSR_IA32_PERF_CAPABILITIES, &capabilities)
     && PERF_CAPABILITIES_PEBS_FORMAT(capabilities) >= 5) {
        return true;
    }
    struct cpu_signature s = cpu_signature();
    for (size_t i = 0; i < sizeof(pebs_format_4_models); i++) {
        if (s.family == 6 && s.model == pebs_format_4_models[i]) {
            return true;
        }
    }
    return false;
}

static void pebs_init(void) {
    uint64_t misc;
    if ((cpuid(1, 0).edx & CPUID_1_EDX_DS) == 0
     || !rdmsr_safe(MSR_IA32_MISC_ENABLE, &misc)
     || (misc & MISC_ENABLE_PEBS_UNAVAILABLE) != 0) {
        return;
    }
    if (!pebs_ept_friendly() || !vmx_perf_global_switched()) {
        pebs_hidden = true;
        virt_intercept_msr(MSR_IA32_PEBS_ENABLE, true, true);
    }
}

// The supervisor state components XSAVES and XRSTORS handle: only those the
// guest gets, as XRSTORS would load PT's IA32_RTIT_CTL, for one.
static void xss_init(void) {
    if (cpuid(0, 0).eax >= 0xd
     && (cpuid(0xd, 1).eax & CPUID_D_1_EAX_XSAVES) != 0) {
        xss = true;
        virt_intercept_msr(MSR_IA32_XSS, false, true);
    }
}

static void tsc_init(void) {
    virt_intercept_msr(MSR_IA32_TSC, true, true);
    if (cpuid(0, 0).eax >= 7
     && (cpuid(7, 0).ebx & CPUID_7_EBX_TSC_ADJUST) != 0) {
        tsc_adjust = true;
        virt_intercept_msr(MSR_IA32_TSC_ADJUST, true, true);
    }
    tsc_deadline = (cpuid(1, 0).ecx & CPUID_1_ECX_TSC_DEADLINE) != 0;
}

// The bitmaps change while no other vCPU runs (SDM 325462-093 Vol. 3C,
// 27.11.4), and an armed deadline stays where the guest's TSC has it.
static void tsc_offset_set(struct vcpu *v, uint64_t offset) {
    if (offset == v->tsc_offset) {
        return;
    }
    if (tsc_deadline
     && !__atomic_load_n(&deadline_translated, __ATOMIC_ACQUIRE)) {
        vcpu_pause_others();
        if (!deadline_translated) {
            virt_intercept_msr(MSR_IA32_TSC_DEADLINE, true, true);
            __atomic_store_n(&deadline_translated, true, __ATOMIC_RELEASE);
        }
        vcpu_resume_others();
    }
    uint64_t deadline = 0;
    if (tsc_deadline && rdmsr_safe(MSR_IA32_TSC_DEADLINE, &deadline)
     && deadline != 0) {
        uint64_t moved = deadline + v->tsc_offset - offset;
        wrmsr_safe(MSR_IA32_TSC_DEADLINE, moved != 0 ? moved : 1);
    }
    v->tsc_offset = offset;
}

void msr_init(void) {
    // First, as the CPUID limit hides the leaves the rest reads.
    misc_enable_init();
    tsc_init();
    mcg_ctl_init();
    hfi_init();
    pt_init();
    mpx_init();
    xss_init();
    if (!virt_vmx) {
        virt_intercept_msr(MSR_AMD_PATCH_LOADER, false, true);
        if (svm_erratum_383) {
            virt_intercept_msr(MSR_DC_CFG, false, true);
        }
        virt_intercept_msr(MSR_AMD_HWCR, true, true);
        uint32_t family = cpu_signature().family;
        if (family == 0x15 || family == 0x16) {
            pstates_read_only = true;
            for (size_t msr = MSR_AMD_PSTATE_DEF0; msr <= MSR_AMD_PSTATE_DEF7;
                 msr++) {
                virt_intercept_msr(msr, false, true);
            }
        }
        if (!svm_lbr_virtualisation) {
            lbr_hidden = true;
            virt_intercept_msr(MSR_DEBUGCTL, false, true);
            for (size_t msr = MSR_LAST_BRANCH_FROM_IP;
                 msr <= MSR_LAST_INT_TO_IP; msr++) {
                virt_intercept_msr(msr, true, true);
            }
        }
        virt_intercept_msr(MSR_AMD_NB_CFG, true, true);
        virt_intercept_msr(MSR_AMD_L3_QOS_EXT_CFG, false, true);
        virt_intercept_msr(MSR_AMD_NODE_ID, false, true);
        for (size_t msr = MSR_AMD_PERF_EVT_SEL0; msr <= MSR_AMD_PERF_EVT_SEL3;
             msr++) {
            virt_intercept_msr(msr, true, true);
        }
        for (size_t msr = MSR_AMD_PERF_CTL0; msr <= MSR_AMD_PERF_CTL5;
             msr += 2) {
            virt_intercept_msr(msr, true, true);
        }
        for (size_t msr = MSR_AMD_CPUID_7_FEATURES;
             msr <= MSR_AMD_CPUID_EXT_FEATURES; msr++) {
            virt_intercept_msr(msr, true, true);
        }
        address_reserved = ~(((uint64_t)1 << cpu_physical_address_bits()) - 1)
            & (((uint64_t)1 << 52) - 1);
        virt_intercept_msr(MSR_AMD_SYSCFG, true, true);
        for (size_t msr = MSR_AMD_IORR_BASE0; msr <= MSR_AMD_IORR_MASK1;
             msr++) {
            virt_intercept_msr(msr, true, true);
        }
        virt_intercept_msr(MSR_AMD_TOP_MEM, true, true);
        virt_intercept_msr(MSR_AMD_TOP_MEM2, true, true);
        virt_intercept_msr(MSR_AMD_MMIO_CFG_BASE, true, true);
        virt_intercept_msr(MSR_AMD_SMM_ADDR, true, true);
        virt_intercept_msr(MSR_AMD_SMM_MASK, true, true);
        virt_intercept_msr(MSR_AMD_SMM_CTL, true, true);
        virt_intercept_msr(MSR_AMD_SVM_KEY, true, true);
        virt_intercept_msr(MSR_AMD_SMM_KEY, true, true);
    }
    if (virt_vmx) {
        perf_capabilities_init();
        pebs_init();
    }
    if (virt_vmx && vmx_perf_global_switched() && !vmx_perf_global_saved()) {
        perf_global_kept = true;
        virt_intercept_msr(MSR_IA32_PERF_GLOBAL_CTRL, true, true);
    }
    if (virt_vmx) {
        virt_intercept_msr(MSR_IA32_FEATURE_CONTROL, true, true);
        virt_intercept_msr(MSR_IA32_CORE_CAPABILITIES, true, false);
        virt_intercept_msr(MSR_IA32_ARCH_CAPABILITIES, true, false);
        virt_intercept_msr(MSR_PLATFORM_INFO, true, false);
    }
}

void msr_cpu_init(void) {
    uint32_t family = cpu_signature().family;
    uint64_t hwcr;
    if (!virt_vmx && (family == 0x17 || family == 0x19 || family == 0x1a)
     && rdmsr_safe(MSR_AMD_HWCR, &hwcr)
     && (hwcr & HWCR_LOCK_TSC_TO_CURRENT_P0) == 0) {
        wrmsr_safe(MSR_AMD_HWCR, hwcr | HWCR_LOCK_TSC_TO_CURRENT_P0);
    }
}

static bool in_ranges(
    const struct msr_range *ranges, size_t count, uint32_t msr
) {
    for (size_t i = 0; i < count; i++) {
        if (msr >= ranges[i].first && msr <= ranges[i].last) {
            return true;
        }
    }
    return false;
}

bool msr_allowed(uint32_t msr) {
    if (!virt_vmx) {
        return in_ranges(
            msr_amd_architectural, msr_amd_architectural_count, msr
        )
            || (family_msrs != NULL
                && in_ranges(family_msrs->ranges, family_msrs->count, msr));
    }
    bool architectural = in_ranges(
        msr_intel_architectural, msr_intel_architectural_count, msr
    );
    if (architectural) {
        return true;
    }
    for (size_t i = 0; model != NULL && i < model->count; i++) {
        bool in_table = in_ranges(
            model->tables[i]->ranges, model->tables[i]->count, msr
        );
        if (in_table) {
            return true;
        }
    }
    return false;
}

static void pass_ranges(const struct msr_range *ranges, size_t count) {
    for (size_t i = 0; i < count; i++) {
        for (size_t msr = ranges[i].first; msr <= ranges[i].last; msr++) {
            virt_pass_msr((uint32_t)msr);
        }
    }
}

void msr_allowed_init(void) {
    struct cpu_signature s = cpu_signature();
    if (!virt_vmx) {
        // Hygon's are in none of AMD's families.
        for (size_t i = 0; i < msr_amd_family_count; i++) {
            if (msr_amd_families[i].family == s.family) {
                family_msrs = &msr_amd_families[i];
            }
        }
        pass_ranges(msr_amd_architectural, msr_amd_architectural_count);
        if (family_msrs != NULL) {
            pass_ranges(family_msrs->ranges, family_msrs->count);
        }
        return;
    }
    for (size_t i = 0; i < msr_intel_model_count; i++) {
        if (msr_intel_models[i].family == s.family
         && msr_intel_models[i].model == s.model) {
            model = &msr_intel_models[i];
        }
    }
    pass_ranges(msr_intel_architectural, msr_intel_architectural_count);
    for (size_t i = 0; model != NULL && i < model->count; i++) {
        pass_ranges(model->tables[i]->ranges, model->tables[i]->count);
    }
}

// The guest's IA32_HW_FEEDBACK_PTR for the processor's package, made if
// create. Needs hfi_lock.
static struct hfi_package *hfi_package(bool create) {
    uint32_t id = this_cpu()->apic_id >> package_shift;
    for (size_t i = 0; i < hfi_package_count; i++) {
        if (hfi_packages[i].id == id) {
            return &hfi_packages[i];
        }
    }
    if (!create) {
        return NULL;
    }
    if (hfi_packages == NULL) {
        hfi_packages = kmalloc(smp_cpu_count() * sizeof(struct hfi_package));
    }
    hfi_packages[hfi_package_count] = (struct hfi_package){ id, 0 };
    return &hfi_packages[hfi_package_count++];
}

// The pointer the processor gets: the host address of the guest's table, or
// none where the guest's RAM does not hold it.
static uint64_t hfi_host_pointer(uint64_t value) {
    uintptr_t gpa = value & ~(uint64_t)HW_FEEDBACK_PTR_VALID;
    uintptr_t hpa;
    if ((value & HW_FEEDBACK_PTR_VALID) == 0
     || !layout_ram(gpa, hfi_pages * PAGE_SIZE)
     || !layout_contiguous(gpa, hfi_pages, &hpa)) {
        return 0;
    }
    return hpa | HW_FEEDBACK_PTR_VALID;
}

// What a BIOS sets up for the guest's memory map: memory to its PCI hole and
// above 4 GiB to its RAM's end, write-back, both on 8 MiB boundaries, and its
// configuration space window, from bus 0, as a power of two buses.
static void amd_memory_init(struct vcpu *v) {
    const struct pci_plan *plan = pci_plan();
    v->amd_top_mem = (plan != NULL ? plan->tolud : FOUR_GIB)
        & ~(TOP_MEM_UNIT - 1);
    v->amd_top_mem2 = layout_high_ram_end() & ~(TOP_MEM_UNIT - 1);
    v->amd_syscfg = SYSCFG_MTRR_FIX_DRAM_EN | SYSCFG_MTRR_VAR_DRAM_EN;
    if (v->amd_top_mem2 > FOUR_GIB) {
        v->amd_syscfg |= SYSCFG_MTRR_TOM2_EN | SYSCFG_TOM2_FORCE_WB;
    } else {
        v->amd_top_mem2 = 0;
    }
    for (size_t i = 0; i < 4; i++) {
        v->amd_iorr[i] = 0;
    }
    v->amd_mmio_cfg_base = 0;
    if (plan != NULL && plan->ecam != 0
     && plan->ecam >= (uint64_t)plan->ecam_start << 20) {
        uint64_t range = 0;
        while (((uint32_t)1 << range) <= plan->ecam_end) {
            range++;
        }
        v->amd_mmio_cfg_base
            = (plan->ecam - ((uint64_t)plan->ecam_start << 20)) | range << 2
            | MMIO_CFG_ENABLE;
    }
}

// The counter an AMD event select MSR is for, or -1.
static int amd_perf_counter(uint32_t msr) {
    if (msr >= MSR_AMD_PERF_EVT_SEL0 && msr <= MSR_AMD_PERF_EVT_SEL3) {
        return (int)(msr - MSR_AMD_PERF_EVT_SEL0);
    }
    if (msr >= MSR_AMD_PERF_CTL0 && msr <= MSR_AMD_PERF_CTL5 && msr % 2 == 0) {
        return (int)(msr - MSR_AMD_PERF_CTL0) / 2;
    }
    return -1;
}

void msr_vcpu_init(struct vcpu *v) {
    v->misc_enable = misc_enable_firmware;
    if (core_handoff->config.cpuid_limit != 0) {
        v->misc_enable |= MISC_ENABLE_LIMIT_CPUID;
    }
    v->tsc_offset = 0;
    v->tsc_adjust = 0;
    if (tsc_adjust) {
        rdmsr_safe(MSR_IA32_TSC_ADJUST, &v->tsc_adjust);
    }
    v->mcg_ctl = 0;
    if (mcg_ctl) {
        rdmsr_safe(MSR_IA32_MCG_CTL, &v->mcg_ctl);
    }
    if (!virt_vmx) {
        amd_memory_init(v);
        // As the firmware left them, from the vCPU's own processor.
        for (size_t i = 0; i < 6; i++) {
            uint32_t msr = i < 4 ? MSR_AMD_PERF_EVT_SEL0 + i
                : MSR_AMD_PERF_CTL0 + 2 * i;
            v->amd_perf_evt_sel[i] = 0;
            if (rdmsr_safe(msr, &v->amd_perf_evt_sel[i])) {
                wrmsr_safe(
                    msr,
                    (v->amd_perf_evt_sel[i] & ~PERF_EVT_SEL_HG_ONLY)
                        | PERF_EVT_SEL_GUEST_ONLY
                );
            }
        }
        // From the processor the vCPU is created on, which is its own.
        v->amd_cpuid_present = 0;
        for (size_t i = 0; i < 4; i++) {
            bool readable = rdmsr_safe(
                MSR_AMD_CPUID_7_FEATURES + i, &v->amd_cpuid_hardware[i]
            );
            if (readable) {
                v->amd_cpuid_present |= (uint32_t)1 << i;
            }
            v->amd_cpuid[i] = v->amd_cpuid_hardware[i];
        }
    }
}


// The guest's AMD memory routing MSRs, and those of SMM and SVM that it may
// read; true where the MSR is one.
static bool amd_memory_rdmsr(
    struct vcpu *v, uint32_t msr, uint64_t *value, bool *fault
) {
    *fault = false;
    if (msr == MSR_AMD_SYSCFG) {
        *value = v->amd_syscfg;
    } else if (msr >= MSR_AMD_IORR_BASE0 && msr <= MSR_AMD_IORR_MASK1) {
        *value = v->amd_iorr[msr - MSR_AMD_IORR_BASE0];
    } else if (msr == MSR_AMD_TOP_MEM) {
        *value = v->amd_top_mem;
    } else if (msr == MSR_AMD_TOP_MEM2) {
        *value = v->amd_top_mem2;
    } else if (msr == MSR_AMD_MMIO_CFG_BASE) {
        *value = v->amd_mmio_cfg_base;
    } else if (msr == MSR_AMD_SMM_ADDR || msr == MSR_AMD_SMM_MASK
            || msr == MSR_AMD_SVM_KEY || msr == MSR_AMD_SMM_KEY) {
        // No SMRAM, and keys read as 0.
        *value = 0;
    } else if (msr == MSR_AMD_SMM_CTL) {
        // Write-only.
        *fault = true;
    } else {
        return false;
    }
    return true;
}

static bool amd_memory_wrmsr(
    struct vcpu *v, uint32_t msr, uint64_t value, bool *fault
) {
    *fault = false;
    if (msr == MSR_AMD_SYSCFG) {
        *fault = ((value ^ v->amd_syscfg) & ~(uint64_t)SYSCFG_GUEST) != 0;
        if (!*fault) {
            v->amd_syscfg = value;
        }
    } else if (msr >= MSR_AMD_IORR_BASE0 && msr <= MSR_AMD_IORR_MASK1) {
        bool base = (msr - MSR_AMD_IORR_BASE0) % 2 == 0;
        uint64_t defined = ((((uint64_t)1 << 52) - 1) & ~(uint64_t)0xfff)
            | (base ? IORR_BASE_RD_WR : IORR_MASK_VALID);
        *fault = (value & address_reserved) != 0;
        if (!*fault) {
            v->amd_iorr[msr - MSR_AMD_IORR_BASE0] = value & defined;
        }
    } else if (msr == MSR_AMD_TOP_MEM || msr == MSR_AMD_TOP_MEM2) {
        uintptr_t top = value & (((uint64_t)1 << 52) - 1)
            & ~(TOP_MEM_UNIT - 1);
        *fault = (value & address_reserved) != 0;
        if (!*fault && msr == MSR_AMD_TOP_MEM) {
            v->amd_top_mem = top;
        } else if (!*fault) {
            v->amd_top_mem2 = top;
        }
    } else if (msr == MSR_AMD_MMIO_CFG_BASE) {
        v->amd_mmio_cfg_base = value
            & (MMIO_CFG_BASE_MASK | MMIO_CFG_BUS_RANGE | MMIO_CFG_ENABLE);
    } else if (msr == MSR_AMD_SMM_ADDR || msr == MSR_AMD_SMM_MASK
            || msr == MSR_AMD_SMM_CTL) {
        // Locked, as SmmLock has them.
        *fault = true;
    } else if (msr == MSR_AMD_SVM_KEY || msr == MSR_AMD_SMM_KEY) {
        // Keys that match none, as a zero key does.
    } else {
        return false;
    }
    return true;
}

static bool lbr_msr(uint32_t msr) {
    return lbr_hidden && msr >= MSR_LAST_BRANCH_FROM_IP
        && msr <= MSR_LAST_INT_TO_IP;
}

bool msr_rdmsr(struct vcpu *v, uint32_t msr, uint64_t *value, bool *fault) {
    if ((pt_hidden && pt_msr(msr)) || (mpx_hidden && msr == MSR_IA32_BNDCFGS)
     || lbr_msr(msr) || (pebs_hidden && msr == MSR_IA32_PEBS_ENABLE)) {
        *fault = true;
        return true;
    }
    if (!virt_vmx && amd_memory_rdmsr(v, msr, value, fault)) {
        return true;
    }
    if (!virt_vmx && msr >= MSR_AMD_CPUID_7_FEATURES
     && msr <= MSR_AMD_CPUID_EXT_FEATURES) {
        size_t i = msr - MSR_AMD_CPUID_7_FEATURES;
        *fault = ((v->amd_cpuid_present >> i) & 1) == 0;
        if (!*fault) {
            *value = v->amd_cpuid[i];
        }
        return true;
    }
    int counter = virt_vmx ? -1 : amd_perf_counter(msr);
    if (counter >= 0) {
        uint64_t hardware;
        *fault = !rdmsr_safe(msr, &hardware);
        *value = v->amd_perf_evt_sel[counter];
        return true;
    }
    switch (msr) {
        case MSR_IA32_TSC: {
            *value = nvmx_tsc(v, rdtsc());
            return true;
        }
        case MSR_IA32_TSC_ADJUST: {
            if (!tsc_adjust) {
                return false;
            }
            *value = v->tsc_adjust;
            return true;
        }
        case MSR_IA32_TSC_DEADLINE: {
            if (!tsc_deadline) {
                return false;
            }
            uint64_t deadline = 0;
            *fault = !rdmsr_safe(msr, &deadline);
            *value = deadline != 0 ? deadline + v->tsc_offset : 0;
            return true;
        }
        case MSR_IA32_FEATURE_CONTROL: {
            if (!virt_vmx) {
                return false;
            }
            *fault = !rdmsr_safe(msr, value);
            *value = (*value & FEATURE_CONTROL_LMCE_ON) | FEATURE_CONTROL_LOCK
                | (nvmx_enabled ? FEATURE_CONTROL_VMX_OUTSIDE_SMX : 0);
            return true;
        }
        case MSR_IA32_CORE_CAPABILITIES:
        case MSR_IA32_ARCH_CAPABILITIES: {
            if (!virt_vmx) {
                return false;
            }
            *fault = !rdmsr_safe(msr, value);
            *value &= msr == MSR_IA32_ARCH_CAPABILITIES
                ? ARCH_CAPABILITIES_KNOWN : CORE_CAPABILITIES_KNOWN;
            return true;
        }
        case MSR_PLATFORM_INFO: {
            if (!virt_vmx) {
                return false;
            }
            *fault = !msr_allowed(msr) || !rdmsr_safe(msr, value);
            *value &= PLATFORM_INFO_KNOWN
                | (msr_allowed(MSR_FEATURE_ENABLES) ? PLATFORM_INFO_CPUID_FAULT
                    : 0);
            return true;
        }
        case MSR_IA32_MISC_ENABLE: {
            if (!misc_enable_copied) {
                return false;
            }
            *fault = !rdmsr_safe(msr, value);
            *value = (*value & ~MISC_ENABLE_COPIED)
                | (v->misc_enable & misc_enable_supported)
                | (pebs_hidden ? MISC_ENABLE_PEBS_UNAVAILABLE : 0);
            return true;
        }
        case MSR_IA32_MCG_CTL: {
            if (!mcg_ctl) {
                return false;
            }
            *value = v->mcg_ctl;
            return true;
        }
        case MSR_IA32_PERF_CAPABILITIES: {
            if (!perf_capabilities) {
                return false;
            }
            *fault = !rdmsr_safe(msr, value);
            *value &= PERF_CAPABILITIES_KNOWN;
            if (!msr_allowed(MSR_PERF_METRICS)) {
                *value &= ~PERF_CAPABILITIES_PERF_METRICS;
            }
            if (pt_hidden) {
                *value &= ~PERF_CAPABILITIES_PEBS_OUTPUT_PT;
            }
            if (pebs_hidden) {
                *value &= ~PERF_CAPABILITIES_PEBS;
            }
            return true;
        }
        case MSR_AMD_HWCR: {
            if (virt_vmx) {
                return false;
            }
            *fault = !rdmsr_safe(msr, value);
            *value |= HWCR_SMM_LOCK;
            return true;
        }
        case MSR_IA32_PERF_GLOBAL_CTRL: {
            if (!perf_global_kept) {
                return false;
            }
            *fault = false;
            *value = vmx_read(VMCS_GUEST_PERF_GLOBAL_CTRL);
            return true;
        }
        case MSR_AMD_NB_CFG: {
            if (virt_vmx) {
                return false;
            }
            *fault = !rdmsr_safe(msr, value);
            return true;
        }
        case MSR_IA32_HW_FEEDBACK_PTR: {
            if (!hfi) {
                return false;
            }
            *fault = !rdmsr_safe(msr, value);
            spinlock_acquire(&hfi_lock);
            struct hfi_package *p = hfi_package(false);
            if (p != NULL) {
                *value = p->guest;
            }
            spinlock_release(&hfi_lock);
            return true;
        }
        default: {
            return false;
        }
    }
}

bool msr_wrmsr(struct vcpu *v, uint32_t msr, uint64_t value, bool *fault) {
    if ((pt_hidden && pt_msr(msr)) || (mpx_hidden && msr == MSR_IA32_BNDCFGS)
     || lbr_msr(msr) || (pebs_hidden && msr == MSR_IA32_PEBS_ENABLE)
     || (pstates_read_only && msr >= MSR_AMD_PSTATE_DEF0
      && msr <= MSR_AMD_PSTATE_DEF7)) {
        *fault = true;
        return true;
    }
    if (!virt_vmx && amd_memory_wrmsr(v, msr, value, fault)) {
        return true;
    }
    if (!virt_vmx && msr >= MSR_AMD_CPUID_7_FEATURES
     && msr <= MSR_AMD_CPUID_EXT_FEATURES) {
        size_t i = msr - MSR_AMD_CPUID_7_FEATURES;
        *fault = ((v->amd_cpuid_present >> i) & 1) == 0
            || (value & ~v->amd_cpuid_hardware[i]) != 0;
        if (!*fault) {
            v->amd_cpuid[i] = value;
        }
        return true;
    }
    int counter = virt_vmx ? -1 : amd_perf_counter(msr);
    if (counter >= 0) {
        *fault = !wrmsr_safe(
            msr, (value & ~PERF_EVT_SEL_HG_ONLY) | PERF_EVT_SEL_GUEST_ONLY
        );
        if (!*fault) {
            v->amd_perf_evt_sel[counter] = value;
        }
        return true;
    }
    switch (msr) {
        // As the processor does, IA32_TSC_ADJUST moves with the TSC (SDM
        // 325462-093 Vol. 3B, 20.17.3).
        case MSR_IA32_TSC: {
            uint64_t now = rdtsc();
            v->tsc_adjust += value - (now + v->tsc_offset);
            tsc_offset_set(v, value - now);
            return true;
        }
        case MSR_IA32_TSC_ADJUST: {
            if (!tsc_adjust) {
                return false;
            }
            tsc_offset_set(v, v->tsc_offset + value - v->tsc_adjust);
            v->tsc_adjust = value;
            return true;
        }
        case MSR_IA32_TSC_DEADLINE: {
            if (!tsc_deadline) {
                return false;
            }
            // 0 disarms the timer, which a deadline does not.
            uint64_t deadline = value - v->tsc_offset;
            if (value == 0) {
                deadline = 0;
            } else if (deadline == 0) {
                deadline = 1;
            }
            *fault = !wrmsr_safe(msr, deadline);
            return true;
        }
        case MSR_IA32_FEATURE_CONTROL: {
            if (!virt_vmx) {
                return false;
            }
            *fault = true;
            return true;
        }
        case MSR_IA32_MISC_ENABLE: {
            if (!misc_enable_copied) {
                return false;
            }
            // Bits the processor lacks are reserved.
            uint64_t kept = MISC_ENABLE_COPIED
                | (pebs_hidden ? MISC_ENABLE_PEBS_UNAVAILABLE : 0);
            *fault = (value & MISC_ENABLE_COPIED & ~misc_enable_supported) != 0
                || !wrmsr_safe(msr, value & ~kept);
            if (!*fault) {
                v->misc_enable = (value & MISC_ENABLE_COPIED)
                    | (v->misc_enable & ~misc_enable_supported);
            }
            return true;
        }
        case MSR_IA32_MCG_CTL: {
            if (!mcg_ctl) {
                return false;
            }
            *fault = !wrmsr_safe(msr, value != 0 ? ~(uint64_t)0 : 0);
            if (!*fault) {
                v->mcg_ctl = value;
            }
            return true;
        }
        case MSR_IA32_HW_FEEDBACK_PTR: {
            if (!hfi) {
                return false;
            }
            if ((value & hfi_reserved) != 0) {
                *fault = true;
                return true;
            }
            uint64_t host = hfi_host_pointer(value);
            spinlock_acquire(&hfi_lock);
            *fault = !wrmsr_safe(msr, host);
            if (!*fault) {
                hfi_package(true)->guest = value;
            }
            spinlock_release(&hfi_lock);
            return true;
        }
        case MSR_AMD_PATCH_LOADER: {
            if (virt_vmx) {
                return false;
            }
            *fault = false;
            return true;
        }
        case MSR_AMD_HWCR: {
            if (virt_vmx) {
                return false;
            }
            uint64_t hardware;
            uint64_t fixed = ~(uint64_t)(HWCR_GUEST | HWCR_SMM_LOCK);
            *fault = !rdmsr_safe(msr, &hardware)
                || ((value ^ hardware) & fixed) != 0
                || !wrmsr_safe(
                    msr,
                    (hardware & ~(uint64_t)HWCR_GUEST) | (value & HWCR_GUEST)
                );
            return true;
        }
        case MSR_AMD_NB_CFG: {
            if (virt_vmx) {
                return false;
            }
            uint64_t hardware;
            uint64_t fixed = ~NB_CFG_ENABLE_CF8_EXT_CFG;
            *fault = !rdmsr_safe(msr, &hardware)
                || ((value ^ hardware) & fixed) != 0
                || !wrmsr_safe(msr, value);
            return true;
        }
        case MSR_IA32_PERF_GLOBAL_CTRL: {
            if (!perf_global_kept) {
                return false;
            }
            *fault = !wrmsr_safe(msr, value);
            if (!*fault) {
                wrmsr(msr, 0);
                vmx_write(VMCS_GUEST_PERF_GLOBAL_CTRL, value);
            }
            return true;
        }
        case MSR_DEBUGCTL: {
            if (!lbr_hidden) {
                return false;
            }
            *fault = (value & DEBUGCTL_LBR) != 0 || !wrmsr_safe(msr, value);
            return true;
        }
        case MSR_AMD_NODE_ID: {
            if (virt_vmx) {
                return false;
            }
            *fault = true;
            return true;
        }
        case MSR_AMD_L3_QOS_EXT_CFG: {
            if (virt_vmx) {
                return false;
            }
            *fault = (value & L3_QOS_EXT_CFG_SDCIAE) != 0
                || !wrmsr_safe(msr, value);
            return true;
        }
        case MSR_DC_CFG: {
            // AMD's erratum 383 workaround (41322 rev. 3.92) stays on (svm.c).
            if (virt_vmx || !svm_erratum_383) {
                return false;
            }
            *fault = !wrmsr_safe(msr, value | DC_CFG_ERRATUM_383);
            return true;
        }
        case MSR_IA32_XSS: {
            if (!xss) {
                return false;
            }
            *fault = (value & ~cpuid_xss_allowed()) != 0
                || !wrmsr_safe(msr, value);
            return true;
        }
        case MSR_IA32_HW_FEEDBACK_CONFIG: {
            if (!hfi) {
                return false;
            }
            // Not without a table the processor can write to, which the
            // guest's pointer may not have given it.
            uint64_t host;
            *fault = ((value & HW_FEEDBACK_CONFIG_ENABLE) != 0
                && (!rdmsr_safe(MSR_IA32_HW_FEEDBACK_PTR, &host)
                    || (host & HW_FEEDBACK_PTR_VALID) == 0))
                || !wrmsr_safe(msr, value);
            return true;
        }
        default: {
            return false;
        }
    }
}
