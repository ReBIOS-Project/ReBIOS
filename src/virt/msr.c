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
#include <virt/layout.h>
#include <virt/msr.h>
#include <virt/vcpu.h>
#include <virt/virt.h>
#include <virt/vmx.h>

// SDM 325462-093 Vol. 1, Tables 21-16, 21-19, and 21-23.
#define CPUID_6_EAX_HW_FEEDBACK (1u << 19)
#define CPUID_7_EBX_PT (1u << 25)
#define CPUID_7_1_EBX_CPUIDMAXVAL_LIM_RMV (1u << 3)

// SDM 325462-093 Vol. 3B, 17.6.4 and 17.6.5.
#define MSR_IA32_HW_FEEDBACK_PTR 0x17d0
#define MSR_IA32_HW_FEEDBACK_CONFIG 0x17d1
#define HW_FEEDBACK_PTR_VALID 1u
#define HW_FEEDBACK_CONFIG_ENABLE 1u

// Intel PT's MSRs (SDM 325462-093 Vol. 4, Table 2-2), those from the first
// trigger's to IA32_RTIT_CR3_MATCH taking in IA32_RTIT_CTL and
// IA32_RTIT_STATUS, and IA32_XSS's bit for its state.
#define MSR_IA32_RTIT_OUTPUT_BASE 0x560
#define MSR_IA32_RTIT_OUTPUT_MASK_PTRS 0x561
#define MSR_IA32_RTIT_TRIGGER0_CFG 0x568
#define MSR_IA32_RTIT_CR3_MATCH 0x572
#define MSR_IA32_RTIT_ADDR0_A 0x580
#define MSR_IA32_RTIT_ADDR3_B 0x587
#define MSR_IA32_XSS 0xda0
#define XSS_PT (1u << 8)

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
    // XRSTORS would load IA32_RTIT_CTL from its PT state.
    virt_intercept_msr(MSR_IA32_XSS, false, true);
}

void msr_init(void) {
    // First, as the CPUID limit hides the leaves the rest reads.
    misc_enable_init();
    hfi_init();
    pt_init();
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
    if (pt_hidden && pt_msr(msr)) {
        *fault = true;
        return true;
    }
    switch (msr) {
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
    if (pt_hidden && pt_msr(msr)) {
        *fault = true;
        return true;
    }
    switch (msr) {
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
        case MSR_IA32_XSS: {
            if (!pt_hidden) {
                return false;
            }
            *fault = (value & XSS_PT) || !wrmsr_safe(msr, value);
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
