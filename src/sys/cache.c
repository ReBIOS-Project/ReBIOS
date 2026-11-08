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
#include <sys/cache.h>
#include <sys/x86.h>

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
    // The PAT changes with caching and the TLBs as the SDM has it for a
    // processor: no-fill mode, the caches written back, the TLBs flushed
    // (CR4.PCIDE is clear), the change, the caches and TLBs again, and caching
    // back on (SDM 325462-093 Vol. 3A, 14.11.8 and 14.12.4).
    write_cr0((read_cr0() | CR0_CD) & ~(uintptr_t)CR0_NW);
    wbinvd();
    tlb_flush_all();
    wrmsr(MSR_IA32_PAT, pat);
    wbinvd();
    tlb_flush_all();
    write_cr0(read_cr0() & ~(uintptr_t)(CR0_CD | CR0_NW));
}
