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
// You should have received a copy of the GNU General Public License along with
// ReBIOS. If not, see <https://www.gnu.org/licenses/>.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <virt/pmu.h>
#include <sys/fixup.h>
#include <sys/x86.h>
#include <virt/nsvm.h>
#include <virt/vcpu.h>
#include <virt/virt.h>
#include <virt/vlapic.h>
#include <virt/vmx.h>

// Intel's architectural performance monitoring (SDM 325462-093 Vol. 3B, 22.2;
// Vol. 4, Table 2-2).
#define MSR_IA32_PMC0 0xc1
#define MSR_IA32_PERFEVTSEL0 0x186
#define MSR_IA32_FIXED_CTR0 0x309
#define MSR_IA32_PERF_CAPABILITIES 0x345
#define MSR_IA32_FIXED_CTR_CTRL 0x38d
#define MSR_IA32_PERF_GLOBAL_STATUS 0x38e
#define MSR_IA32_PERF_GLOBAL_STATUS_SET 0x391
#define MSR_IA32_A_PMC0 0x4c1
#define CPUID_1_ECX_PDCM ((uint32_t)1 << 15)
#define PERF_CAPABILITIES_FW_WRITE ((uint64_t)1 << 13)

// AMD's core performance counters (APM 24593 rev. 3.46, 13.2.1):
// the legacy counters, those of PerfCtrExtCore, and PerfMonV2's global
// registers.
#define MSR_AMD_PERF_GLOBAL_STATUS 0xc0000300
#define MSR_AMD_PERF_GLOBAL_CTL 0xc0000301
#define MSR_AMD_PERF_GLOBAL_STATUS_SET 0xc0000303
#define MSR_AMD_PERF_CTR0 0xc0010004
#define MSR_AMD_PERF_CTR_EXT0 0xc0010201
#define CPUID_80000001_ECX_PERF_CTR_EXT_CORE ((uint32_t)1 << 23)
#define CPUID_80000022_EAX_PERFMON_V2 ((uint32_t)1 << 0)
#define AMD_COUNTERS 6
#define AMD_WIDTH 48

// The event select fields that both vendors have, and AMD's own.
#define EVTSEL_EVENT ((uint64_t)0xffff)
#define EVTSEL_USR ((uint64_t)1 << 16)
#define EVTSEL_OS ((uint64_t)1 << 17)
#define EVTSEL_EDGE ((uint64_t)1 << 18)
#define EVTSEL_INT ((uint64_t)1 << 20)
#define EVTSEL_EN ((uint64_t)1 << 22)
#define EVTSEL_INV ((uint64_t)1 << 23)
#define EVTSEL_CMASK ((uint64_t)0xff << 24)
#define AMD_EVTSEL_EVENT_HIGH ((uint64_t)0xf << 32)
#define AMD_EVTSEL_GUEST_ONLY ((uint64_t)1 << 40)
#define AMD_EVTSEL_HOST_ONLY ((uint64_t)1 << 41)

// Retired instructions and retired branch instructions, as event select | unit
// mask << 8: Intel's architectural events (SDM 325462-093 Vol. 3B, Table
// 22-3), and AMD's PMCx0C0 and PMCx0C2 (PPR 57238 rev. 0.49).
#define INTEL_INSTRUCTIONS 0x00c0
#define INTEL_BRANCHES 0x00c4
#define AMD_INSTRUCTIONS 0x00c0
#define AMD_BRANCHES 0x00c2

// Fixed counter 0's field of IA32_FIXED_CTR_CTRL: it counts retired
// instructions.
#define FIXED_OS 1
#define FIXED_USR 2
#define FIXED_PMI 8

// Fixed counter 0's bit in a vCPU's masks of counters (vcpu.h), past the
// general ones'.
#define FIXED0 31

// The freezes on PMIs that the guest's debug control MSR asks for, and the
// global status bits that show them, the counters' stopping them (SDM
// 325462-093 Vol. 3B, 20.4.7; APM 24593 rev. 3.46, 13.1.1.6 and 13.2.1).
#define DEBUGCTL_FREEZE_LBRS_ON_PMI ((uint64_t)1 << 11)
#define DEBUGCTL_FREEZE_PERFMON_ON_PMI ((uint64_t)1 << 12)
#define GLOBAL_STATUS_LBR_FROZEN ((uint64_t)1 << 58)
#define GLOBAL_STATUS_FROZEN ((uint64_t)1 << 59)

