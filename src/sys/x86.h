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

#ifndef SYS__X86_H
#define SYS__X86_H

#include <stdbool.h>
#include <stdint.h>

#define CR0_PE ((uint32_t)1 << 0)
#define CR0_MP ((uint32_t)1 << 1)
#define CR0_EM ((uint32_t)1 << 2)
#define CR0_TS ((uint32_t)1 << 3)
#define CR0_ET ((uint32_t)1 << 4)
#define CR0_NE ((uint32_t)1 << 5)
#define CR0_WP ((uint32_t)1 << 16)
#define CR0_AM ((uint32_t)1 << 18)
#define CR0_NW ((uint32_t)1 << 29)
#define CR0_CD ((uint32_t)1 << 30)
#define CR0_PG ((uint32_t)1 << 31)

#define CR4_VME ((uint32_t)1 << 0)
#define CR4_PVI ((uint32_t)1 << 1)
#define CR4_TSD ((uint32_t)1 << 2)
#define CR4_DE ((uint32_t)1 << 3)
#define CR4_PSE ((uint32_t)1 << 4)
#define CR4_PAE ((uint32_t)1 << 5)
#define CR4_MCE ((uint32_t)1 << 6)
#define CR4_PGE ((uint32_t)1 << 7)
#define CR4_PCE ((uint32_t)1 << 8)
#define CR4_OSFXSR ((uint32_t)1 << 9)
#define CR4_OSXMMEXCPT ((uint32_t)1 << 10)
#define CR4_UMIP ((uint32_t)1 << 11)
#define CR4_LA57 ((uint32_t)1 << 12)
#define CR4_VMXE ((uint32_t)1 << 13)
#define CR4_SMXE ((uint32_t)1 << 14)
#define CR4_FSGSBASE ((uint32_t)1 << 16)
#define CR4_PCIDE ((uint32_t)1 << 17)
#define CR4_OSXSAVE ((uint32_t)1 << 18)
#define CR4_KL ((uint32_t)1 << 19)
#define CR4_SMEP ((uint32_t)1 << 20)
#define CR4_SMAP ((uint32_t)1 << 21)
#define CR4_PKE ((uint32_t)1 << 22)
#define CR4_CET ((uint32_t)1 << 23)
#define CR4_PKS ((uint32_t)1 << 24)
#define CR4_UINTR ((uint32_t)1 << 25)
#define CR4_LASS ((uint32_t)1 << 27)
#define CR4_LAM_SUP ((uint32_t)1 << 28)
#define CR4_FRED ((uint64_t)1 << 32)

#define CR3_LAM_U57 ((uint64_t)1 << 61)
#define CR3_LAM_U48 ((uint64_t)1 << 62)

#define MSR_IA32_APIC_BASE 0x1b
#define MSR_IA32_FEATURE_CONTROL 0x3a
#define MSR_IA32_MISC_ENABLE 0x1a0
#define MSR_IA32_PAT 0x277
#define MSR_IA32_PERF_GLOBAL_CTRL 0x38f
#define MSR_IA32_RTIT_STATUS 0x571
#define MSR_EFER 0xc0000080

// SDM 325462-093 Vol. 4, Tables 2-2 and 2-3.
#define MISC_ENABLE_LIMIT_CPUID ((uint64_t)1 << 22)
#define MISC_ENABLE_XD_DISABLE ((uint64_t)1 << 34)

// SDM 325462-093 Vol. 3C, Tables 36-6 and 36-7.
#define RTIT_CTL_TRACEEN ((uint32_t)1 << 0)
#define RTIT_STATUS_ERROR ((uint32_t)1 << 4)

#define EFER_SCE ((uint32_t)1 << 0)
#define EFER_LME ((uint32_t)1 << 8)
#define EFER_LMA ((uint32_t)1 << 10)
#define EFER_NXE ((uint32_t)1 << 11)
#define EFER_SVME ((uint32_t)1 << 12)
#define EFER_LMSLE ((uint32_t)1 << 13)
#define EFER_FFXSR ((uint32_t)1 << 14)
#define EFER_TCE ((uint32_t)1 << 15)
#define EFER_UAIE ((uint32_t)1 << 20)
#define EFER_AIBRSE ((uint32_t)1 << 21)

#define RFLAGS_CF ((uint32_t)1 << 0)
#define RFLAGS_FIXED ((uint32_t)1 << 1)
#define RFLAGS_PF ((uint32_t)1 << 2)
#define RFLAGS_AF ((uint32_t)1 << 4)
#define RFLAGS_ZF ((uint32_t)1 << 6)
#define RFLAGS_SF ((uint32_t)1 << 7)
#define RFLAGS_TF ((uint32_t)1 << 8)
#define RFLAGS_IF ((uint32_t)1 << 9)
#define RFLAGS_DF ((uint32_t)1 << 10)
#define RFLAGS_OF ((uint32_t)1 << 11)
#define RFLAGS_IOPL ((uint32_t)3 << 12)
#define RFLAGS_NT ((uint32_t)1 << 14)
#define RFLAGS_RF ((uint32_t)1 << 16)
#define RFLAGS_VM ((uint32_t)1 << 17)
#define RFLAGS_AC ((uint32_t)1 << 18)
#define RFLAGS_VIF ((uint32_t)1 << 19)
#define RFLAGS_VIP ((uint32_t)1 << 20)
#define RFLAGS_ID ((uint32_t)1 << 21)

