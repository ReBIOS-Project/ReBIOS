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

#include <stdint.h>
#include <stdbool.h>
#include <lib/heap.h>
#include <lib/spinlock.h>
#include <sys/cpu.h>
#include <sys/fixup.h>
#include <sys/mm.h>
#include <sys/smp.h>
#include <sys/x86.h>
#include <virt/cpuid.h>
#include <virt/layout.h>
#include <virt/msr.h>
#include <virt/msr_table.h>
#include <virt/nvmx.h>
#include <virt/vcpu.h>
#include <virt/virt.h>
#include <virt/vmx.h>

// SDM 325462-093 Vol. 1, Tables 21-16, 21-19, 21-23, and 21-35.
#define CPUID_6_EAX_HW_FEEDBACK (1u << 19)
#define CPUID_7_EBX_MPX (1u << 14)
#define CPUID_7_EBX_PT (1u << 25)
#define CPUID_7_1_EBX_CPUIDMAXVAL_LIM_RMV (1u << 3)
#define CPUID_D_1_EAX_XSAVES (1u << 3)

// SDM 325462-093 Vol. 3B, 17.6.4 and 17.6.5.
#define MSR_IA32_HW_FEEDBACK_PTR 0x17d0
#define MSR_IA32_HW_FEEDBACK_CONFIG 0x17d1
#define HW_FEEDBACK_PTR_VALID 1u
#define HW_FEEDBACK_CONFIG_ENABLE 1u

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

// IA32_FEATURE_CONTROL as the guest sees it (SDM 325462-093 Vol. 4, Table
// 2-2): locked, as vmx_cpu_init() leaves it, VMX outside SMX where the guest
// gets VMX, and the firmware's local machine check enable; not SMX's or SGX's.
#define FEATURE_CONTROL_LMCE_ON (1u << 20)

// IA32_ARCH_CAPABILITIES' bits the guest sees (SDM 325462-093 Vol. 4, Table
// 2-2): the defined ones, but for the microcode update interface's 9, 16, and
// 22, and 21, XAPIC_DISABLE_STATUS, as the guest's xAPIC mode is never off for
// good. IA32_CORE_CAPABILITIES has only model-specific ones (SDM 325462-093
// Vol. 4, Tables 2-46 and 2-56): 3, RSM_IN_CPL0_ONLY, and 4 and 5, the UC-lock
// and split-lock disables' support; 2 enumerates MSR_INTEGRITY_CAPABILITIES,
// which the guest lacks.
#define MSR_IA32_CORE_CAPABILITIES 0xcf
#define MSR_IA32_ARCH_CAPABILITIES 0x10a
#define ARCH_CAPABILITIES_KNOWN 0x17f9efdffull
#define CORE_CAPABILITIES_KNOWN 0x38ull

// MSR_PLATFORM_INFO's bits the model tables of SDM 325462-093 Vol. 4 define:
// the ratios, what is programmable, PPIN, the 16K DCU mode, LPM, and the
// ConfigTDP levels, but not BIOS Guard's, PEG2DMIDIS', or SMM's. Bit 31, which
// those tables reserve, is where processors report CPUID faulting (Linux 7.3's
// MSR_PLATFORM_INFO_CPUID_FAULT), kept where MSR_FEATURE_ENABLES is the
// guest's.
#define MSR_PLATFORM_INFO 0xce
#define MSR_FEATURE_ENABLES 0x140
#define PLATFORM_INFO_KNOWN 0x00ffff077480ff00ull
#define PLATFORM_INFO_CPUID_FAULT (1ull << 31)

// AMD's microcode patch loader, which loads the patch at the linear address
// written (Linux 7.3's MSR_AMD64_PATCH_LOADER): the guest's writes load
// nothing, as VMX has Intel's do (SDM 325462-093 Vol. 3C, 28.3), so that the
// processor does not change under the hypervisor.
#define MSR_AMD_PATCH_LOADER 0xc0010020

// IA32_MISC_ENABLE's bits the processor's keep clear, as the CPUID limit would
// hide leaves the hypervisor reads and XD disable would make EFER.NXE, which
// the host's and the guest's EFER may have set, reserved; either may also be
// shared with other logical processors (SDM 325462-093 Vol. 4, Tables 2-2 and
// 2-3). The AP start-up code clears them on the other processors.
#define MISC_ENABLE_COPIED (MISC_ENABLE_LIMIT_CPUID | MISC_ENABLE_XD_DISABLE)

static bool misc_enable_copied;
// The firmware's settings of those bits, which the guest starts with, and those
// the processor has.
static uint64_t misc_enable_firmware;
static uint64_t misc_enable_supported;