// Intel's: the architectural performance monitoring version, the general and
// fixed counters and their widths, and whether IA32_A_PMCi takes their full
// width. AMD's: whether it has PerfCtrExtCore's counters, and PerfMonV2.
static unsigned int intel_version;
static size_t gp_count;
static unsigned int gp_width;
static size_t fixed_count;
static unsigned int fixed_width;
static bool full_width_writes;
static bool amd_extended;
static bool amd_global;

void pmu_init(void) {
    if (virt_vmx) {
        if (cpuid(0, 0).eax < 0xa) {
            return;
        }
        // The guest's event selects and fixed counter control go to the
        // processor, and tell pmu_programmed() what its counters count.
        struct cpuid_regs r = cpuid(0xa, 0);
        intel_version = r.eax & 0xff;
        gp_count = (r.eax >> 8) & 0xff;
        gp_width = (r.eax >> 16) & 0xff;
        if (intel_version >= 2) {
            fixed_count = r.edx & 0x1f;
            fixed_width = (r.edx >> 5) & 0xff;
        }
        uint64_t capabilities;
        full_width_writes = (cpuid(1, 0).ecx & CPUID_1_ECX_PDCM) != 0
                         && rdmsr_safe(MSR_IA32_PERF_CAPABILITIES, &capabilities)
                         && (capabilities & PERF_CAPABILITIES_FW_WRITE) != 0;
        if (gp_count > FIXED0) {
            gp_count = FIXED0;
        }
        for (size_t i = 0; i < gp_count; i++) {
            virt_intercept_msr(MSR_IA32_PERFEVTSEL0 + (uint32_t)i, false, true);
        }
        if (fixed_count != 0) {
            virt_intercept_msr(MSR_IA32_FIXED_CTR_CTRL, false, true);
        }
        return;
    }
    uint32_t extended = cpuid(0x80000000, 0).eax;
    amd_extended = (cpuid(0x80000001, 0).ecx & CPUID_80000001_ECX_PERF_CTR_EXT_CORE) != 0;
    amd_global = extended >= 0x80000022
              && (cpuid(0x80000022, 0).eax & CPUID_80000022_EAX_PERFMON_V2) != 0;
    gp_count = amd_extended ? AMD_COUNTERS : 4;
    gp_width = AMD_WIDTH;
}

bool pmu_programming(uint32_t msr) {
    if (!virt_vmx) {
        return false;
    }
    if (msr >= MSR_IA32_PERFEVTSEL0 && msr < MSR_IA32_PERFEVTSEL0 + gp_count) {
        return true;
    }
    return fixed_count != 0 && msr == MSR_IA32_FIXED_CTR_CTRL;
}

// The architectural event an event select counts, as event select | unit mask
// << 8, with AMD's high event select bits above.
static uint64_t event_of(uint64_t evtsel) {
    if (virt_vmx) {
        return (evtsel >> 32) != 0 ? UINT64_MAX : evtsel & EVTSEL_EVENT;
    }
    return (evtsel & EVTSEL_EVENT) | (evtsel & AMD_EVTSEL_EVENT_HIGH) >> 16;
}

void pmu_programmed(struct vcpu *v) {
    v->pmu_instructions = 0;
    v->pmu_branches = 0;
    for (size_t i = 0; i < gp_count; i++) {
        uint64_t evtsel = v->amd_perf_evt_sel[i];
        if (virt_vmx) {
            evtsel = rdmsr(MSR_IA32_PERFEVTSEL0 + (uint32_t)i);
        }
        if ((evtsel & EVTSEL_EN) == 0) {
            continue;
        }
        uint64_t event = event_of(evtsel);
        if (event == (virt_vmx ? INTEL_INSTRUCTIONS : AMD_INSTRUCTIONS)) {
            v->pmu_instructions |= (uint32_t)1 << i;
        } else if (event == (virt_vmx ? INTEL_BRANCHES : AMD_BRANCHES)) {
            v->pmu_branches |= (uint32_t)1 << i;
        }
    }
    if (fixed_count != 0 && (rdmsr(MSR_IA32_FIXED_CTR_CTRL) & (FIXED_OS | FIXED_USR)) != 0) {
        v->pmu_instructions |= (uint32_t)1 << FIXED0;
    }
}

