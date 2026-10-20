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
#include <stddef.h>
#include <stdbool.h>
#include <uacpi/acpi.h>
#include <uacpi/tables.h>
#include <memory.h>
#include <core.h>
#include <hw/acpi.h>
#include <hw/lapic_legacy.h>
#include <lib/log.h>
#include <lib/printf.h>
#include <sys/fixup.h>
#include <sys/x86.h>

#define MSR_IA32_XAPIC_DISABLE_STATUS 0xbd
#define MSR_IA32_ARCH_CAPABILITIES 0x10a
#define MSR_X2APIC_FIRST 0x800

#define ARCH_CAPABILITIES_XAPIC_DISABLE_STATUS (1ull << 21)
#define LEGACY_XAPIC_DISABLED (1ull << 0)

#define APIC_BASE_EXTD (1ull << 10)
#define APIC_BASE_EN (1ull << 11)
#define APIC_BASE_ADDRESS 0x000ffffffffff000ull

#define APIC_ID 0x20
#define APIC_VERSION 0x30
#define APIC_TPR 0x80
#define APIC_SVR 0xf0
#define APIC_LVT_CMCI 0x2f0
#define APIC_LVT_TIMER 0x320
#define APIC_LVT_THERMAL 0x330
#define APIC_LVT_PERF 0x340
#define APIC_LVT_LINT0 0x350
#define APIC_LVT_LINT1 0x360
#define APIC_LVT_ERROR 0x370

#define LVT_DELIVERY_SHIFT 8
#define LVT_DELIVERY_FIXED 0
#define LVT_DELIVERY_LOWEST 1
#define LVT_DELIVERY_NMI 4
#define LVT_DELIVERY_EXTINT 7
#define LVT_DELIVERY_STATUS (1u << 12)
#define LVT_ACTIVE_LOW (1u << 13)
#define LVT_REMOTE_IRR (1u << 14)
#define LVT_LEVEL (1u << 15)
#define LVT_MASKED (1u << 16)

#define SVR_APIC_ENABLED (1u << 8)
// The vector a legacy BIOS's IRQ 7 handler serves, which copes with spurious
// interrupts.
#define SPURIOUS_VECTOR 0x0f

#define LVT_COUNT 7

struct lapic {
    bool x2apic;
    uint64_t base;
};

struct lapic_regs {
    uint32_t tpr;
    uint32_t svr;
    uint32_t lvt[LVT_COUNT];
};

struct nmi_pin {
    unsigned lint;
    bool active_low;
    bool level;
};

static const uint32_t lvt_regs[LVT_COUNT] = {
    APIC_LVT_TIMER, APIC_LVT_THERMAL, APIC_LVT_PERF, APIC_LVT_LINT0, APIC_LVT_LINT1, APIC_LVT_ERROR,
    APIC_LVT_CMCI,
};

static const char *const lvt_names[LVT_COUNT] = {
    "timer", "thermal", "perfmon", "LINT0", "LINT1", "error", "CMCI",
};

// The version register's maximum LVT entry from which each entry exists.
static const uint8_t lvt_first_max_entry[LVT_COUNT] = {0, 5, 4, 0, 0, 0, 6};

static uint32_t lapic_read(const struct lapic *lapic, uint32_t reg) {
    if (lapic->x2apic) {
        return (uint32_t)rdmsr(MSR_X2APIC_FIRST + reg / 16);
    }
    return *(volatile uint32_t *)(uintptr_t)(lapic->base + reg);
}

static void lapic_write(const struct lapic *lapic, uint32_t reg, uint32_t value) {
    if (lapic->x2apic) {
        wrmsr(MSR_X2APIC_FIRST + reg / 16, value);
        return;
    }
    *(volatile uint32_t *)(uintptr_t)(lapic->base + reg) = value;
}

static uint32_t lapic_max_entry(const struct lapic *lapic) {
    return (lapic_read(lapic, APIC_VERSION) >> 16) & 0xff;
}

static bool lvt_exists(unsigned index, uint32_t max_entry) {
    return max_entry >= lvt_first_max_entry[index];
}

static void lapic_read_regs(const struct lapic *lapic, uint32_t max_entry,
                            struct lapic_regs *regs) {
    regs->tpr = lapic_read(lapic, APIC_TPR);
    regs->svr = lapic_read(lapic, APIC_SVR);
    for (unsigned i = 0; i < LVT_COUNT; i++) {
        regs->lvt[i] = lvt_exists(i, max_entry) ? lapic_read(lapic, lvt_regs[i]) : 0;
    }
}

