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
#include <hw/lapic_legacy.h>
#include <core.h>
#include <hw/acpi.h>
#include <hw/lapic.h>
#include <lib/log.h>
#include <lib/printf.h>
#include <sys/x86.h>

// The vector a legacy BIOS's IRQ 7 handler serves, which copes with spurious
// interrupts.
#define SPURIOUS_VECTOR 0x0f

// Whether the firmware handed the boot processor's local APIC over in x2APIC
// mode.
static bool firmware_x2apic;

struct lapic_regs {
    uint32_t tpr;
    uint32_t svr;
    uint32_t lvt[LAPIC_LVT_COUNT];
};

struct nmi_pin {
    unsigned int lint;
    bool active_low;
    bool level;
};

static const uint32_t lvt_regs[LAPIC_LVT_COUNT] = {
    APIC_LVT_TIMER, APIC_LVT_THERMAL, APIC_LVT_PERF, APIC_LVT_LINT0,
    APIC_LVT_LINT1, APIC_LVT_ERROR, APIC_LVT_CMCI
};

static const char *const lvt_names[LAPIC_LVT_COUNT] = {
    "timer", "thermal", "perfmon", "LINT0", "LINT1", "error", "CMCI"
};

// The version register's maximum LVT entry from which each entry exists.
static const uint8_t lvt_first_max_entry[LAPIC_LVT_COUNT] = { 0, 5, 4, 0, 0, 0, 6 };

static uint32_t max_entry(void) {
    return (lapic_read(APIC_VERSION) >> 16) & 0xff;
}

static bool lvt_exists(size_t index, uint32_t max_entry) {
    return max_entry >= lvt_first_max_entry[index];
}

static void read_regs(uint32_t max, struct lapic_regs *regs) {
    regs->tpr = lapic_read(APIC_TPR);
    regs->svr = lapic_read(APIC_SVR);
    for (size_t i = 0; i < LAPIC_LVT_COUNT; i++) {
        regs->lvt[i] = lvt_exists(i, max) ? lapic_read(lvt_regs[i]) : 0;
    }
}

static void log_regs(
    uint32_t apic_id, const char *what, const struct lapic_regs *regs, uint32_t max_entry
) {
    char line[256];
    int len = snprintf(line, sizeof(line), "TPR %#x SVR %#x", regs->tpr, regs->svr);
    for (size_t i = 0; i < LAPIC_LVT_COUNT; i++) {
        if (len < 0 || (size_t)len >= sizeof(line)) {
            break;
        }
        if (lvt_exists(i, max_entry)) {
            len += snprintf(
                line + len, sizeof(line) - (size_t)len, " %s %#x", lvt_names[i], regs->lvt[i]
            );
        }
    }
    printf("Local APIC %#x: %s %s\n", apic_id, what, line);
}

static uint32_t apic_id_of(void) {
    if (cpuid(0, 0).eax >= 0xb) {
        struct cpuid_regs topology = cpuid(0xb, 0);
        if (topology.ebx != 0) {
            return topology.edx;
        }
    }
    uint32_t id = lapic_read(APIC_ID);
    return lapic_x2apic() ? id : id >> 24;
}

