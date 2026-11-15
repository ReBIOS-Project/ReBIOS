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
#include <loader/last_log.h>
#include <handoff.h>
#include <lib/log.h>
#include <loader/efi_glue.h>
#include <loader/files.h>

#define FILE_NAME "rebios-last.log"
// Far more than the core saves (virt/log_save.c), so as to refuse what a
// firmware gets wrong.
#define TEXT_MAX 0x100000
#define ATTRIBUTES \
    (EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS)

static uint16_t start_variable[] = HANDOFF_LOG_VARIABLE;
static uint16_t end_variable[] = HANDOFF_LOG_END_VARIABLE;
static struct efi_guid guid = HANDOFF_VARIABLE_GUID;

// Appends the variable's contents to text, at *len, room bytes at most.
// Returns whether the variable exists.
static bool append_variable(uint16_t *name, char *text, size_t *len, size_t room) {
    size_t size = 0;
    efi_status status = efi_rt->get_variable(name, &guid, NULL, &size, NULL);
    if (status != EFI_BUFFER_TOO_SMALL || size == 0) {
        return false;
    }
    if (size > room - *len
     || efi_rt->get_variable(name, &guid, NULL, &size, text + *len) != EFI_SUCCESS) {
        return true;
    }
    *len += size;
    return true;
}

void last_log_write(bool save_log) {
    char *text = efi_glue_alloc_pool(TEXT_MAX);
    if (text == NULL) {
        return;
    }
    size_t len = 0;
    bool start = append_variable(start_variable, text, &len, TEXT_MAX);
    bool end = append_variable(end_variable, text, &len, TEXT_MAX);
    if (len == 0 && save_log) {
        log_notice(
            "No log from ReBIOS's last start for %s: save_log was off then, or the firmware's "
            "variable store refused it\n", FILE_NAME
        );
    }
    if (len != 0) {
        // The file holds text alone, whatever the firmware hands back.
        for (size_t i = 0; i < len; i++) {
            if ((text[i] < 0x20 || text[i] > 0x7e) && text[i] != '\n' && text[i] != '\t') {
                text[i] = '?';
            }
        }
        char where[256] = FILE_NAME;
        efi_status status;
        const char *why = files_write_beside(FILE_NAME, text, len, where, sizeof(where), &status);
        if (why != NULL) {
            log_notice(
                "Cannot write the log that ReBIOS saved last time to %s: %s, status %#llx\n",
                where, why, (unsigned long long)status
            );
            efi_bs->free_pool(text);
            return;
        }
        log_notice("Wrote the log that ReBIOS saved last time to %s\n", where);
    }
    if (start) {
        efi_rt->set_variable(start_variable, &guid, ATTRIBUTES, 0, NULL);
    }
    if (end) {
        efi_rt->set_variable(end_variable, &guid, ATTRIBUTES, 0, NULL);
    }
    efi_bs->free_pool(text);
}
