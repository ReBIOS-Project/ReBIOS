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
#include <sys/simd.h>
#include <lib/util.h>
#include <sys/cpu.h>
#include <sys/fixup.h>
#include <sys/mm.h>
#include <sys/x86.h>

// CPUID's flags for the levels (SDM 325462-093 Vol. 1, 21.3, Table 21-9 and
// Table 21-19).
#define CPUID1_SSSE3 ((uint32_t)1 << 9)
#define CPUID1_SSE4_1 ((uint32_t)1 << 19)
#define CPUID1_SSE4_2 ((uint32_t)1 << 20)
#define CPUID1_AVX ((uint32_t)1 << 28)
#define CPUID7_EBX_AVX2 ((uint32_t)1 << 5)

// The state components that each level's registers are in. Instructions with a
// VEX prefix clear the upper halves of the ZMM registers too, where the
// processor has them.
#define STATE_SSE XCR0_SSE
#define STATE_AVX (STATE_SSE | XCR0_AVX)

static bool usable[SIMD_LEVELS];
static bool xsave_on;
// The size of an XSAVE area for every state component the processor has, or of
// FXSAVE's.
static size_t area_size;

static const char *const level_names[SIMD_LEVELS] = {
    "SSE2", "SSSE3", "SSE4.1", "SSE4.2", "AVX", "AVX2"
};

void simd_init(void) {
    uint32_t max_leaf = cpuid(0, 0).eax;
    struct cpuid_regs one = cpuid(1, 0);
    struct cpuid_regs seven = { 0 };
    if (max_leaf >= 7) {
        seven = cpuid(7, 0);
    }
    uint64_t components = 0;
    xsave_on = (read_cr4() & CR4_OSXSAVE) != 0;
    if (xsave_on) {
        struct cpuid_regs state = cpuid(0xd, 0);
        components = state.eax | (uint64_t)state.edx << 32;
        area_size = state.ecx;
    } else {
        area_size = FXSAVE_SIZE;
    }
    usable[SIMD_SSE2] = true;
    usable[SIMD_SSSE3] = (one.ecx & CPUID1_SSSE3) != 0;
    usable[SIMD_SSE4_1] = usable[SIMD_SSSE3] && (one.ecx & CPUID1_SSE4_1) != 0;
    usable[SIMD_SSE4_2] = usable[SIMD_SSE4_1] && (one.ecx & CPUID1_SSE4_2) != 0;
    usable[SIMD_AVX] = usable[SIMD_SSE4_2] && (one.ecx & CPUID1_AVX) != 0 && xsave_on
                    && (components & STATE_AVX) == STATE_AVX;
    usable[SIMD_AVX2] = usable[SIMD_AVX] && (seven.ebx & CPUID7_EBX_AVX2) != 0;
}

void simd_cpu_init(struct cpu *cpu) {
    cpu->simd_area = page_alloc(page_count(area_size));
}

bool simd_usable(size_t level) {
    return level < SIMD_LEVELS && usable[level];
}

const char *simd_level_name(size_t level) {
    return level < SIMD_LEVELS ? level_names[level] : "none";
}

// Instructions without a VEX prefix leave the upper halves of the YMM and ZMM
// registers as they are, so FXSAVE keeps all that SSE levels change, whatever
// XCR0 holds. XCR0 stays the guest's while the hypervisor runs, as the
// hypervisor executes the guest's XSETBV, and may leave out the SSE state,
// which SSE instructions use all the same, and the AVX state, which the
// processor keeps as it is while XCR0 leaves it out (SDM 325462-093 Vol. 1,
// 13.3). XCR0 takes both meanwhile, and XRSTOR puts back all that XSAVE took.
void simd_begin(size_t level) {
    struct cpu *cpu = this_cpu();
    if (level < SIMD_AVX) {
        cpu->simd_saved = 0;
        fxsave(cpu->simd_area);
        return;
    }
    uint64_t xcr0 = xgetbv(0);
    uint64_t saved = STATE_AVX | (xcr0 & XCR0_ZMM_HI256);
    if ((xcr0 & STATE_AVX) != STATE_AVX) {
        xsetbv_safe(0, xcr0 | STATE_AVX);
    }
    cpu->simd_xcr0 = xcr0;
    cpu->simd_saved = saved;
    xsave(cpu->simd_area, saved);
}

void simd_end(void) {
    struct cpu *cpu = this_cpu();
    if (cpu->simd_saved == 0) {
        fxrstor(cpu->simd_area);
        return;
    }
    xrstor(cpu->simd_area, cpu->simd_saved);
    if ((cpu->simd_xcr0 & STATE_AVX) != STATE_AVX) {
        xsetbv_safe(0, cpu->simd_xcr0);
    }
}