bool pmu_amd_counts_now(struct vcpu *v, uint64_t evtsel) {
    uint64_t host_guest = evtsel & (AMD_EVTSEL_HOST_ONLY | AMD_EVTSEL_GUEST_ONLY);
    if (host_guest == 0) {
        return true;
    }
    if (!nsvm_svme(v)) {
        return false;
    }
    return host_guest == (AMD_EVTSEL_HOST_ONLY | AMD_EVTSEL_GUEST_ONLY)
        || (host_guest == AMD_EVTSEL_GUEST_ONLY) == nsvm_in_l2(v);
}

static uint64_t width_mask(unsigned int width) {
    return width >= 64 ? UINT64_MAX : ((uint64_t)1 << width) - 1;
}

// Whether an event select counts its event at the CPL, as both vendors have OS
// and USR (SDM 325462-093 Vol. 3B, 22.2.1; APM 24593 rev. 3.46, Table 13-6),
// and counts it once for each, with no edge, inversion, or counter mask.
static bool counts_at(uint64_t evtsel, unsigned int cpl) {
    if ((evtsel & (EVTSEL_EDGE | EVTSEL_INV | EVTSEL_CMASK)) != 0) {
        return false;
    }
    return (evtsel & (cpl == 0 ? EVTSEL_OS : EVTSEL_USR)) != 0;
}

// What adding n leaves in a counter of width bits that holds value, and
// whether it overflows: where the processor's global status cannot show the
// overflow, before Intel's architectural performance monitoring version 4, the
// largest value, whose next event's overflow the processor then signals
// itself, rather than the sum wrapped.
static uint64_t sum(uint64_t value, uint64_t n, unsigned int width, bool shown, bool *overflow) {
    uint64_t mask = width_mask(width);
    uint64_t total = (value & mask) + n;
    *overflow = total > mask;
    if (*overflow && !shown) {
        *overflow = false;
        return mask;
    }
    return total & mask;
}

// Intel's counters of the vCPU's masks: the overflows they had, as
// IA32_PERF_GLOBAL_STATUS shows them, and whether one asks for a PMI.
static uint64_t intel_count(
    struct vcpu *v,
    uint64_t instructions,
    uint64_t branches,
    bool *interrupt
) {
    uint64_t global = UINT64_MAX;
    if (intel_version >= 2) {
        global = vmx_read(VMCS_GUEST_PERF_GLOBAL_CTRL);
        if (global == 0) {
            return 0;
        }
    }
    if (intel_version >= 4 && (rdmsr(MSR_IA32_PERF_GLOBAL_STATUS) & GLOBAL_STATUS_FROZEN) != 0) {
        return 0;
    }
    unsigned int cpl = vcpu_cpl(v);
    bool shown = intel_version >= 4;
    uint64_t status = 0;
    uint32_t fixed0 = (uint32_t)1 << FIXED0;
    if (instructions != 0 && (v->pmu_instructions & fixed0) != 0
     && (global & ((uint64_t)1 << 32)) != 0) {
        uint64_t control = rdmsr(MSR_IA32_FIXED_CTR_CTRL) & 0xf;
        if ((control & (cpl == 0 ? FIXED_OS : FIXED_USR)) != 0) {
            bool overflow;
            uint64_t value = rdmsr(MSR_IA32_FIXED_CTR0);
            wrmsr(MSR_IA32_FIXED_CTR0, sum(value, instructions, fixed_width, shown, &overflow));
            if (overflow) {
                status |= (uint64_t)1 << 32;
                *interrupt |= (control & FIXED_PMI) != 0;
            }
        }
    }
    for (size_t i = 0; i < gp_count; i++) {
        uint32_t bit = (uint32_t)1 << i;
        uint64_t n = (v->pmu_instructions & bit) != 0 ? instructions : 0;
        if ((v->pmu_branches & bit) != 0) {
            n = branches;
        }
        if (n == 0 || (global & ((uint64_t)1 << i)) == 0) {
            continue;
        }
        uint64_t evtsel = rdmsr(MSR_IA32_PERFEVTSEL0 + (uint32_t)i);
        if (!counts_at(evtsel, cpl)) {
            continue;
        }
        bool overflow;
        uint64_t value = sum(rdmsr(MSR_IA32_PMC0 + (uint32_t)i), n, gp_width, shown, &overflow);
        // IA32_PMCi takes bits 31:0 sign-extended, which hold the value where
        // its upper bits all equal bit 31 (SDM 325462-093 Vol. 3B, 22.2.8).
        uint64_t upper = value >> 31;
        if (full_width_writes) {
            wrmsr(MSR_IA32_A_PMC0 + (uint32_t)i, value);
        } else if (upper == 0 || upper == width_mask(gp_width) >> 31) {
            wrmsr(MSR_IA32_PMC0 + (uint32_t)i, value);
        } else {
            continue;
        }
        if (overflow) {
            status |= (uint64_t)1 << i;
            *interrupt |= (evtsel & EVTSEL_INT) != 0;
        }
    }
    return status;
}

