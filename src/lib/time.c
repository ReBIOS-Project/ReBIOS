// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of ReBIOS.
// Copyright (C) 2026 Mintsuki and contributors
//
// ReBIOS is free software: you can redistribute it and/or modify it under the
// terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version.
//
// ReBIOS is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
// FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
// details.
//
// You should have received a copy of the GNU General Public License along
// with ReBIOS. If not, see <https://www.gnu.org/licenses/>.

#include <stdint.h>
#include <lib/time.h>
#include <sys/x86.h>

static uint64_t tsc_hz;
static uint64_t tsc_start;

void time_init(uint64_t hz) {
    tsc_hz = hz;
    tsc_start = rdtsc();
}

uint64_t time_tsc_hz(void) {
    return tsc_hz;
}

uint64_t time_ns(void) {
    uint64_t ticks = rdtsc() - tsc_start;
    // Split, so that the product cannot overflow.
    return ticks / tsc_hz * 1000000000 + ticks % tsc_hz * 1000000000 / tsc_hz;
}

void time_udelay(uint64_t us) {
    uint64_t end = rdtsc() + (tsc_hz * us + 999999) / 1000000;
    while ((int64_t)(rdtsc() - end) < 0) {
        cpu_relax();
    }
}
