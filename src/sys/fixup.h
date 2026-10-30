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

#ifndef SYS__FIXUP_H
#define SYS__FIXUP_H

#include <stdbool.h>
#include <stdint.h>

// Instructions that may fault, such as accesses to MSRs the guest names,
// register where execution resumes if they do, in fixup_table, which the
// linkers bound with __start_fixup_table and __stop_fixup_table. The addresses
// are relative to the fields holding them, so that nothing needs relocating. A
// relocation of no effect from the instruction to its entry keeps the entry
// wherever --gc-sections keeps the code, as LLD keeps no section for being
// bounded alone.
#define FIXUP_ENTRY(insn, resume) \
    ".pushsection fixup_table, \"a\"\n\t" \
    ".balign 4\n\t" \
    "3:\n\t" \
    ".long " insn " - .\n\t" \
    ".long " resume " - .\n\t" \
    ".popsection\n\t" \
    ".reloc " insn ", R_X86_64_NONE, 3b\n\t"

uintptr_t fixup_find(uintptr_t rip);

static inline bool rdmsr_safe(uint32_t msr, uint64_t *value) {
    uint32_t lo = 0, hi = 0;
    bool ok = false;
    asm volatile (
        "1: rdmsr\n\t"
        "movb $1, %2\n\t"
        "2:\n\t"
        FIXUP_ENTRY("1b", "2b")
        : "+a"(lo), "+d"(hi), "+r"(ok)
        : "c"(msr)
        : "memory"
    );
    *value = ((uint64_t)hi << 32) | lo;
    return ok;
}

static inline bool wrmsr_safe(uint32_t msr, uint64_t value) {
    bool ok = false;
    asm volatile (
        "1: wrmsr\n\t"
        "movb $1, %0\n\t"
        "2:\n\t"
        FIXUP_ENTRY("1b", "2b")
        : "+r"(ok)
        : "c"(msr), "a"((uint32_t)value), "d"((uint32_t)(value >> 32))
        : "memory"
    );
    return ok;
}

static inline bool xsetbv_safe(uint32_t index, uint64_t value) {
    bool ok = false;
    asm volatile (
        "1: xsetbv\n\t"
        "movb $1, %0\n\t"
        "2:\n\t"
        FIXUP_ENTRY("1b", "2b")
        : "+r"(ok)
        : "c"(index), "a"((uint32_t)value), "d"((uint32_t)(value >> 32))
        : "memory"
    );
    return ok;
}

#endif