static void log_regs(uint32_t apic_id, const char *what, const struct lapic_regs *regs,
                     uint32_t max_entry) {
    char line[256];
    int len = snprintf(line, sizeof(line), "TPR %#x SVR %#x", regs->tpr, regs->svr);
    for (unsigned i = 0; i < LVT_COUNT; i++) {
        if (len < 0 || (size_t)len >= sizeof(line)) {
            break;
        }
        if (lvt_exists(i, max_entry)) {
            len += snprintf(line + len, sizeof(line) - (size_t)len, " %s %#x", lvt_names[i],
                            regs->lvt[i]);
        }
    }
    printf("Local APIC %#x: %s %s\n", apic_id, what, line);
}

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

static uint32_t apic_id_of(const struct lapic *lapic) {
    if (cpuid(0, 0).eax >= 0xb) {
        struct cpuid_regs topology = cpuid(0xb, 0);
        if (topology.ebx != 0) {
            return topology.edx;
        }
    }
    uint32_t id = lapic_read(lapic, APIC_ID);
    return lapic->x2apic ? id : id >> 24;
}

// Advances *offset past the MADT entry it is at, which it returns, or returns
// NULL at the end of the table.
static const uint8_t *madt_next(const uint8_t *madt, uint32_t length, uint32_t *offset) {
    if (*offset > length || length - *offset < 2) {
        return NULL;
    }
    const uint8_t *entry = madt + *offset;
    if (entry[1] < 2 || entry[1] > length - *offset) {
        return NULL;
    }
    *offset += entry[1];
    return entry;
}

static bool madt_uid(const uint8_t *madt, uint32_t length, uint32_t apic_id, uint32_t *uid) {
    uint32_t offset = sizeof(struct acpi_madt);
    const uint8_t *entry;
    while ((entry = madt_next(madt, length, &offset)) != NULL) {
        if (entry[0] == ACPI_MADT_ENTRY_TYPE_LAPIC && entry[1] >= sizeof(struct acpi_madt_lapic)) {
            struct acpi_madt_lapic lapic;
            memcpy(&lapic, entry, sizeof(lapic));
            if (lapic.id == apic_id) {
                *uid = lapic.uid;
                return true;
            }
        } else if (entry[0] == ACPI_MADT_ENTRY_TYPE_LOCAL_X2APIC
                   && entry[1] >= sizeof(struct acpi_madt_x2apic)) {
            struct acpi_madt_x2apic x2apic;
            memcpy(&x2apic, entry, sizeof(x2apic));
            if (x2apic.id == apic_id) {
                *uid = x2apic.uid;
                return true;
            }
        }
    }
    return false;
}

// The MADT's NMI structures that apply to the processor with the APIC ID, one
// for its ACPI processor UID taking precedence over one for all processors.
static bool madt_nmi_pin(uint32_t apic_id, struct nmi_pin *pin) {
    if (!acpi_tables_ready()) {
        return false;
    }
    struct uacpi_table table;
    if (uacpi_table_find_by_signature(ACPI_MADT_SIGNATURE, &table) != UACPI_STATUS_OK) {
        return false;
    }
    const uint8_t *madt = table.ptr;
    uint32_t length = table.hdr->length;

    uint32_t uid = 0;
    bool have_uid = madt_uid(madt, length, apic_id, &uid);

    bool found = false;
    bool specific = false;
    uint32_t offset = sizeof(struct acpi_madt);
    const uint8_t *entry;
    while (!specific && (entry = madt_next(madt, length, &offset)) != NULL) {
        bool all;
        bool mine;
        uint16_t flags;
        uint8_t lint;
        if (entry[0] == ACPI_MADT_ENTRY_TYPE_LAPIC_NMI
            && entry[1] >= sizeof(struct acpi_madt_lapic_nmi)) {
            struct acpi_madt_lapic_nmi nmi;
            memcpy(&nmi, entry, sizeof(nmi));
            all = nmi.uid == 0xff;
            mine = have_uid && nmi.uid == uid;
            flags = nmi.flags;
            lint = nmi.lint;
        } else if (entry[0] == ACPI_MADT_ENTRY_TYPE_LOCAL_X2APIC_NMI
                   && entry[1] >= sizeof(struct acpi_madt_x2apic_nmi)) {
            struct acpi_madt_x2apic_nmi nmi;
            memcpy(&nmi, entry, sizeof(nmi));
            all = nmi.uid == 0xffffffff;
            mine = have_uid && nmi.uid == uid;
            flags = nmi.flags;
            lint = nmi.lint;
        } else {
            continue;
        }

        if (lint > 1 || !(mine || (all && !found))) {
            continue;
        }
        pin->lint = lint;
        pin->active_low = (flags & ACPI_MADT_POLARITY_MASK) == ACPI_MADT_POLARITY_ACTIVE_LOW;
        pin->level = (flags & ACPI_MADT_TRIGGERING_MASK) == ACPI_MADT_TRIGGERING_LEVEL;
        found = true;
        specific = mine;
    }

    uacpi_table_unref(&table);
    return found;
}