struct cpuid_regs {
    uint32_t eax;
    uint32_t ebx;
    uint32_t ecx;
    uint32_t edx;
};

static inline struct cpuid_regs cpuid(uint32_t leaf, uint32_t subleaf) {
    struct cpuid_regs r;
    asm volatile (
        "cpuid"
        : "=a"(r.eax), "=b"(r.ebx), "=c"(r.ecx), "=d"(r.edx)
        : "a"(leaf), "c"(subleaf)
    );
    return r;
}

// CPUID leaf 1's display family, model, and stepping, which both vendors
// define alike: the extended model counts for base families 6 and 0Fh.
struct cpu_signature {
    uint32_t family;
    uint32_t model;
    uint32_t stepping;
};

static inline struct cpu_signature cpu_signature(void) {
    uint32_t eax = cpuid(1, 0).eax;
    struct cpu_signature s = { (eax >> 8) & 0xf, (eax >> 4) & 0xf, eax & 0xf };
    if (s.family == 6 || s.family == 0xf) {
        s.model |= ((eax >> 16) & 0xf) << 4;
    }
    if (s.family == 0xf) {
        s.family += (eax >> 20) & 0xff;
    }
    return s;
}

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    asm volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr) : "memory");
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t value) {
    asm volatile (
        "wrmsr"
        :
        : "c"(msr), "a"((uint32_t)value), "d"((uint32_t)(value >> 32))
            : "memory");
}

static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    asm volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static inline void outb(uint16_t port, uint8_t value) {
    asm volatile ("outb %0, %1" :: "a"(value), "Nd"(port) : "memory");
}

static inline void outw(uint16_t port, uint16_t value) {
    asm volatile ("outw %0, %1" :: "a"(value), "Nd"(port) : "memory");
}

static inline void outl(uint16_t port, uint32_t value) {
    asm volatile ("outl %0, %1" :: "a"(value), "Nd"(port) : "memory");
}

static inline uint8_t inb(uint16_t port) {
    uint8_t value;
    asm volatile ("inb %1, %0" : "=a"(value) : "Nd"(port) : "memory");
    return value;
}

static inline uint16_t inw(uint16_t port) {
    uint16_t value;
    asm volatile ("inw %1, %0" : "=a"(value) : "Nd"(port) : "memory");
    return value;
}

static inline uint32_t inl(uint16_t port) {
    uint32_t value;
    asm volatile ("inl %1, %0" : "=a"(value) : "Nd"(port) : "memory");
    return value;
}

static inline uintptr_t read_cr0(void) {
    uintptr_t value;
    asm volatile ("mov %%cr0, %0" : "=r"(value));
    return value;
}

static inline void write_cr0(uintptr_t value) {
    asm volatile ("mov %0, %%cr0" :: "r"(value) : "memory");
}

static inline uintptr_t read_cr2(void) {
    uintptr_t value;
    asm volatile ("mov %%cr2, %0" : "=r"(value));
    return value;
}

static inline void write_cr2(uintptr_t value) {
    asm volatile ("mov %0, %%cr2" :: "r"(value));
}

static inline uintptr_t read_cr3(void) {
    uintptr_t value;
    asm volatile ("mov %%cr3, %0" : "=r"(value));
    return value;
}

static inline void write_cr3(uintptr_t value) {
    asm volatile ("mov %0, %%cr3" :: "r"(value) : "memory");
}

static inline uintptr_t read_cr4(void) {
    uintptr_t value;
    asm volatile ("mov %%cr4, %0" : "=r"(value));
    return value;
}

static inline void write_cr4(uintptr_t value) {
    asm volatile ("mov %0, %%cr4" :: "r"(value) : "memory");
}

static inline uintptr_t read_dr6(void) {
    uintptr_t value;
    asm volatile ("mov %%dr6, %0" : "=r"(value));
    return value;
}

static inline uintptr_t read_dr7(void) {
    uintptr_t value;
    asm volatile ("mov %%dr7, %0" : "=r"(value));
    return value;
}

static inline void cpu_relax(void) {
    asm volatile ("pause" :: : "memory");
}

static inline void cli(void) {
    asm volatile ("cli" :: : "memory");
}

static inline void sti(void) {
    asm volatile ("sti" :: : "memory");
}

static inline void hlt(void) {
    asm volatile ("hlt" :: : "memory");
}

static inline void wbinvd(void) {
    asm volatile ("wbinvd" :: : "memory");
}

static inline void invlpg(uintptr_t addr) {
    asm volatile ("invlpg (%0)" :: "r"(addr) : "memory");
}

__attribute__((noreturn)) static inline void halt_forever(void) {
    for (;;) {
        cli();
        hlt();
    }
}

#endif
