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

#ifndef SYS__SIMD_H
#define SYS__SIMD_H

#include <stdbool.h>
#include <stddef.h>

// The levels of SIMD instructions that code built for them uses, each with the
// instructions of those below it.
#define SIMD_SSE2 0
#define SIMD_SSSE3 1
#define SIMD_SSE4_1 2
#define SIMD_SSE4_2 3
#define SIMD_AVX 4
#define SIMD_AVX2 5
#define SIMD_LEVELS 6

struct cpu;

// On the boot processor, before simd_cpu_init(): which levels the processors
// have.
void simd_init(void);
// On each processor, its control registers set.
void simd_cpu_init(struct cpu *cpu);
bool simd_usable(size_t level);
const char *simd_level_name(size_t level);

// Around the hypervisor's own use of the SIMD registers of a level, which
// leaves the guest's registers as they were.
void simd_begin(size_t level);
void simd_end(void);

#endif