// The hardware feedback interface's table, which the processor writes to at
// the physical address a package's IA32_HW_FEEDBACK_PTR holds: the host
// address of the guest's table goes there, and what the guest wrote is kept
// for each package.
struct hfi_package {
    uint32_t id;
    uint64_t guest;
};
static bool hfi;
static unsigned hfi_pages;
static uint64_t hfi_reserved;
static unsigned package_shift;
static struct spinlock hfi_lock;
static struct hfi_package *hfi_packages;
static uint32_t hfi_package_count;

// Intel PT where it cannot be the guest's: its output would go to host
// addresses, and its tracing would go on through the hypervisor's own code.
static bool pt_hidden;
// MPX where its supervisor configuration would stay on in the hypervisor
// (vmx.c).
static bool mpx_hidden;
// IA32_XSS, which exists where XSAVES does.
static bool xss;
// The tables of the processor's model-specific MSRs, where it has any.
static const struct msr_model *model;

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
    if (max > 2 && (max < 7 || !(cpuid(7, 1).ebx & CPUID_7_1_EBX_CPUIDMAXVAL_LIM_RMV))) {
        misc_enable_supported |= MISC_ENABLE_LIMIT_CPUID;
    }
    misc_enable_supported |= misc_enable_firmware & MISC_ENABLE_XD_DISABLE;
    misc_enable_copied = true;
    virt_intercept_msr(MSR_IA32_MISC_ENABLE, true, true);
}

static void hfi_init(void) {
    if (cpuid(0, 0).eax < 6 || !(cpuid(6, 0).eax & CPUID_6_EAX_HW_FEEDBACK)) {
        return;
    }
    hfi = true;
    hfi_pages = ((cpuid(6, 0).edx >> 8) & 0xf) + 1;
    hfi_reserved = 0xffe | ~((1ull << cpu_physical_address_bits()) - 1);
    package_shift = cpu_package_shift();
    virt_intercept_msr(MSR_IA32_HW_FEEDBACK_PTR, true, true);
    virt_intercept_msr(MSR_IA32_HW_FEEDBACK_CONFIG, false, true);
}

static bool pt_msr(uint32_t msr) {
    return msr == MSR_IA32_RTIT_OUTPUT_BASE || msr == MSR_IA32_RTIT_OUTPUT_MASK_PTRS
           || (msr >= MSR_IA32_RTIT_TRIGGER0_CFG && msr <= MSR_IA32_RTIT_CR3_MATCH)
           || (msr >= MSR_IA32_RTIT_ADDR0_A && msr <= MSR_IA32_RTIT_ADDR3_B);
}

static void pt_init(void) {
    if (!virt_vmx || vmx_pt_passthrough() || cpuid(0, 0).eax < 7
        || !(cpuid(7, 0).ebx & CPUID_7_EBX_PT)) {
        return;
    }
    pt_hidden = true;
    for (uint32_t msr = MSR_IA32_RTIT_OUTPUT_BASE; msr <= MSR_IA32_RTIT_ADDR3_B; msr++) {
        if (pt_msr(msr)) {
            virt_intercept_msr(msr, true, true);
        }
    }
}

static void mpx_init(void) {
    if (!virt_vmx || vmx_mpx_passthrough() || cpuid(0, 0).eax < 7
        || !(cpuid(7, 0).ebx & CPUID_7_EBX_MPX)) {
        return;
    }
    mpx_hidden = true;
    virt_intercept_msr(MSR_IA32_BNDCFGS, true, true);
}

// The supervisor state components XSAVES and XRSTORS handle: only those the
// guest gets, as XRSTORS would load PT's IA32_RTIT_CTL, for one.
static void xss_init(void) {
    if (cpuid(0, 0).eax >= 0xd && (cpuid(0xd, 1).eax & CPUID_D_1_EAX_XSAVES)) {
        xss = true;
        virt_intercept_msr(MSR_IA32_XSS, false, true);
    }
}

void msr_init(void) {
    // First, as the CPUID limit hides the leaves the rest reads.
    misc_enable_init();
    hfi_init();
    pt_init();
    mpx_init();
    xss_init();
    if (!virt_vmx) {
        virt_intercept_msr(MSR_AMD_PATCH_LOADER, false, true);
    }
    if (virt_vmx) {
        virt_intercept_msr(MSR_IA32_FEATURE_CONTROL, true, true);
        virt_intercept_msr(MSR_IA32_CORE_CAPABILITIES, true, false);
        virt_intercept_msr(MSR_IA32_ARCH_CAPABILITIES, true, false);
        virt_intercept_msr(MSR_PLATFORM_INFO, true, false);
    }
}

static bool in_ranges(const struct msr_range *ranges, size_t count, uint32_t msr) {
    for (size_t i = 0; i < count; i++) {
        if (msr >= ranges[i].first && msr <= ranges[i].last) {
            return true;
        }
    }
    return false;
}

