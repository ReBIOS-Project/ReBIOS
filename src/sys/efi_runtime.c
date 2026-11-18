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
// You should have received a copy of the GNU General Public License along with
// ReBIOS. If not, see <https://www.gnu.org/licenses/>.

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <efi.h>
#include <sys/efi_runtime.h>
#include <core.h>
#include <lib/log.h>
#include <sys/cpu.h>

// Whether a page fault in the services ended a call to them, which leaves them
// unusable, as Linux 7.3 takes them to be (quirks.c,
// efi_crash_gracefully_on_page_fault()).
static _Atomic(bool) broken;

// The core never asks for virtual addresses (UEFI 2.11, 8.4.1), so the
// services run at the physical addresses that its identity map gives them.
static struct efi_runtime_services *services(void) {
    return (struct efi_runtime_services *)(uintptr_t)core_handoff->efi_rt;
}

// Runs call(ctx), which calls a service, and ends it with EFI_ABORTED if the
// firmware's code takes a page fault (sys/cpu.c, firmware_fault()).
static efi_status guarded(efi_status (*call)(void *ctx), void *ctx) {
    struct cpu_resume resume;
    if (cpu_resume_save(&resume) != 0) {
        atomic_store_explicit(&broken, true, memory_order_release);
        // A panic's processor may hold the log's lock.
        if (!log_panicked()) {
            printf(
                "The firmware's runtime services took a page fault at %#llx, and are not called "
                "again\n",
                (unsigned long long)this_cpu()->firmware_fault
            );
        }
        return EFI_ABORTED;
    }
    this_cpu()->firmware_resume = &resume;
    efi_status status = call(ctx);
    this_cpu()->firmware_resume = NULL;
    return status;
}

static efi_status reset_system(void *ctx) {
    services()->reset_system(*(const uint32_t *)ctx, EFI_SUCCESS, 0, NULL);
    return EFI_DEVICE_ERROR;
}

// With the guest gone, its registers are not kept around the call.
void efi_runtime_reset_system(uint32_t type) {
    if (services() != NULL && !atomic_load_explicit(&broken, memory_order_acquire)) {
        guarded(reset_system, &type);
    }
}
