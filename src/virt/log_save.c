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
// or the power going off. Every write takes new room in the variable store, so
// each save writes only the text that the log gained since the last, in
// variables of its own (handoff.h). Firmware gives the room back as it
// collects the store's garbage, which rewrites the whole store
// (edk2-stable202608, MdeModulePkg/Universal/Variable/RuntimeDxe/Variable.c,
// Reclaim()): EDK2's before ExitBootServices() alone, and others also where a
// write does not fit, as Linux 7.3 has them do
// (arch/x86/platform/efi/quirks.c, efi_query_variable_store()). Only the last
// save asks for that, and the others stop where too little room is left.
#define SAVE_INTERVAL_S 10
#define SAVE_LIMIT 360
// Room in the store that the others leave for the last save.
#define LAST_ROOM 8192
// The least room worth a variable.
#define SAVE_MIN 1024
// Linux 7.3 keeps at least this much of the variable store free, a figure from
// Samsung, some of whose machines stop starting with less
// (arch/x86/platform/efi/quirks.c, efi_query_variable_store()).
#define STORE_RESERVE 5120
#define ATTRIBUTES \
    (EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS)

static uint16_t dummy_name[] = u"ReBIOSDummy";
static struct efi_guid guid = HANDOFF_VARIABLE_GUID;
static const char gap[] = "[...]\n";
static struct hv_timer timer;
// A variable's data, LOG_SAVED_START bytes at most.
static char *text;
// How much of the log the variables hold, and how many there are.
static size_t saved_count;
static size_t variables;
static size_t saves;

// How many bytes, want at most, a variable named name_size bytes may hold with
// kept bytes of the store left for later, in *size, as Linux 7.3's
// efi_query_variable_store() decides: 0 where the write would leave less than
// STORE_RESERVE free. With collect, where too little is left, a write too
// large to fit, which fails, first has the firmware collect the store's
// garbage, where it does that after ExitBootServices().
static efi_status room(
    size_t name_size,
    size_t want,
    size_t kept,
    bool wait,
    bool collect,
    size_t *size
) {
    uint64_t storage, remaining, max_size;
    efi_status status = efi_runtime_query_variable_info(
        ATTRIBUTES, &storage, &remaining, &max_size, wait
    );
    if (status == EFI_SUCCESS && collect && remaining < want + kept + STORE_RESERVE) {
        printf(
            "The log's last save has the firmware collect its variable store's garbage, with "
            "%llu of %llu bytes free\n",
            (unsigned long long)remaining, (unsigned long long)storage
        );
        size_t dummy_size = (size_t)remaining + 1024;
        void *dummy = kmalloc(dummy_size);
        if (dummy != NULL) {
            memset(dummy, 0, dummy_size);
            status = efi_runtime_set_variable(
                dummy_name, &guid, ATTRIBUTES, dummy_size, dummy, wait
            );
            if (status == EFI_SUCCESS) {
                efi_runtime_set_variable(dummy_name, &guid, ATTRIBUTES, 0, NULL, wait);
            }
            kfree(dummy);
        }
        status = efi_runtime_query_variable_info(
            ATTRIBUTES, &storage, &remaining, &max_size, wait
        );
    }
    *size = 0;
    if (status != EFI_SUCCESS) {
        return status;
    }
    if (max_size <= name_size || remaining <= STORE_RESERVE + kept) {
        return EFI_SUCCESS;
    }
    uint64_t fits = remaining - STORE_RESERVE - kept;
    if (fits > max_size - name_size) {
        fits = max_size - name_size;
    }
    *size = fits < want ? (size_t)fits : want;
    return EFI_SUCCESS;
}

// Writes the text that the log gained since the last save in variables of
// their own, as far as the store has room: the last save takes the room that
// the others leave, and may have the firmware collect the store's garbage
// first.
static efi_status save(bool wait, bool last) {
    size_t kept = last ? 0 : LAST_ROOM;
    size_t gap_len = sizeof(gap) - 1;
    while (saved_count < log_saved_count()) {
        if (variables == HANDOFF_LOG_PARTS) {
            return EFI_OUT_OF_RESOURCES;
        }
        uint16_t name[HANDOFF_LOG_PART_NAME_LEN];
        handoff_log_part_name(name, variables);
        size_t size;
        efi_status status = room(sizeof(name), LOG_SAVED_START, kept, wait, last && wait, &size);
        if (status != EFI_SUCCESS) {
            return status;
        }
        size_t pending = log_saved_count() - saved_count;
        if (size < SAVE_MIN && size < gap_len + pending) {
            return EFI_OUT_OF_RESOURCES;
        }
        size_t first;
        size_t len = log_saved_text(saved_count, text + gap_len, size - gap_len, &first, wait);
        // Marked where text is lost.
        char *data = text + gap_len;
        size_t data_len = len;
        if (first != saved_count) {
            memcpy(text, gap, gap_len);
            data = text;
            data_len += gap_len;
        }
        status = efi_runtime_set_variable(name, &guid, ATTRIBUTES, data_len, data, wait);
        if (status != EFI_SUCCESS) {
            return status;
        }
        saved_count = first + len;
        variables++;
    }
    return EFI_SUCCESS;
}

// Says on the screen too why the log is saved again only at the end, as what
// the next start writes out may otherwise end early without a word.
static void refused(efi_status status) {
    uint64_t storage = 0;
    uint64_t remaining = 0;
    uint64_t max_size = 0;
    efi_status queried = efi_runtime_query_variable_info(
        ATTRIBUTES, &storage, &remaining, &max_size, true
    );
    const char *why = "refuses it";
    if (status == EFI_OUT_OF_RESOURCES) {
        why = "has too little room left for it";
    }
    log_notice(
        "The log is saved again only at a panic or before a reset or power off: the "
        "firmware's variable store %s, status %#llx, with %llu of %llu bytes free and %llu at "
        "most for a variable (status %#llx)\n",
        why, (unsigned long long)status, (unsigned long long)remaining,
        (unsigned long long)storage, (unsigned long long)max_size, (unsigned long long)queried
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
    if (core_handoff->config.save_log != HANDOFF_SAVE_LOG_EFI) {
        return;
    }
    efi_runtime_init();
    text = page_alloc(page_count(LOG_SAVED_START));
    if (text == NULL) {
        printf("Cannot save the log: out of memory\n");
        return;
    }
    printf("The log is saved as it grows, in EFI variables from ReBIOSLog0000 on\n");
    timer.fired = timer_fired;
    hv_timer_add(&timer);
    hv_timer_set(&timer, rdtsc());
    log_set_keeper(keep);
}
