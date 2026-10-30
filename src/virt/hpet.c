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
#include <uacpi/platform/types.h>
#include <uacpi/status.h>
#include <uacpi/tables.h>
#include <virt/hpet.h>
#include <lib/log.h>
#include <memory.h>
#include <sys/mm.h>
#include <virt/layout.h>

// IA-PC HPET Specification 1.0a, 2.3: the capabilities and configuration
// registers, and each timer's configuration register.
#define HPET_SIZE ((uint32_t)0x1000)
#define CAP_TIMERS_SHIFT 8
#define CAP_TIMERS_MASK ((uint32_t)0x1f)
#define CONFIG ((uint32_t)0x10)
#define CONFIG_LEGACY_ROUTE ((uint32_t)1 << 1)
#define TIMER_BASE ((uint32_t)0x100)
#define TIMER_STRIDE ((uint32_t)0x20)
#define TIMER_INT_ENABLE ((uint32_t)1 << 2)
#define TIMER_FSB_ENABLE ((uint32_t)1 << 14)
#define TIMER_FSB_CAP ((uint32_t)1 << 15)
#define MAX_HIDDEN 8

static uint64_t hidden[MAX_HIDDEN];
static size_t hidden_count;

static uint32_t read32(uintptr_t address) {
    return *(volatile uint32_t *)(uintptr_t)address;
}

static void write32(uintptr_t address, uint32_t value) {
    *(volatile uint32_t *)(uintptr_t)address = value;
}

static unsigned int timers(uintptr_t base) {
    return ((read32(base) >> CAP_TIMERS_SHIFT) & CAP_TIMERS_MASK) + 1;
}

static bool fsb_capable(uintptr_t base) {
    for (size_t n = 0; n < timers(base); n++) {
        if ((read32(base + TIMER_BASE + n * TIMER_STRIDE) & TIMER_FSB_CAP)
         != 0) {
            return true;
        }
    }
    return false;
}

// Its timers interrupt no more, and the PIT and the RTC interrupt as without
// it; the counter runs on, as firmware may time its own delays with it.
static void hide(uintptr_t base) {
    for (size_t n = 0; n < timers(base); n++) {
        uintptr_t config = base + TIMER_BASE + n * TIMER_STRIDE;
        write32(
            config, read32(config) & ~(TIMER_INT_ENABLE | TIMER_FSB_ENABLE)
        );
    }
    write32(base + CONFIG, read32(base + CONFIG) & ~CONFIG_LEGACY_ROUTE);
    layout_absent(base, HPET_SIZE);
    hidden[hidden_count++] = base;
    printf(
        "HPET at %#llx hidden from the guest, as its timers can deliver "
        "through the FSB\n",
        (unsigned long long)base
    );
}

void hpet_init(void) {
    struct uacpi_table table;
    for (size_t n = 0;
         uacpi_table_find_nth_by_signature(ACPI_HPET_SIGNATURE, n, &table)
      == UACPI_STATUS_OK; n++) {
        if (table.hdr->length >= sizeof(struct acpi_hpet)) {
            struct acpi_hpet hpet;
            memcpy(&hpet, table.ptr, sizeof(struct acpi_hpet));
            uintptr_t base = hpet.address.address;
            if (hpet.address.address_space_id == ACPI_AS_ID_SYS_MEM
             && (base & (PAGE_SIZE - 1)) == 0 && hidden_count < MAX_HIDDEN
             && fsb_capable(base)) {
                hide(base);
            }
        }
        uacpi_table_unref(&table);
    }
}

bool hpet_hidden(uintptr_t base) {
    for (size_t i = 0; i < hidden_count; i++) {
        if (hidden[i] == base) {
            return true;
        }
    }
    return false;
}

bool hpet_any_hidden(void) {
    return hidden_count != 0;
}
