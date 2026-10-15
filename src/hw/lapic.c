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
#include <stdbool.h>
#include <hw/lapic.h>
#include <lib/log.h>
#include <lib/time.h>
#include <sys/fixup.h>
#include <sys/x86.h>

#define MSR_IA32_ARCH_CAPABILITIES 0x10a
#define MSR_IA32_XAPIC_DISABLE_STATUS 0xbd
#define MSR_X2APIC_FIRST 0x800
#define MSR_X2APIC_ICR 0x830

#define ARCH_CAPABILITIES_XAPIC_DISABLE_STATUS (1ull << 21)
#define LEGACY_XAPIC_DISABLED (1ull << 0)

#define APIC_BASE_EXTD (1ull << 10)
#define APIC_BASE_EN (1ull << 11)
#define APIC_BASE_ADDRESS 0x000ffffffffff000ull

#define APIC_VERSION 0x30
#define APIC_TPR 0x80
#define APIC_EOI 0xb0
#define APIC_LDR 0xd0
#define APIC_DFR 0xe0
#define APIC_SVR 0xf0
#define APIC_ISR 0x100
#define APIC_ESR 0x280
#define APIC_LVT_CMCI 0x2f0
#define APIC_ICR_LOW 0x300
#define APIC_ICR_HIGH 0x310
#define APIC_LVT_TIMER 0x320
#define APIC_LVT_THERMAL 0x330
#define APIC_LVT_PERF 0x340
#define APIC_LVT_LINT0 0x350
#define APIC_LVT_LINT1 0x360
#define APIC_LVT_ERROR 0x370
#define APIC_TIMER_INITIAL 0x380
#define APIC_TIMER_DIVIDE 0x3e0

#define LVT_MASKED (1u << 16)
// CMCI has an LVT entry where the version register's maximum entry is this
// or more.
#define LVT_CMCI_MIN_MAX_ENTRY 6

#define IPI_TIMEOUT_NS 100000000ull

static volatile uint32_t *xapic_reg(uint32_t reg) {
    uint64_t base = rdmsr(MSR_IA32_APIC_BASE) & APIC_BASE_ADDRESS;
    return (volatile uint32_t *)(uintptr_t)(base + reg);
}

static uint32_t lapic_read(uint32_t reg) {
    if (lapic_x2apic()) {
        return (uint32_t)rdmsr(MSR_X2APIC_FIRST + (reg >> 4));
    }
    return *xapic_reg(reg);
}

static void lapic_write(uint32_t reg, uint32_t value) {
    if (lapic_x2apic()) {
        wrmsr(MSR_X2APIC_FIRST + (reg >> 4), value);
        return;
    }
    *xapic_reg(reg) = value;
}

bool lapic_x2apic(void) {
    uint64_t base = rdmsr(MSR_IA32_APIC_BASE);
    return (base & (APIC_BASE_EN | APIC_BASE_EXTD)) == (APIC_BASE_EN | APIC_BASE_EXTD);
}

uint32_t lapic_cpu_apic_id(void) {
    if (cpuid(0, 0).eax >= 0xb) {
        struct cpuid_regs topology = cpuid(0xb, 0);
        if (topology.ebx != 0) {
            return topology.edx;
        }
    }
    return cpuid(1, 0).ebx >> 24;
}

// Whether the processor has the legacy xAPIC interface turned off for good.
static bool xapic_disabled(void) {
    if (cpuid(0, 0).eax < 7 || !(cpuid(7, 0).edx & (1u << 29))) {
        return false;
    }
    uint64_t caps;
    if (!rdmsr_safe(MSR_IA32_ARCH_CAPABILITIES, &caps)
        || !(caps & ARCH_CAPABILITIES_XAPIC_DISABLE_STATUS)) {
        return false;
    }
    uint64_t status;
    return rdmsr_safe(MSR_IA32_XAPIC_DISABLE_STATUS, &status) && (status & LEGACY_XAPIC_DISABLED);
}

