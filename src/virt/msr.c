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
#include <sys/fixup.h>
#include <sys/x86.h>
#include <virt/msr.h>
#include <virt/vcpu.h>
#include <virt/virt.h>

// SDM 325462-093 Vol. 1, Table 21-23.
#define CPUID_7_1_EBX_CPUIDMAXVAL_LIM_RMV (1u << 3)

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

void msr_init(void) {
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

void msr_vcpu_init(struct vcpu *v) {
    v->misc_enable = misc_enable_firmware;
}

bool msr_rdmsr(struct vcpu *v, uint32_t msr, uint64_t *value, bool *fault) {
    switch (msr) {
        case MSR_IA32_MISC_ENABLE: {
            if (!misc_enable_copied) {
                return false;
            }
            *fault = !rdmsr_safe(msr, value);
            *value = (*value & ~MISC_ENABLE_COPIED) | v->misc_enable;
            return true;
        }
        default: {
            return false;
        }
    }
}

bool msr_wrmsr(struct vcpu *v, uint32_t msr, uint64_t value, bool *fault) {
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
        default: {
            return false;
        }
    }
}
