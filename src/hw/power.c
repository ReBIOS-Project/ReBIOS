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
#include <efi.h>
#include <uacpi/sleep.h>
#include <uacpi/status.h>
#include <uacpi/uacpi.h>
#include <hw/power.h>
#include <core.h>
#include <hw/acpi.h>
#include <lib/log.h>
#include <lib/time.h>
#include <sys/x86.h>

#define RESET_WAIT_US 100000

// Returns only if the firmware fails to.
static void firmware_reset(EFI_RESET_TYPE type) {
    EFI_RUNTIME_SERVICES *rt
        = (EFI_RUNTIME_SERVICES *)(uintptr_t)core_handoff->efi_rt;
    rt->ResetSystem(type, EFI_SUCCESS, 0, NULL);
}

// ACPI's reset register, then the keyboard controller's reset line, as Linux
// 7.3 restarts, then the reset control register at CF9h. The firmware's
// service comes after them, as a Dell Inspiron 7700's jumps to address 2 by
// now.
void power_reset(bool warm) {
    printf("Resetting the machine\n");
    if (acpi_init()) {
        uacpi_reboot();
        time_udelay(RESET_WAIT_US);
    }
    if (inb(0x64) != 0xff) {
        for (size_t i = 0; i < 100000; i++) {
            if ((inb(0x64) & 2) == 0) {
                break;
            }
            cpu_relax();
        }
        outb(0x64, 0xfe);
        time_udelay(RESET_WAIT_US);
    }
    outb(0xcf9, 0x02);
    outb(0xcf9, 0x06);
    time_udelay(RESET_WAIT_US);
    firmware_reset(warm ? EfiResetWarm : EfiResetCold);
    struct {
        uint16_t limit;
        uint64_t base;
    } __attribute__((packed)) null_idt = { 0, 0 };
    asm volatile ("lidt %0\n\tint3" :: "m"(null_idt));
    halt_forever();
}

void power_off(void) {
    printf("Powering the machine off\n");
    firmware_reset(EfiResetShutdown);
    if (acpi_init()) {
        // Sleep states need the namespace initialised, which runs the _INI
        // methods otherwise left to the guest, which is done with by now.
        uacpi_status status = uacpi_namespace_initialize();
        if (status == UACPI_STATUS_OK) {
            status = uacpi_prepare_for_sleep_state(UACPI_SLEEP_STATE_S5);
        }
        if (status == UACPI_STATUS_OK) {
            status = uacpi_enter_sleep_state(UACPI_SLEEP_STATE_S5);
        }
        printf("Cannot enter ACPI S5: %s\n", uacpi_status_to_string(status));
    }
    panic("Cannot power the machine off\n");
}
