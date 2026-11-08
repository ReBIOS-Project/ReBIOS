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
#include <sys/errata.h>
#include <lib/util.h>
#include <sys/fixup.h>
#include <sys/x86.h>

#define CPUID_1_ECX_AVX ((uint32_t)1 << 28)
#define CPUID_1_ECX_HYPERVISOR ((uint32_t)1 << 31)
#define CPUID_1_EDX_SSE2 ((uint32_t)1 << 26)
#define CPUID_80000021_EAX_LFENCE_SERIALISING ((uint32_t)1 << 2)

#define MSR_AMD_PATCH_LEVEL 0x8b
#define MSR_AMD_MC4_MASK 0xc0010048
#define MSR_AMD_LS_CFG 0xc0011020
#define MSR_AMD_IC_CFG 0xc0011021
#define MSR_AMD_FP_CFG 0xc0011028
#define MSR_AMD_DE_CFG 0xc0011029
#define MSR_AMD_BU_CFG2 0xc001102a
#define MSR_AMD_BP_CFG 0xc001102e
#define MSR_AMD_SPECTRAL_CHICKEN 0xc00110e3

#define MC4_MASK_GART_TLB_WALK ((uint64_t)1 << 10)
#define LS_CFG_ERRATUM_793 ((uint64_t)1 << 15)
#define IC_CFG_WAY_ACCESS_FILTER ((uint64_t)0x1e)
#define FP_CFG_DENORM_FIX ((uint64_t)1 << 9)
#define DE_CFG_LFENCE_SERIALISE ((uint64_t)1 << 1)
#define DE_CFG_FP_BACKUP_FIX ((uint64_t)1 << 9)
#define BU_CFG2_WC_PLUS_OFF ((uint64_t)1 << 24)
#define BP_CFG_SHARED_BTB_FIX ((uint64_t)1 << 5)
#define BP_CFG_ZEN2_FIX ((uint64_t)1 << 33)
#define SPECTRAL_CHICKEN ((uint64_t)1 << 1)

// The Zen 2 models and steppings whose microcode from a revision on fixes
// Zenbleed, as Linux 7.3 lists them (amd_zenbleed_microcode[]).
static const struct {
    uint32_t model;
    uint32_t stepping;
    uint32_t revision;
} zenbleed_fixed[] = {
    { 0x31, 0x0, 0x0830107b },
    { 0x60, 0x1, 0x0860010c },
    { 0x68, 0x1, 0x08608107 },
    { 0x71, 0x0, 0x08701033 },
    { 0xa0, 0x0, 0x08a00009 }
};

unsigned int errata_amd_zen(void) {
    if (x86_cpu_vendor() != X86_CPU_VENDOR_AMD) {
        return 0;
    }
    struct x86_cpu_signature s = x86_cpu_signature();
    uint32_t m = s.model;
    if (s.family == 0x17) {
        if (m <= 0x2f || (m >= 0x50 && m <= 0x5f)) {
            return 1;
        }
        if ((m >= 0x30 && m <= 0x4f) || (m >= 0x60 && m <= 0x7f) || (m >= 0x90 && m <= 0x91)
         || (m >= 0xa0 && m <= 0xaf)) {
            return 2;
        }
    } else if (s.family == 0x19) {
        if (m <= 0x0f || (m >= 0x20 && m <= 0x5f)) {
            return 3;
        }
        if ((m >= 0x10 && m <= 0x1f) || (m >= 0x60 && m <= 0xaf)) {
            return 4;
        }
    } else if (s.family == 0x1a) {
        if (m <= 0x2f || (m >= 0x40 && m <= 0x4f) || (m >= 0x60 && m <= 0x7f)
         || (m >= 0xd0 && m <= 0xd7)) {
            return 5;
        }
        if ((m >= 0x50 && m <= 0x5f) || (m >= 0x80 && m <= 0xaf) || (m >= 0xc0 && m <= 0xcf)
         || (m >= 0xd8 && m <= 0xef)) {
            return 6;
        }
    }
    return 0;
}

// Sets and clears bits of an MSR the processor may lack, as Linux 7.3's
// msr_set_bit() and msr_clear_bit() do.
static void msr_change(uint32_t msr, uint64_t set, uint64_t clear) {
    uint64_t value;
    if (rdmsr_safe(msr, &value) && ((value | set) & ~clear) != value) {
        wrmsr_safe(msr, (value | set) & ~clear);
    }
}

