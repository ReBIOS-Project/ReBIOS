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
#include <uacpi/acpi.h>
#include <uacpi/status.h>
#include <uacpi/tables.h>
#include <hw/tsc.h>
#include <hw/acpi.h>
#include <hw/pm_timer.h>
#include <lib/log.h>
#include <lib/time.h>
#include <lib/util.h>
#include <memory.h>
#include <sys/mm.h>
#include <sys/x86.h>

#define FS_PER_SECOND ((uint64_t)1000000000000000)

// A round's length, and the rounds of a measurement, whose median it gives.
#define WINDOW_FS ((uint64_t)50000000000000)
#define ROUNDS 3
// The reads a round starts with, for the quickest of them.
#define START_READS 8

// The HPET's period, configuration, and main counter registers (IA-PC HPET
// Specification 1.0a, 2.3).
#define HPET_PERIOD 0x04
#define HPET_CONFIG 0x10
#define HPET_CONFIG_ENABLE ((uint32_t)1 << 0)
#define HPET_COUNTER 0xf0
// The periods Linux 7.3 takes (asm/hpet.h), the specification's limit, and the
// TSC cycles within which it sees a running counter move (hpet.c,
// hpet_counting()).
#define HPET_PERIOD_MIN 100000
#define HPET_PERIOD_MAX 100000000
#define HPET_COUNTING_CYCLES 200000

// The crystals of SDM 325462-093 Vol. 3B, Table 22-95: the Xeon Scalable
// processors', 6th and 7th generation Core processors', and Goldmont Atoms'.
#define CRYSTAL_SKX_HZ 25000000
#define CRYSTAL_CORE_HZ 24000000
#define CRYSTAL_GOLDMONT_HZ 19200000

// A counter that the TSC is measured against, which wraps at mask.
struct reference {
    const char *name;
    uint32_t (*read)(void);
    uint32_t mask;
    uint64_t fs_per_tick;
};

static uintptr_t hpet;

static uint32_t hpet_register(uint32_t offset) {
    return *(volatile uint32_t *)(hpet + offset);
}

// The low half of the main counter, as Linux 7.3 reads it.
static uint32_t hpet_read(void) {
    return hpet_register(HPET_COUNTER);
}

// The first HPET that the firmware's ACPI names, where the identity map has
// it, its counter runs, and its period is one that Linux 7.3 takes.
static bool hpet_reference(struct reference *r) {
    struct uacpi_table table;
    if (!acpi_tables_ready()
     || uacpi_table_find_by_signature(ACPI_HPET_SIGNATURE, &table) != UACPI_STATUS_OK) {
        return false;
    }
    if (table.hdr->length >= sizeof(struct acpi_hpet)) {
        struct acpi_hpet h;
        memcpy(&h, table.ptr, sizeof(struct acpi_hpet));
        if (h.address.address_space_id == ACPI_AS_ID_SYS_MEM && h.address.address != 0
         && h.address.address <= FOUR_GIB - PAGE_SIZE) {
            hpet = h.address.address;
        }
    }
    uacpi_table_unref(&table);
    if (hpet == 0) {
        return false;
    }
    uint32_t period = hpet_register(HPET_PERIOD);
    if (period < HPET_PERIOD_MIN || period > HPET_PERIOD_MAX
     || (hpet_register(HPET_CONFIG) & HPET_CONFIG_ENABLE) == 0) {
        return false;
    }
    uint32_t first = hpet_read();
    uint64_t start = rdtsc();
    while (hpet_read() == first) {
        if (rdtsc() - start > HPET_COUNTING_CYCLES) {
            return false;
        }
    }
    *r = (struct reference){ "the HPET", hpet_read, UINT32_MAX, period };
    return true;
}