void lapic_legacy_init(void) {
    uint64_t found_base = rdmsr(MSR_IA32_APIC_BASE);
    struct lapic lapic = {
        .x2apic = (found_base & APIC_BASE_EXTD) && xapic_disabled(),
        .base = found_base & APIC_BASE_ADDRESS,
    };
    if (!lapic.x2apic && lapic.base + 0x1000 > core_handoff->map_top) {
        printf("Local APIC registers at %#llx are out of reach\n", (unsigned long long)lapic.base);
        return;
    }

    // Read through the interface the mode found offers.
    struct lapic found_lapic = {
        .x2apic = (found_base & APIC_BASE_EXTD) != 0,
        .base = lapic.base,
    };
    bool found_enabled = (found_base & APIC_BASE_EN) != 0;
    uint32_t found_max_entry = 0;
    struct lapic_regs found = {0};
    if (found_enabled) {
        found_max_entry = lapic_max_entry(&found_lapic);
        lapic_read_regs(&found_lapic, found_max_entry, &found);
    }

    uint64_t base = found_base;
    if ((base & APIC_BASE_EXTD) && !lapic.x2apic) {
        // x2APIC mode can only be left for the disabled state.
        base &= ~(APIC_BASE_EN | APIC_BASE_EXTD);
        wrmsr(MSR_IA32_APIC_BASE, base);
    }
    if (!(base & APIC_BASE_EN)) {
        base |= APIC_BASE_EN;
        wrmsr(MSR_IA32_APIC_BASE, base);
    }

    uint32_t version = lapic_read(&lapic, APIC_VERSION);
    uint32_t max_entry = (version >> 16) & 0xff;
    uint32_t apic_id = apic_id_of(&lapic);
    printf("Local APIC %#x: IA32_APIC_BASE %#llx -> %#llx, %s, version %#x\n", apic_id,
           (unsigned long long)found_base, (unsigned long long)rdmsr(MSR_IA32_APIC_BASE),
           lapic.x2apic ? "x2APIC mode locked" : "xAPIC mode", version);
    if (found_enabled) {
        log_regs(apic_id, "found", &found, found_max_entry);
    } else {
        printf("Local APIC %#x: found disabled\n", apic_id);
    }

    struct nmi_pin nmi = {.lint = 1, .active_low = false, .level = false};
    bool from_madt = madt_nmi_pin(apic_id, &nmi);
    // LINT1 cannot be level-triggered (SDM 325462-093 Vol. 3A, 13.5.1).
    if (nmi.lint == 1) {
        nmi.level = false;
    }

    lapic_write(&lapic, APIC_TPR, 0);
    for (unsigned i = 0; i < LVT_COUNT; i++) {
        if (lvt_regs[i] == APIC_LVT_LINT0 || lvt_regs[i] == APIC_LVT_LINT1
            || !lvt_exists(i, max_entry)) {
            continue;
        }
        // Other delivery modes, such as SMI for thermal events, are the
        // firmware's.
        uint32_t value = lapic_read(&lapic, lvt_regs[i]);
        unsigned mode = (value >> LVT_DELIVERY_SHIFT) & 7;
        bool interrupt = mode == LVT_DELIVERY_FIXED || mode == LVT_DELIVERY_LOWEST
                         || mode == LVT_DELIVERY_NMI || mode == LVT_DELIVERY_EXTINT;
        if (interrupt && !(value & LVT_MASKED)) {
            lapic_write(&lapic, lvt_regs[i],
                        (value & ~(LVT_DELIVERY_STATUS | LVT_REMOTE_IRR)) | LVT_MASKED);
        }
    }

    // Before the LINT entries, as a software disabled APIC keeps them masked.
    lapic_write(&lapic, APIC_SVR, SVR_APIC_ENABLED | SPURIOUS_VECTOR);

    uint32_t nmi_lvt = (LVT_DELIVERY_NMI << LVT_DELIVERY_SHIFT)
                       | (nmi.active_low ? LVT_ACTIVE_LOW : 0) | (nmi.level ? LVT_LEVEL : 0);
    uint32_t extint_lvt = LVT_DELIVERY_EXTINT << LVT_DELIVERY_SHIFT;
    lapic_write(&lapic, nmi.lint == 0 ? APIC_LVT_LINT0 : APIC_LVT_LINT1, nmi_lvt);
    lapic_write(&lapic, nmi.lint == 0 ? APIC_LVT_LINT1 : APIC_LVT_LINT0, extint_lvt);

    struct lapic_regs set;
    lapic_read_regs(&lapic, max_entry, &set);
    log_regs(apic_id, "set", &set, max_entry);
    printf("Local APIC %#x: NMIs on LINT%u, active %s, %s triggered, %s\n", apic_id, nmi.lint,
           nmi.active_low ? "low" : "high", nmi.level ? "level" : "edge",
           from_madt ? "from the MADT" : "by default");
}