// Whether a Zen 2 processor's microcode fixes Zenbleed (Linux 7.3,
// zen2_zenbleed_check()).
static bool zenbleed_fixed_by_microcode(struct x86_cpu_signature s) {
    uint64_t revision;
    if (!rdmsr_safe(MSR_AMD_PATCH_LEVEL, &revision)) {
        return false;
    }
    for (size_t i = 0; i < ARRAY_COUNT(zenbleed_fixed); i++) {
        if (zenbleed_fixed[i].model == s.model && zenbleed_fixed[i].stepping == s.stepping) {
            return (uint32_t)revision >= zenbleed_fixed[i].revision;
        }
    }
    return false;
}

void errata_cpu_init(void) {
    if (x86_cpu_vendor() != X86_CPU_VENDOR_AMD) {
        return;
    }
    struct x86_cpu_signature s = x86_cpu_signature();
    struct cpuid_regs leaf1 = cpuid(1, 0);
    // A hypervisor's processor may lack the MSRs Linux 7.3 only sets on bare
    // metal.
    bool bare = (leaf1.ecx & CPUID_1_ECX_HYPERVISOR) == 0;

    // Family 16h erratum 793, a core hang (early_init_amd()).
    if (s.family == 0x16 && s.model <= 0xf) {
        msr_change(MSR_AMD_LS_CFG, LS_CFG_ERRATUM_793, 0);
    }
    // Family 10h's GART table walk errors masked, and WC+ kept from turning
    // into UC for guests under nested paging (init_amd_gh()).
    if (s.family == 0x10) {
        msr_change(MSR_AMD_MC4_MASK, MC4_MASK_GART_TLB_WALK, 0);
        msr_change(MSR_AMD_BU_CFG2, 0, BU_CFG2_WC_PLUS_OFF);
    }
    // Family 15h's way access filter, slow on some workloads, where the
    // firmware left all of it on (init_amd_bd()).
    uint64_t ic_cfg;
    if (s.family == 0x15 && s.model >= 0x02 && s.model < 0x20
     && rdmsr_safe(MSR_AMD_IC_CFG, &ic_cfg) && (ic_cfg & IC_CFG_WAY_ACCESS_FILTER) == 0) {
        wrmsr_safe(MSR_AMD_IC_CFG, ic_cfg | IC_CFG_WAY_ACCESS_FILTER);
    }

    switch (errata_amd_zen()) {
        case 1: {
            // Floating-point divider state sampling (init_amd_zen1()).
            msr_change(MSR_AMD_FP_CFG, FP_CFG_DENORM_FIX, 0);
            break;
        }
        case 2: {
            // Speculation from the middle of a basic block, Zenbleed with
            // microcode that leaves it, and the branch predictor's bug
            // (init_amd_zen2()).
            if (bare) {
                msr_change(MSR_AMD_SPECTRAL_CHICKEN, SPECTRAL_CHICKEN, 0);
                if ((leaf1.ecx & CPUID_1_ECX_AVX) != 0) {
                    if (zenbleed_fixed_by_microcode(s)) {
                        msr_change(MSR_AMD_DE_CFG, 0, DE_CFG_FP_BACKUP_FIX);
                    } else {
                        msr_change(MSR_AMD_DE_CFG, DE_CFG_FP_BACKUP_FIX, 0);
                    }
                }
                msr_change(MSR_AMD_BP_CFG, BP_CFG_ZEN2_FIX, 0);
            }
            break;
        }
        case 4: {
            // The shared branch target buffer (init_amd_zen4()).
            if (bare) {
                msr_change(MSR_AMD_BP_CFG, BP_CFG_SHARED_BTB_FIX, 0);
            }
            break;
        }
        default: {
            break;
        }
    }

    // LFENCE serialising where it may not be, as the hypervisor's fences need
    // (init_amd()).
    bool serialising = cpuid(0x80000000, 0).eax >= 0x80000021
                    && (cpuid(0x80000021, 0).eax & CPUID_80000021_EAX_LFENCE_SERIALISING) != 0;
    if (!serialising && (leaf1.edx & CPUID_1_EDX_SSE2) != 0) {
        msr_change(MSR_AMD_DE_CFG, DE_CFG_LFENCE_SERIALISE, 0);
    }
}
