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

#include <stdint.h>
#include <stdbool.h>

#define CR0_PE (1u << 0)
#define CR0_MP (1u << 1)
#define CR0_EM (1u << 2)
#define CR0_TS (1u << 3)
#define CR0_ET (1u << 4)
#define CR0_NE (1u << 5)
#define CR0_WP (1u << 16)
#define CR0_AM (1u << 18)
#define CR0_NW (1u << 29)
#define CR0_CD (1u << 30)
#define CR0_PG (1u << 31)

#define CR4_VME (1u << 0)
#define CR4_PVI (1u << 1)
#define CR4_TSD (1u << 2)
#define CR4_DE (1u << 3)
#define CR4_PSE (1u << 4)
#define CR4_PAE (1u << 5)
#define CR4_MCE (1u << 6)
#define CR4_PGE (1u << 7)
#define CR4_PCE (1u << 8)
#define CR4_OSFXSR (1u << 9)
#define CR4_OSXMMEXCPT (1u << 10)
#define CR4_UMIP (1u << 11)
#define CR4_LA57 (1u << 12)
#define CR4_VMXE (1u << 13)
#define CR4_SMXE (1u << 14)
#define CR4_FSGSBASE (1u << 16)
#define CR4_PCIDE (1u << 17)
#define CR4_OSXSAVE (1u << 18)
#define CR4_SMEP (1u << 20)
#define CR4_SMAP (1u << 21)
#define CR4_PKE (1u << 22)
#define CR4_CET (1u << 23)

#define CR3_LAM_U57 (1ull << 61)
#define CR3_LAM_U48 (1ull << 62)

#define MSR_IA32_APIC_BASE 0x1b
#define MSR_IA32_FEATURE_CONTROL 0x3a
#define MSR_IA32_PAT 0x277
#define MSR_EFER 0xc0000080

#define EFER_SCE (1u << 0)
#define EFER_LME (1u << 8)
#define EFER_LMA (1u << 10)
#define EFER_NXE (1u << 11)
#define EFER_SVME (1u << 12)

#define RFLAGS_CF (1u << 0)
#define RFLAGS_FIXED (1u << 1)
#define RFLAGS_PF (1u << 2)
#define RFLAGS_AF (1u << 4)
#define RFLAGS_ZF (1u << 6)
#define RFLAGS_SF (1u << 7)
#define RFLAGS_TF (1u << 8)
#define RFLAGS_IF (1u << 9)
#define RFLAGS_DF (1u << 10)
#define RFLAGS_OF (1u << 11)
#define RFLAGS_IOPL (3u << 12)
#define RFLAGS_NT (1u << 14)
#define RFLAGS_RF (1u << 16)
#define RFLAGS_VM (1u << 17)
#define RFLAGS_AC (1u << 18)
#define RFLAGS_VIF (1u << 19)
#define RFLAGS_VIP (1u << 20)
#define RFLAGS_ID (1u << 21)

struct cpuid_regs {
    uint32_t eax;
    uint32_t ebx;
    uint32_t ecx;
    uint32_t edx;
};

static inline struct cpuid_regs cpuid(uint32_t leaf, uint32_t subleaf) {
    struct cpuid_regs r;
    asm volatile ("cpuid"
                  : "=a"(r.eax), "=b"(r.ebx), "=c"(r.ecx), "=d"(r.edx)
                  : "a"(leaf), "c"(subleaf));
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
    struct cpu_signature s = {(eax >> 8) & 0xf, (eax >> 4) & 0xf, eax & 0xf};
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
    asm volatile ("wrmsr" :: "c"(msr), "a"((uint32_t)value), "d"((uint32_t)(value >> 32))
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

static inline void cpu_relax(void) {
    asm volatile ("pause" ::: "memory");
}

static inline void cli(void) {
    asm volatile ("cli" ::: "memory");
}

static inline void sti(void) {
    asm volatile ("sti" ::: "memory");
}

static inline void hlt(void) {
    asm volatile ("hlt" ::: "memory");
}

static inline void wbinvd(void) {
    asm volatile ("wbinvd" ::: "memory");
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