// AMD's counters of the vCPU's masks, from the event selects the guest wrote
// (msr.c). Where the guest's EFER.SVME is set, its host/guest-only field
// counts in the mode the guest is in: its guest's while its guest runs, else
// its own (APM 24593 rev. 3.46, Table 13-4).
static uint64_t amd_count(
    struct vcpu *v,
    uint64_t instructions,
    uint64_t branches,
    bool *interrupt
) {
    uint64_t global = amd_global ? rdmsr(MSR_AMD_PERF_GLOBAL_CTL) : UINT64_MAX;
    if (global == 0
     || (amd_global && (rdmsr(MSR_AMD_PERF_GLOBAL_STATUS) & GLOBAL_STATUS_FROZEN) != 0)) {
        return 0;
    }
    unsigned int cpl = vcpu_cpl(v);
    uint64_t status = 0;
    for (size_t i = 0; i < gp_count; i++) {
        uint32_t bit = (uint32_t)1 << i;
        uint64_t n = (v->pmu_instructions & bit) != 0 ? instructions : 0;
        if ((v->pmu_branches & bit) != 0) {
            n = branches;
        }
        uint64_t evtsel = v->amd_perf_evt_sel[i];
        if (n == 0 || (global & ((uint64_t)1 << i)) == 0 || !counts_at(evtsel, cpl)
         || !pmu_amd_counts_now(v, evtsel)) {
            continue;
        }
        uint32_t counter = MSR_AMD_PERF_CTR0 + (uint32_t)i;
        if (amd_extended) {
            counter = MSR_AMD_PERF_CTR_EXT0 + 2 * (uint32_t)i;
        }
        bool overflow;
        wrmsr(counter, sum(rdmsr(counter), n, gp_width, true, &overflow));
        if (overflow) {
            status |= (uint64_t)1 << i;
            *interrupt |= (evtsel & EVTSEL_INT) != 0;
        }
    }
    return status;
}

void pmu_retired(struct vcpu *v, uint64_t instructions, uint64_t branches) {
    bool counted = (instructions != 0 && v->pmu_instructions != 0)
                || (branches != 0 && v->pmu_branches != 0);
    if (!counted || (virt_vmx && !vmx_perf_global_switched())) {
        return;
    }
    bool interrupt = false;
    uint64_t status;
    uint32_t status_set;
    if (virt_vmx) {
        status = intel_count(v, instructions, branches, &interrupt);
        status_set = MSR_IA32_PERF_GLOBAL_STATUS_SET;
    } else {
        status = amd_count(v, instructions, branches, &interrupt);
        status_set = amd_global ? MSR_AMD_PERF_GLOBAL_STATUS_SET : 0;
    }
    if (status == 0) {
        return;
    }
    // The freezes a PMI brings, which the status shows: before Intel's version
    // 4, sum() never overflows.
    if (interrupt) {
        uint64_t debugctl = vcpu_debugctl(v);
        if ((debugctl & DEBUGCTL_FREEZE_PERFMON_ON_PMI) != 0) {
            status |= GLOBAL_STATUS_FROZEN;
        }
        if ((debugctl & DEBUGCTL_FREEZE_LBRS_ON_PMI) != 0) {
            status |= GLOBAL_STATUS_LBR_FROZEN;
        }
    }
    if (status_set != 0) {
        wrmsr(status_set, status);
    }
    if (interrupt) {
        vlapic_perf_fired(v);
    }
}