bool msr_allowed(uint32_t msr) {
    // AMD's processors keep every MSR no module handles.
    if (!virt_vmx) {
        return true;
    }
    if (in_ranges(msr_intel_architectural, msr_intel_architectural_count, msr)) {
        return true;
    }
    for (size_t i = 0; model != NULL && i < model->count; i++) {
        if (in_ranges(model->tables[i]->ranges, model->tables[i]->count, msr)) {
            return true;
        }
    }
    return false;
}

static void pass_ranges(const struct msr_range *ranges, size_t count) {
    for (size_t i = 0; i < count; i++) {
        for (uint64_t msr = ranges[i].first; msr <= ranges[i].last; msr++) {
            virt_pass_msr((uint32_t)msr);
        }
    }
}

void msr_allowed_init(void) {
    if (!virt_vmx) {
        return;
    }
    struct cpu_signature s = cpu_signature();
    for (size_t i = 0; i < msr_intel_model_count; i++) {
        if (msr_intel_models[i].family == s.family && msr_intel_models[i].model == s.model) {
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
    for (uint32_t i = 0; i < hfi_package_count; i++) {
        if (hfi_packages[i].id == id) {
            return &hfi_packages[i];
        }
    }
    if (!create) {
        return NULL;
    }
    if (hfi_packages == NULL) {
        hfi_packages = kmalloc(smp_cpu_count() * sizeof(*hfi_packages));
    }
    hfi_packages[hfi_package_count] = (struct hfi_package){id, 0};
    return &hfi_packages[hfi_package_count++];
}

// The pointer the processor gets: the host address of the guest's table, or
// none where the guest's RAM does not hold it.
static uint64_t hfi_host_pointer(uint64_t value) {
    uint64_t gpa = value & ~(uint64_t)HW_FEEDBACK_PTR_VALID;
    uint64_t hpa;
    if (!(value & HW_FEEDBACK_PTR_VALID) || !layout_ram(gpa, hfi_pages * PAGE_SIZE)
        || !layout_contiguous(gpa, hfi_pages, &hpa)) {
        return 0;
    }
    return hpa | HW_FEEDBACK_PTR_VALID;
}

void msr_vcpu_init(struct vcpu *v) {
    v->misc_enable = misc_enable_firmware;
}

bool msr_rdmsr(struct vcpu *v, uint32_t msr, uint64_t *value, bool *fault) {
    if ((pt_hidden && pt_msr(msr)) || (mpx_hidden && msr == MSR_IA32_BNDCFGS)) {
        *fault = true;
        return true;
    }
    switch (msr) {
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
            *value &= msr == MSR_IA32_ARCH_CAPABILITIES ? ARCH_CAPABILITIES_KNOWN
                                                        : CORE_CAPABILITIES_KNOWN;
            return true;
        }
        case MSR_PLATFORM_INFO: {
            if (!virt_vmx) {
                return false;
            }
            *fault = !msr_allowed(msr) || !rdmsr_safe(msr, value);
            *value &= PLATFORM_INFO_KNOWN
                      | (msr_allowed(MSR_FEATURE_ENABLES) ? PLATFORM_INFO_CPUID_FAULT : 0);
            return true;
        }
        case MSR_IA32_MISC_ENABLE: {
            if (!misc_enable_copied) {
                return false;
            }
            *fault = !rdmsr_safe(msr, value);
            *value = (*value & ~MISC_ENABLE_COPIED) | v->misc_enable;
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
    if ((pt_hidden && pt_msr(msr)) || (mpx_hidden && msr == MSR_IA32_BNDCFGS)) {
        *fault = true;
        return true;
    }
    switch (msr) {
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
            *fault = (value & MISC_ENABLE_COPIED & ~misc_enable_supported) != 0
                     || !wrmsr_safe(msr, value & ~MISC_ENABLE_COPIED);
            if (!*fault) {
                v->misc_enable = value & MISC_ENABLE_COPIED;
            }
            return true;
        }
        case MSR_IA32_HW_FEEDBACK_PTR: {
            if (!hfi) {
                return false;
            }
            if (value & hfi_reserved) {
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
        case MSR_IA32_XSS: {
            if (!xss) {
                return false;
            }
            *fault = (value & ~cpuid_xss_allowed()) || !wrmsr_safe(msr, value);
            return true;
        }
        case MSR_IA32_HW_FEEDBACK_CONFIG: {
            if (!hfi) {
                return false;
            }
            // Not without a table the processor can write to, which the
            // guest's pointer may not have given it.
            uint64_t host;
            *fault = ((value & HW_FEEDBACK_CONFIG_ENABLE)
                      && (!rdmsr_safe(MSR_IA32_HW_FEEDBACK_PTR, &host)
                          || !(host & HW_FEEDBACK_PTR_VALID)))
                     || !wrmsr_safe(msr, value);
            return true;
        }
        default: {
            return false;
        }
    }
}
