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
#include <uacpi/types.h>
#include <hw/pm_timer.h>
#include <hw/acpi.h>
#include <lib/log.h>
#include <lib/time.h>
#include <sys/x86.h>

#define MASK_24 ((uint32_t)0xffffff)
// The FADT's PM_TMR_LEN where the timer exists (ACPI 6.6, 5.2.9).
#define PM_TIMER_LEN 4

// Linux 7.3's check: rounds after ever longer delays, in each of which the
// count must move within so many reads, and only up, but for a wrap, which
// leaves it below WRAPPED_BELOW.
#define CHECK_ROUNDS 10
#define CHECK_READS 10000
#define WRAPPED_BELOW 0xfff

static uint16_t port;
static uint32_t mask;
static bool sound;

uint32_t pm_timer_read(void) {
    return inl(port) & mask;
}

static bool counts(void) {
    for (size_t round = 0; round < CHECK_ROUNDS; round++) {
        time_udelay(100 * round);
        uint32_t first = pm_timer_read();
        uint32_t now = first;
        for (size_t i = 0; i < CHECK_READS && now == first; i++) {
            now = pm_timer_read();
        }
        if (now == first) {
            printf("The PM timer at %#x stands still at %#x\n", port, first);
            return false;
        }
        if (now < first && now >= WRAPPED_BELOW) {
            printf("The PM timer at %#x went back from %#x to %#x\n", port, first, now);
            return false;
        }
    }
    return true;
}

void pm_timer_init(void) {
    struct acpi_fadt *fadt;
    if (!acpi_tables_ready() || uacpi_table_fadt(&fadt) != UACPI_STATUS_OK) {
        return;
    }
    // A machine without the timer has a PM_TMR_LEN of 0 (ACPI 6.6, 5.2.9), and
    // below 100h a 32-bit read reaches fixed motherboard ports, such as the
    // 8042's data port, whose byte it would take.
    const struct acpi_gas *gas = &fadt->x_pm_tmr_blk;
    if (fadt->pm_tmr_len != PM_TIMER_LEN || gas->address < 0x100
     || gas->address_space_id != UACPI_ADDRESS_SPACE_SYSTEM_IO || gas->address > 0xfffc) {
        return;
    }
    port = (uint16_t)gas->address;
    mask = (fadt->flags & ACPI_TMR_VAL_EXT) != 0 ? UINT32_MAX : MASK_24;
    sound = counts();
}

uint16_t pm_timer_port(void) {
    return port;
}

uint32_t pm_timer_mask(void) {
    return mask;
}

bool pm_timer_sound(void) {
    return sound;
}

void pm_timer_reject(void) {
    sound = false;
    printf("The PM timer at %#x does not count at its rate\n", port);
}
