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
#include <hw/lapic.h>
#include <lib/log.h>
#include <lib/time.h>
#include <sys/cpu.h>
#include <sys/fixup.h>
#include <sys/x86.h>

#define MSR_IA32_ARCH_CAPABILITIES 0x10a
#define MSR_IA32_XAPIC_DISABLE_STATUS 0xbd
#define MSR_X2APIC_FIRST 0x800
#define MSR_X2APIC_ICR 0x830

#define ARCH_CAPABILITIES_XAPIC_DISABLE_STATUS ((uint64_t)1 << 21)
#define LEGACY_XAPIC_DISABLED ((uint64_t)1 << 0)

#define APIC_BASE_EXTD ((uint64_t)1 << 10)
#define APIC_BASE_EN ((uint64_t)1 << 11)
#define APIC_BASE_ADDRESS ((uint64_t)0x000ffffffffff000)

// CMCI has an LVT entry where the version register's maximum entry is this or
// more.
#define LVT_CMCI_MIN_MAX_ENTRY 6

#define IPI_TIMEOUT_NS ((uint64_t)100000000)

// RDMSR is an exit when the hypervisor itself runs nested.
static uint64_t apic_base(void) {
    uintptr_t base = this_cpu()->apic_base;
    return base != 0 ? base : rdmsr(MSR_IA32_APIC_BASE);
}

static volatile uint32_t *xapic_reg(uint32_t reg) {
    return (volatile uint32_t *)(uintptr_t)((apic_base() & APIC_BASE_ADDRESS)
        + reg);
}

uint32_t lapic_read(uint32_t reg) {
    if (lapic_x2apic()) {
        return (uint32_t)rdmsr(MSR_X2APIC_FIRST + (reg >> 4));
    }
    return *xapic_reg(reg);
}

void lapic_write(uint32_t reg, uint32_t value) {
    if (lapic_x2apic()) {
        wrmsr(MSR_X2APIC_FIRST + (reg >> 4), value);
        return;
    }
    *xapic_reg(reg) = value;
}

bool lapic_x2apic(void) {
    uintptr_t base = apic_base();
    return (base & (APIC_BASE_EN | APIC_BASE_EXTD))
        == (APIC_BASE_EN | APIC_BASE_EXTD);
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

bool lapic_x2apic_supported(void) {
    return (cpuid(1, 0).ecx & ((uint32_t)1 << 21)) != 0;
}

bool lapic_xapic_locked(void) {
    if (cpuid(0, 0).eax < 7 || (cpuid(7, 0).edx & ((uint32_t)1 << 29)) == 0) {
        return false;
    }
    uint64_t caps;
    if (!rdmsr_safe(MSR_IA32_ARCH_CAPABILITIES, &caps)
     || (caps & ARCH_CAPABILITIES_XAPIC_DISABLE_STATUS) == 0) {
        return false;
    }
    uint64_t status;
    return rdmsr_safe(MSR_IA32_XAPIC_DISABLE_STATUS, &status)
        && (status & LEGACY_XAPIC_DISABLED) != 0;
}

void lapic_send_ipi(uint32_t dest, uint32_t command) {
    if (lapic_x2apic()) {
        wrmsr(MSR_X2APIC_ICR, (uint64_t)dest << 32 | command);
        return;
    }
    uint64_t deadline = time_ns() + IPI_TIMEOUT_NS;
    while ((*xapic_reg(APIC_ICR_LOW) & ICR_PENDING) != 0) {
        if (time_ns() > deadline) {
            printf(
                "Local APIC %#x: an earlier IPI is still pending\n",
                lapic_cpu_apic_id()
            );
            break;
        }
        cpu_relax();
    }
    *xapic_reg(APIC_ICR_HIGH) = dest << 24;
    *xapic_reg(APIC_ICR_LOW) = command;
    while ((*xapic_reg(APIC_ICR_LOW) & ICR_PENDING) != 0) {
        if (time_ns() > deadline) {
            printf(
                "Local APIC %#x: IPI %#x not taken\n", lapic_cpu_apic_id(),
                command
            );
            return;
        }
        cpu_relax();
    }
}

void lapic_eoi(void) {
    lapic_write(APIC_EOI, 0);
}

static uint32_t max_entry(void) {
    return (lapic_read(APIC_VERSION) >> 16) & 0xff;
}

static bool host_mode(bool x2apic) {
    if (x2apic && !lapic_x2apic_supported()) {
        x2apic = false;
    }
    if (!x2apic && lapic_xapic_locked()) {
        x2apic = true;
    }
    uint64_t base = rdmsr(MSR_IA32_APIC_BASE);
    if ((base & APIC_BASE_EN) == 0) {
        base = (base | APIC_BASE_EN) & ~APIC_BASE_EXTD;
        wrmsr(MSR_IA32_APIC_BASE, base);
    }
    bool now = (base & APIC_BASE_EXTD) != 0;
    if (x2apic == now) {
        return x2apic;
    }
    if (x2apic) {
        wrmsr(MSR_IA32_APIC_BASE, base | APIC_BASE_EXTD);
        return true;
    }

    // x2APIC mode is only left for the disabled state, which resets the
    // registers: those the hypervisor set come back.
    static const uint32_t kept[] = {
        APIC_SVR, APIC_TPR, APIC_LVT_TIMER, APIC_LVT_THERMAL, APIC_LVT_PERF,
        APIC_LVT_LINT0, APIC_LVT_LINT1, APIC_LVT_ERROR, APIC_TIMER_DIVIDE,
        APIC_LVT_CMCI,
    };
    size_t count = sizeof(kept) / sizeof(uint32_t);
    if (max_entry() < LVT_CMCI_MIN_MAX_ENTRY) {
        count--;
    }
    uint32_t values[sizeof(kept) / sizeof(uint32_t)];
    for (size_t i = 0; i < count; i++) {
        values[i] = lapic_read(kept[i]);
    }
    wrmsr(MSR_IA32_APIC_BASE, base & ~(APIC_BASE_EN | APIC_BASE_EXTD));
    wrmsr(MSR_IA32_APIC_BASE, (base & ~APIC_BASE_EXTD) | APIC_BASE_EN);
    for (size_t i = 0; i < count; i++) {
        lapic_write(kept[i], values[i]);
    }
    return false;
}

bool lapic_host_mode(bool x2apic) {
    this_cpu()->apic_base = 0;
    x2apic = host_mode(x2apic);
    this_cpu()->apic_base = rdmsr(MSR_IA32_APIC_BASE);
    return x2apic;
}

void lapic_host_init(uint8_t spurious_vector, uint8_t timer_vector) {
    lapic_host_mode(false);
    lapic_write(APIC_TPR, 0);
    lapic_write(APIC_SVR, APIC_SVR_ENABLED | spurious_vector);
    lapic_write(APIC_LVT_LINT0, APIC_LVT_MASKED);
    lapic_write(APIC_TIMER_INITIAL, 0);
    lapic_write(APIC_LVT_TIMER, APIC_LVT_MASKED | timer_vector);
    // In-service interrupts end one EOI each, highest first.
    for (size_t n = 0; n < 256; n++) {
        bool in_service = false;
        for (size_t reg = APIC_ISR; reg < APIC_ISR + 0x80; reg += 0x10) {
            in_service |= lapic_read(reg) != 0;
        }
        if (!in_service) {
            break;
        }
        lapic_eoi();
    }
}