// One round of the TSC against r: its ends are reads of r between reads of the
// TSC, taken at their middles, as Ironclad takes them, the start at the
// quickest of START_READS reads, and the end at the first read past the window
// that took at most twice as long, or else past twice the window. False if r
// takes more than a second by the estimate.
static bool measure_round(const struct reference *r, uint64_t estimate, uint64_t *hz) {
    uint64_t window = WINDOW_FS / r->fs_per_tick;
    uint64_t quickest = UINT64_MAX;
    uint64_t start_tsc = 0;
    uint32_t start = 0;
    for (size_t i = 0; i < START_READS; i++) {
        uint64_t before = rdtsc();
        uint32_t value = r->read();
        uint64_t after = rdtsc();
        if (after - before < quickest) {
            quickest = after - before;
            start = value;
            start_tsc = before + quickest / 2;
        }
    }
    uint64_t deadline = rdtsc() + estimate;
    for (;;) {
        uint64_t before = rdtsc();
        uint32_t value = r->read();
        uint64_t after = rdtsc();
        uint64_t ticks = (value - start) & r->mask;
        if ((ticks >= window && after - before <= 2 * quickest) || ticks >= 2 * window) {
            uint64_t ns = ticks * r->fs_per_tick / 1000000;
            *hz = (before + (after - before) / 2 - start_tsc) * 1000000000 / ns;
            return true;
        }
        if ((int64_t)(after - deadline) > 0) {
            return false;
        }
    }
}

static bool measure(const struct reference *r, uint64_t estimate, uint64_t *hz) {
    uint64_t rounds[ROUNDS];
    for (size_t i = 0; i < ROUNDS; i++) {
        if (!measure_round(r, estimate, &rounds[i])) {
            return false;
        }
        for (size_t j = i; j > 0 && rounds[j] < rounds[j - 1]; j--) {
            uint64_t swapped = rounds[j];
            rounds[j] = rounds[j - 1];
            rounds[j - 1] = swapped;
        }
    }
    *hz = rounds[ROUNDS / 2];
    return true;
}

// The TSC's nominal rate, from CPUID leaf 15h's ratio to the crystal and the
// crystal's rate (SDM 325462-093 Vol. 3B, 22.7.3), or Table 22-95's crystal
// for the processors it lists, where the leaf leaves the crystal out; 0 where
// neither gives it.
static uint64_t nominal_hz(void) {
    if (x86_cpu_vendor() != X86_CPU_VENDOR_INTEL || cpuid(0, 0).eax < 0x15) {
        return 0;
    }
    struct cpuid_regs leaf = cpuid(0x15, 0);
    if (leaf.eax == 0 || leaf.ebx == 0) {
        return 0;
    }
    uint64_t crystal = leaf.ecx;
    struct x86_cpu_signature s = x86_cpu_signature();
    if (crystal == 0 && s.family == 6) {
        switch (s.model) {
            case 0x55: {
                crystal = CRYSTAL_SKX_HZ;
                break;
            }
            case 0x4e:
            case 0x5e:
            case 0x8e:
            case 0x9e: {
                crystal = CRYSTAL_CORE_HZ;
                break;
            }
            case 0x5c: {
                crystal = CRYSTAL_GOLDMONT_HZ;
                break;
            }
            default: {
                break;
            }
        }
    }
    return crystal * leaf.ebx / leaf.eax;
}

// Whether a is within 1% of b.
static bool agree(uint64_t a, uint64_t b) {
    uint64_t difference = a > b ? a - b : b - a;
    return difference <= b / 100;
}

void tsc_calibrate(void) {
    uint64_t estimate = time_tsc_hz();
    struct reference references[2];
    size_t count = 0;
    if (hpet_reference(&references[count])) {
        count++;
    }
    if (pm_timer_sound()) {
        references[count++] = (struct reference){
            "the PM timer", pm_timer_read, pm_timer_mask(), FS_PER_SECOND / PM_TIMER_HZ
        };
    }
    uint64_t hz = estimate;
    const char *source = "the firmware's Stall()";
    for (size_t i = 0; i < count; i++) {
        uint64_t measured;
        if (measure(&references[i], estimate, &measured)) {
            hz = measured;
            source = references[i].name;
            break;
        }
    }

    // The nominal rate is exact where it is right, which Table 22-95's crystal
    // for 06_55H is not on the Xeon W processors that share it.
    uint64_t nominal = nominal_hz();
    bool nominal_agrees = nominal != 0 && agree(nominal, hz);
    if (nominal_agrees) {
        hz = nominal;
    }
    time_set_tsc_hz(hz);
    printf(
        "TSC at %llu Hz, %s %s\n", (unsigned long long)hz,
        nominal_agrees ? "its nominal rate, which agrees with" : "as measured with", source
    );
    if (!agree(estimate, hz)) {
        printf("The firmware's Stall() put it at %llu Hz\n", (unsigned long long)estimate);
    }
}
