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
#include <virt/log_save.h>
#include <core.h>
#include <handoff.h>
#include <lib/heap.h>
#include <lib/log.h>
#include <lib/time.h>
#include <lib/util.h>
#include <memory.h>
#include <sys/efi_runtime.h>
#include <sys/mm.h>
#include <sys/x86.h>
#include <virt/hv_timer.h>

// Each save is a write of the firmware's flash, and may hold the boot
// processor for tens of milliseconds, so there is one at most every 10
// seconds, and 360 at most a boot, but for the last before a panic, a reset,
// or the power going off. Firmware may collect the garbage of its variable
// store only before ExitBootServices(), as EDK2's does (edk2-stable202608,
// MdeModulePkg/Universal/Variable/RuntimeDxe/Variable.c), so each save takes
// new room: the log's start goes once to one variable, and only its latest
// text, END_MAX bytes at most, to the other, as often as it changes.
#define SAVE_INTERVAL_S 10
#define SAVE_LIMIT 360
#define END_MAX 8192
#define SAVE_MIN 1024
// Linux 7.3 keeps at least this much of the variable store free, a figure from
// Samsung, some of whose machines stop starting with less
// (arch/x86/platform/efi/quirks.c, efi_query_variable_store()).
#define STORE_RESERVE 5120
#define ATTRIBUTES \
    (EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS)

static uint16_t start_name[] = HANDOFF_LOG_VARIABLE;
static uint16_t end_name[] = HANDOFF_LOG_END_VARIABLE;
static uint16_t dummy_name[] = u"ReBIOSDummy";
static struct efi_guid guid = HANDOFF_VARIABLE_GUID;
static const char gap[] = "[...]\n";
static struct hv_timer timer;
static char *text;
static bool start_saved;
static size_t start_len;
static size_t saved_count;
static size_t saves;

// How many bytes, want at most, a variable named name_size bytes may hold with
// kept bytes of the store left for later, as Linux 7.3's
// efi_query_variable_store() decides: where too little is left, a write too
// large to fit, which fails, may have the firmware collect its garbage first,
// but not without wait, for panics. 0 where the write would leave less than
// STORE_RESERVE free.
static size_t room(size_t name_size, size_t want, size_t kept, bool wait) {
    uint64_t storage, remaining, max_size;
    efi_status status = efi_runtime_query_variable_info(
        ATTRIBUTES, &storage, &remaining, &max_size, wait
    );
    if (status != EFI_SUCCESS || max_size <= name_size) {
        return 0;
    }
    if (wait && remaining < want + kept + STORE_RESERVE) {
        size_t dummy_size = (size_t)remaining + 1024;
        void *dummy = kmalloc(dummy_size);
        if (dummy != NULL) {
            memset(dummy, 0, dummy_size);
            status = efi_runtime_set_variable(
                dummy_name, &guid, ATTRIBUTES, dummy_size, dummy, true
            );
            if (status == EFI_SUCCESS) {
                efi_runtime_set_variable(dummy_name, &guid, ATTRIBUTES, 0, NULL, true);
            }
            kfree(dummy);
        }
        status = efi_runtime_query_variable_info(
            ATTRIBUTES, &storage, &remaining, &max_size, true
        );
        if (status != EFI_SUCCESS || max_size <= name_size) {
            return 0;
        }
    }
    if (remaining <= STORE_RESERVE + kept) {
        return 0;
    }
    uint64_t size = remaining - STORE_RESERVE - kept;
    if (size > max_size - name_size) {
        size = max_size - name_size;
    }
    return size < want ? (size_t)size : want;
}

// Room for the last save is kept from the others.
static efi_status save(bool wait, bool last) {
    size_t count = log_saved_count();
    size_t kept = last ? 0 : END_MAX;
    efi_status status;
    if (!start_saved) {
        size_t size = room(sizeof(start_name), LOG_SAVED_START, kept, wait);
        if (size < SAVE_MIN) {
            return EFI_OUT_OF_RESOURCES;
        }
        size_t len = log_saved_start(text, size, wait);
        status = efi_runtime_set_variable(start_name, &guid, ATTRIBUTES, len, text, wait);
        if (status != EFI_SUCCESS) {
            return status;
        }
        start_saved = true;
        start_len = len;
    }
    if (count > start_len) {
        size_t size = room(sizeof(end_name), END_MAX, kept, wait);
        if (size < SAVE_MIN) {
            return EFI_OUT_OF_RESOURCES;
        }
        // Marked where text between the two is lost.
        size_t gap_len = sizeof(gap) - 1;
        size_t first;
        size_t len = log_saved_latest(start_len, text + gap_len, size - gap_len, &first, wait);
        char *data = text + gap_len;
        if (first > start_len) {
            data = text;
            memcpy(text, gap, gap_len);
            len += gap_len;
        }
        status = efi_runtime_set_variable(end_name, &guid, ATTRIBUTES, len, data, wait);
        if (status != EFI_SUCCESS) {
            return status;
        }
    }
    saved_count = count;
    return EFI_SUCCESS;
}

// Says on the screen too why the log is saved again only at the end, as the
// user may otherwise find none at the next start.
static void refused(efi_status status) {
    uint64_t storage = 0;
    uint64_t remaining = 0;
    uint64_t max_size = 0;
    efi_status queried = efi_runtime_query_variable_info(
        ATTRIBUTES, &storage, &remaining, &max_size, true
    );
    log_notice(
        "The log is saved again only at a panic or before a reset or power off: the "
        "firmware's variable store refuses it, status %#llx, with %llu of %llu bytes free and "
        "%llu at most for a variable (status %#llx)\n", (unsigned long long)status,
        (unsigned long long)remaining, (unsigned long long)storage, (unsigned long long)max_size,
        (unsigned long long)queried
    );
}

static void timer_fired(void *ctx) {
    (void)ctx;
    if (log_saved_count() != saved_count) {
        efi_status status = save(true, false);
        if (status != EFI_SUCCESS) {
            refused(status);
            return;
        }
        if (++saves == SAVE_LIMIT) {
            printf(
                "The log is saved again only at a panic or before a reset or power off, after "
                "%u saves\n",
                SAVE_LIMIT
            );
            return;
        }
    }
    hv_timer_set(&timer, rdtsc() + SAVE_INTERVAL_S * time_tsc_hz());
}

static void keep(bool wait) {
    if (log_saved_count() != saved_count) {
        save(wait, true);
    }
}

void log_save_start(void) {
    if (core_handoff->config.save_log == 0) {
        return;
    }
    efi_runtime_init();
    // The log's start is the larger of the two variables.
    text = page_alloc(page_count(LOG_SAVED_START));
    if (text == NULL) {
        printf("Cannot save the log: out of memory\n");
        return;
    }
    printf("The log is saved in the ReBIOSLog and ReBIOSLogEnd EFI variables as it grows\n");
    timer.fired = timer_fired;
    hv_timer_add(&timer);
    hv_timer_set(&timer, rdtsc());
    log_set_keeper(keep);
}