static bool madt_uid(const uint8_t *madt, size_t length, uint32_t apic_id, uint32_t *uid) {
    size_t offset = sizeof(struct acpi_madt);
    const struct acpi_entry_hdr *entry;
    while ((entry = acpi_madt_next(madt, length, &offset)) != NULL) {
        uint32_t id;
        uint32_t entry_uid;
        uint32_t flags;
        if (acpi_madt_processor(entry, &id, &entry_uid, &flags) && id == apic_id) {
            *uid = entry_uid;
            return true;
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
    size_t length = table.hdr->length;

    uint32_t uid = 0;
    bool have_uid = madt_uid(madt, length, apic_id, &uid);

    bool found = false;
    bool specific = false;
    size_t offset = sizeof(struct acpi_madt);
    const struct acpi_entry_hdr *entry;
    while (!specific && (entry = acpi_madt_next(madt, length, &offset)) != NULL) {
        uint32_t nmi_uid;
        bool all;
        uint8_t lint;
        uint16_t flags;
        if (!acpi_madt_lapic_nmi(entry, &nmi_uid, &all, &lint, &flags)) {
            continue;
        }
        bool mine = have_uid && nmi_uid == uid;

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
    firmware_x2apic = (found_base & APIC_BASE_EXTD) != 0;
    bool x2apic = firmware_x2apic && lapic_xapic_locked();
    uintptr_t registers = found_base & APIC_BASE_ADDRESS;
    if (!x2apic && registers + LAPIC_MMIO_SIZE > core_handoff->map_top) {
        printf("Local APIC registers at %#llx are out of reach\n", (unsigned long long)registers);
        return;
    }

    // Read through the interface the mode found offers, which hw/lapic.c's
    // accessors follow.
    bool found_enabled = (found_base & APIC_BASE_EN) != 0;
    uint32_t found_max_entry = 0;
    struct lapic_regs found = { 0 };
    if (found_enabled) {
        found_max_entry = max_entry();
        read_regs(found_max_entry, &found);
    }

    uintptr_t base = found_base;
    if ((base & APIC_BASE_EXTD) != 0 && !x2apic) {
        // x2APIC mode can only be left for the disabled state.
        base &= ~(APIC_BASE_EN | APIC_BASE_EXTD);
        wrmsr(MSR_IA32_APIC_BASE, base);
    }
    if ((base & APIC_BASE_EN) == 0) {
        base |= APIC_BASE_EN;
        wrmsr(MSR_IA32_APIC_BASE, base);
    }

    uint32_t version = lapic_read(APIC_VERSION);
    uint32_t max_entry = (version >> 16) & 0xff;
    uint32_t apic_id = apic_id_of();
    printf(
        "Local APIC %#x: IA32_APIC_BASE %#llx -> %#llx, %s, version %#x\n", apic_id,
        (unsigned long long)found_base, (unsigned long long)rdmsr(MSR_IA32_APIC_BASE),
        x2apic ? "x2APIC mode locked" : "xAPIC mode", version
    );
    if (found_enabled) {
        log_regs(apic_id, "found", &found, found_max_entry);
    } else {
        printf("Local APIC %#x: found disabled\n", apic_id);
    }

    struct nmi_pin nmi = { .lint = 1, .active_low = false, .level = false };
    bool from_madt = madt_nmi_pin(apic_id, &nmi);
    // LINT1 cannot be level-triggered (SDM 325462-093 Vol. 3A, 13.5.1).
    if (nmi.lint == 1) {
        nmi.level = false;
    }

    lapic_write(APIC_TPR, 0);
    for (size_t i = 0; i < LAPIC_LVT_COUNT; i++) {
        if (lvt_regs[i] == APIC_LVT_LINT0 || lvt_regs[i] == APIC_LVT_LINT1
         || !lvt_exists(i, max_entry)) {
            continue;
        }
        // Other delivery modes, such as SMI for thermal events, are the
        // firmware's.
        uint32_t value = lapic_read(lvt_regs[i]);
        uint32_t mode = value & APIC_LVT_DELIVERY_MASK;
        bool interrupt = mode == APIC_LVT_DELIVERY_FIXED || mode == ICR_DELIVERY_LOWEST
                      || mode == APIC_LVT_DELIVERY_NMI || mode == APIC_LVT_DELIVERY_EXTINT;
        if (interrupt && (value & APIC_LVT_MASKED) == 0) {
            uint32_t masked = (value & ~(APIC_LVT_DELIVERY_STATUS | APIC_LVT_REMOTE_IRR))
                            | APIC_LVT_MASKED;
            lapic_write(lvt_regs[i], masked);
        }
    }

    // Before the LINT entries, as a software disabled APIC keeps them masked.
    lapic_write(APIC_SVR, APIC_SVR_ENABLED | SPURIOUS_VECTOR);

    uint32_t nmi_lvt = APIC_LVT_DELIVERY_NMI | (nmi.active_low ? APIC_LVT_ACTIVE_LOW : 0)
                     | (nmi.level ? APIC_LVT_LEVEL : 0);
    uint32_t extint_lvt = APIC_LVT_DELIVERY_EXTINT;
    lapic_write(nmi.lint == 0 ? APIC_LVT_LINT0 : APIC_LVT_LINT1, nmi_lvt);
    lapic_write(nmi.lint == 0 ? APIC_LVT_LINT1 : APIC_LVT_LINT0, extint_lvt);

    struct lapic_regs set;
    read_regs(max_entry, &set);
    log_regs(apic_id, "set", &set, max_entry);
    printf(
        "Local APIC %#x: NMIs on LINT%u, active %s, %s triggered, %s\n", apic_id, nmi.lint,
        nmi.active_low ? "low" : "high", nmi.level ? "level" : "edge",
        from_madt ? "from the MADT" : "by default"
    );
}

bool lapic_legacy_firmware_x2apic(void) {
    return firmware_x2apic;
}
