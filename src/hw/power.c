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
#include <hw/power.h>
#include <core.h>
#include <hw/acpi.h>
#include <hw/i8042.h>
#include <hw/pci_cfg.h>
#include <lib/log.h>
#include <lib/time.h>
#include <sys/x86.h>

#define RESET_WAIT_US 100000

// What Linux 7.3 writes to the reset control register: a hard reset, and for a
// cold one, the power cycled.
#define RST_CNT_WARM (RST_CNT_SYS_RST | RST_CNT_RST_CPU)
#define RST_CNT_COLD (RST_CNT_SYS_RST | RST_CNT_RST_CPU | RST_CNT_FULL_RST)
#define RST_CNT_SETUP_US 50

// Returns only if the firmware fails to.
static void firmware_reset(EFI_RESET_TYPE type) {
    EFI_RUNTIME_SERVICES *rt = (EFI_RUNTIME_SERVICES *)(uintptr_t)core_handoff->efi_rt;
    rt->ResetSystem(type, EFI_SUCCESS, 0, NULL);
}

// ACPI's reset register, then the keyboard controller's reset line where there
// is a controller, then the reset control register at CF9h where the
// configuration ports beside it answer, as Linux 7.3 restarts. The firmware's
// service comes after them, as a Dell Inspiron 7700's jumps to address 2 by
// now.
void power_reset(bool warm) {
    printf("Resetting the machine\n");
    log_keep();
    if (acpi_init()) {
        uacpi_reboot();
        time_udelay(RESET_WAIT_US);
    }
    if (i8042_present()) {
        for (size_t i = 0; i < 100000; i++) {
            if ((inb(0x64) & 2) == 0) {
                break;
            }
            cpu_relax();
        }
        outb(0x64, 0xfe);
        time_udelay(RESET_WAIT_US);
    }
    if (pci_cfg_ports_answer()) {
        uint8_t code = warm ? RST_CNT_WARM : RST_CNT_COLD;
        uint8_t other = (uint8_t)(inb(RST_CNT) & ~code);
        outb(RST_CNT, (uint8_t)(other | RST_CNT_SYS_RST));
        time_udelay(RST_CNT_SETUP_US);
        outb(RST_CNT, (uint8_t)(other | code));
        time_udelay(RESET_WAIT_US);
    }
    firmware_reset(warm ? EfiResetWarm : EfiResetCold);
    struct {
        uint16_t limit;
        uint64_t base;
    } __attribute__((packed)) null_idt = { 0, 0 };
    asm volatile (
        "lidtq %0\n\t"
        "int3"
        :
        : "m"(null_idt)
    );
    x86_halt_forever();
}

// ACPI's S5, as Linux 7.3 powers off. The firmware's service comes after it,
// for the reason it does in power_reset().
void power_off(void) {
    printf("Powering the machine off\n");
    log_keep();
    if (acpi_init()) {
        uacpi_status status = uacpi_prepare_for_sleep_state(UACPI_SLEEP_STATE_S5);
        if (status == UACPI_STATUS_OK) {
            status = uacpi_enter_sleep_state(UACPI_SLEEP_STATE_S5);
        }
        printf("Cannot enter ACPI S5: %s\n", uacpi_status_to_string(status));
    }
    firmware_reset(EfiResetShutdown);
    panic("Cannot power the machine off\n");
}