void lapic_send_ipi(uint32_t dest, uint32_t command) {
    if (lapic_x2apic()) {
        wrmsr(MSR_X2APIC_ICR, (uint64_t)dest << 32 | command);
        return;
    }
    uint64_t deadline = time_ns() + IPI_TIMEOUT_NS;
    while (*xapic_reg(APIC_ICR_LOW) & ICR_PENDING) {
        if (time_ns() > deadline) {
            printf("Local APIC %#x: an earlier IPI is still pending\n", lapic_cpu_apic_id());
            break;
        }
        cpu_relax();
    }
    // The guest may be between its writes to the two halves.
    uint32_t guest_high = *xapic_reg(APIC_ICR_HIGH);
    *xapic_reg(APIC_ICR_HIGH) = dest << 24;
    *xapic_reg(APIC_ICR_LOW) = command;
    while (*xapic_reg(APIC_ICR_LOW) & ICR_PENDING) {
        if (time_ns() > deadline) {
            printf("Local APIC %#x: IPI %#x not taken\n", lapic_cpu_apic_id(), command);
            break;
        }
        cpu_relax();
    }
    *xapic_reg(APIC_ICR_HIGH) = guest_high;
}

uint8_t lapic_highest_isr(void) {
    for (int i = 7; i >= 0; i--) {
        uint32_t isr = lapic_read(APIC_ISR + (uint32_t)i * 0x10);
        if (isr != 0) {
            return (uint8_t)(i * 32 + 31 - __builtin_clz(isr));
        }
    }
    return 0;
}

bool lapic_in_service(uint8_t vector) {
    return (lapic_read(APIC_ISR + (uint32_t)(vector / 32) * 0x10) >> (vector % 32)) & 1;
}

void lapic_xapic_mode(void) {
    uint64_t base = rdmsr(MSR_IA32_APIC_BASE);
    if ((base & APIC_BASE_EXTD) && !xapic_disabled()) {
        // x2APIC mode can only be left for the disabled state.
        base &= ~(APIC_BASE_EN | APIC_BASE_EXTD);
        wrmsr(MSR_IA32_APIC_BASE, base);
    }
    if (!(base & APIC_BASE_EN)) {
        wrmsr(MSR_IA32_APIC_BASE, base | APIC_BASE_EN);
    }
}

void lapic_init_reset(void) {
    uint32_t max_entry = (lapic_read(APIC_VERSION) >> 16) & 0xff;
    static const uint32_t lvts[] = {
        APIC_LVT_TIMER, APIC_LVT_THERMAL, APIC_LVT_PERF, APIC_LVT_LINT0, APIC_LVT_LINT1,
        APIC_LVT_ERROR,
    };
    for (unsigned i = 0; i < sizeof(lvts) / sizeof(lvts[0]); i++) {
        lapic_write(lvts[i], LVT_MASKED);
    }
    if (max_entry >= LVT_CMCI_MIN_MAX_ENTRY) {
        lapic_write(APIC_LVT_CMCI, LVT_MASKED);
    }
    lapic_write(APIC_TIMER_INITIAL, 0);
    lapic_write(APIC_TIMER_DIVIDE, 0);
    lapic_write(APIC_TPR, 0);
    lapic_write(APIC_ESR, 0);
    if (!lapic_x2apic()) {
        lapic_write(APIC_LDR, 0);
        lapic_write(APIC_DFR, 0xffffffff);
    }
    // In-service interrupts end one EOI each, highest first.
    for (unsigned n = 0; n < 256; n++) {
        bool in_service = false;
        for (uint32_t reg = APIC_ISR; reg < APIC_ISR + 0x80; reg += 0x10) {
            in_service |= lapic_read(reg) != 0;
        }
        if (!in_service) {
            break;
        }
        lapic_write(APIC_EOI, 0);
    }
    lapic_write(APIC_SVR, 0xff);
}
