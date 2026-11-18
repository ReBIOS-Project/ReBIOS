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

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/cache.h>
#include <lib/log.h>
#include <lib/util.h>
#include <memory.h>
#include <sys/cpu.h>
#include <sys/fixup.h>
#include <sys/x86.h>

#define MSR_MTRRCAP 0xfe
#define MSR_MTRR_PHYSBASE0 0x200
#define MSR_MTRR_DEF_TYPE 0x2ff
#define MSR_AMD_SYSCFG 0xc0010010

#define CPUID_1_EDX_MTRR ((uint32_t)1 << 12)
// CPUID 01H EBX[15:8], the line size that CLFLUSH works on, in 8-byte units
// (SDM 325462-093 Vol. 2A, CPUID, Table 3-8).
#define CPUID_1_EBX_CLFLUSH_SHIFT 8
#define CPUID_1_EBX_CLFLUSH_MASK 0xff
#define CLFLUSH_UNIT 8
#define MTRRCAP_VCNT 0xff
#define MTRRCAP_FIX ((uint64_t)1 << 8)
#define MTRR_DEF_TYPE_E ((uint64_t)1 << 11)
#define SYSCFG_MTRR_FIX_DRAM_MOD_EN ((uint64_t)1 << 19)

// The variable ranges' registers run from 200h to 21Fh.
#define MAX_VARIABLE 16

static const uint32_t fixed_msrs[] = {
    0x250, 0x258, 0x259, 0x268, 0x269, 0x26a, 0x26b, 0x26c, 0x26d, 0x26e, 0x26f
};

struct mtrrs {
    uint64_t def_type;
    uint64_t fixed[ARRAY_COUNT(fixed_msrs)];
    uint64_t variable[2 * MAX_VARIABLE];
};

// The boot processor's MTRRs, which every other processor must have too (SDM
// 325462-093 Vol. 3A, 14.11.8), as Linux 7.3 gives them (cacheinfo.c,
// cache_cpu_init()).
static bool mtrrs_present;
static bool fixed_present;
static size_t variable_count;
static struct mtrrs boot_mtrrs;
static _Atomic(bool) reported;
// CLFLUSH's line size, found at its first use.
static _Atomic(size_t) flush_line;

static void mtrrs_read(struct mtrrs *m) {
    memset(m, 0, sizeof(struct mtrrs));
    m->def_type = rdmsr(MSR_MTRR_DEF_TYPE);
    for (size_t i = 0; fixed_present && i < ARRAY_COUNT(fixed_msrs); i++) {
        m->fixed[i] = rdmsr(fixed_msrs[i]);
    }
    for (size_t i = 0; i < 2 * variable_count; i++) {
        m->variable[i] = rdmsr(MSR_MTRR_PHYSBASE0 + (uint32_t)i);
    }
}

// With the ranges disabled while they change.
static void mtrrs_write(const struct mtrrs *m) {
    wrmsr(MSR_MTRR_DEF_TYPE, rdmsr(MSR_MTRR_DEF_TYPE) & ~MTRR_DEF_TYPE_E);
    for (size_t i = 0; fixed_present && i < ARRAY_COUNT(fixed_msrs); i++) {
        wrmsr(fixed_msrs[i], m->fixed[i]);
    }
    for (size_t i = 0; i < 2 * variable_count; i++) {
        wrmsr(MSR_MTRR_PHYSBASE0 + (uint32_t)i, m->variable[i]);
    }
    wrmsr(MSR_MTRR_DEF_TYPE, m->def_type);
}

// All of the TLBs: reloading CR3 leaves global entries, which a change of
// CR4.PGE flushes too (SDM 325462-093 Vol. 3A, 5.10.4.1).
static void tlb_flush_all(void) {
    uintptr_t cr4 = read_cr4();
    if ((cr4 & CR4_PGE) != 0) {
        write_cr4(cr4 & ~(uintptr_t)CR4_PGE);
        write_cr4(cr4);
    } else {
        write_cr3(read_cr3());
    }
}

void cache_cpu_init(uint64_t pat) {
    bool bsp = this_cpu()->bsp;
    // AMD's fixed ranges also say whether their reads and writes reach RAM or
    // memory-mapped I/O, which only firmware is to change, as MtrrFixDramModEn
    // lets it: cleared, as Linux 7.3 has it (mtrr/generic.c,
    // k8_check_syscfg_dram_mod_en()), copying them leaves those bits alone
    // (APM 24593 rev. 3.46, 7.9.1). Virtual machines may lack the register.
    uint64_t syscfg;
    if (x86_cpu_vendor() == X86_CPU_VENDOR_AMD && rdmsr_safe(MSR_AMD_SYSCFG, &syscfg)
     && (syscfg & SYSCFG_MTRR_FIX_DRAM_MOD_EN) != 0) {
        wrmsr_safe(MSR_AMD_SYSCFG, syscfg & ~SYSCFG_MTRR_FIX_DRAM_MOD_EN);
    }
    if (bsp) {
        mtrrs_present = (cpuid(1, 0).edx & CPUID_1_EDX_MTRR) != 0;
        if (mtrrs_present) {
            uint64_t cap = rdmsr(MSR_MTRRCAP);
            fixed_present = (cap & MTRRCAP_FIX) != 0;
            variable_count = cap & MTRRCAP_VCNT;
            if (variable_count > MAX_VARIABLE) {
                variable_count = MAX_VARIABLE;
            }
            mtrrs_read(&boot_mtrrs);
        }
    }
    bool copy = false;
    if (!bsp && mtrrs_present) {
        struct mtrrs mine;
        mtrrs_read(&mine);
        copy = memcmp(&mine, &boot_mtrrs, sizeof(struct mtrrs)) != 0;
    }

    // The MTRRs and the PAT change with caching and the TLBs as the SDM has it
    // for a processor: no-fill mode, the caches written back, the TLBs flushed
    // (CR4.PCIDE is clear), the change, the caches and TLBs again, and caching
    // back on (SDM 325462-093 Vol. 3A, 14.11.8 and 14.12.4).
    write_cr0((read_cr0() | CR0_CD) & ~(uintptr_t)CR0_NW);
    wbinvd();
    tlb_flush_all();
    if (copy) {
        mtrrs_write(&boot_mtrrs);
    }
    wrmsr(MSR_IA32_PAT, pat);
    wbinvd();
    tlb_flush_all();
    write_cr0(read_cr0() & ~(uintptr_t)(CR0_CD | CR0_NW));

    if (copy && !atomic_exchange_explicit(&reported, true, memory_order_relaxed)) {
        printf(
            "Processor %#x started with MTRRs unlike the boot processor's: it and any other such "
            "processor now have the boot processor's\n",
            this_cpu()->apic_id
        );
    }
}

void cache_write_back(const void *p, size_t size) {
    size_t line = atomic_load_explicit(&flush_line, memory_order_relaxed);
    if (line == 0) {
        uint32_t units = cpuid(1, 0).ebx >> CPUID_1_EBX_CLFLUSH_SHIFT & CPUID_1_EBX_CLFLUSH_MASK;
        // The smallest size that x86-64 processors have, where none is given.
        line = units != 0 ? units * CLFLUSH_UNIT : 64;
        atomic_store_explicit(&flush_line, line, memory_order_relaxed);
    }
    uintptr_t end = (uintptr_t)p + size;
    for (uintptr_t at = align_down((uintptr_t)p, line); at < end; at += line) {
        clflush((const void *)at);
    }
}
