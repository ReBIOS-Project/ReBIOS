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

static EFI_CHAR16 start_variable[] = HANDOFF_LOG_VARIABLE;
static EFI_CHAR16 end_variable[] = HANDOFF_LOG_END_VARIABLE;
static EFI_GUID guid = HANDOFF_VARIABLE_GUID;

static bool write_file(const char *text, size_t size) {
    EFI_FILE_PROTOCOL *root;
    const EFI_DEVICE_PATH_PROTOCOL *image_path;
    if (!files_open_image_volume(&root, &image_path)) {
        return false;
    }
    bool written = false;
    EFI_CHAR16 *path = files_sibling_path(image_path, FILE_NAME);
    if (path == NULL) {
        goto out;
    }
    // A file left from before goes first, as an opened file keeps its length.
    EFI_FILE_PROTOCOL *file;
    EFI_UINT64 mode = EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE;
    EFI_STATUS status = root->Open(root, &file, path, mode, 0);
    if (status == EFI_SUCCESS) {
        status = file->Delete(file);
    } else if (status == EFI_NOT_FOUND) {
        status = EFI_SUCCESS;
    }
    if (status == EFI_SUCCESS
     && root->Open(root, &file, path, mode | EFI_FILE_MODE_CREATE, 0) == EFI_SUCCESS) {
        EFI_UINTN done = size;
        written = file->Write(file, &done, (void *)text) == EFI_SUCCESS && done == size;
        written = file->Close(file) == EFI_SUCCESS && written;
    }
    efi_bs->FreePool(path);
out:
    root->Close(root);
    return written;
}

// Appends the variable's contents to text, at *len, room bytes at most.
// Returns whether the variable exists.
static bool append_variable(EFI_CHAR16 *name, char *text, size_t *len, size_t room) {
    EFI_UINTN size = 0;
    EFI_STATUS status = efi_rt->GetVariable(name, &guid, NULL, &size, NULL);
    if (status != EFI_BUFFER_TOO_SMALL || size == 0) {
        return false;
    }
    if (size > room - *len
     || efi_rt->GetVariable(name, &guid, NULL, &size, text + *len) != EFI_SUCCESS) {
        return true;
    }
    *len += size;
    return true;
}

void last_log_write(void) {
    char *text = efi_glue_alloc_pool(TEXT_MAX);
    if (text == NULL) {
        return;
    }
    size_t len = 0;
    bool start = append_variable(start_variable, text, &len, TEXT_MAX);
    bool end = append_variable(end_variable, text, &len, TEXT_MAX);
    if (len != 0) {
        // The file holds text alone, whatever the firmware hands back.
        for (size_t i = 0; i < len; i++) {
            if ((text[i] < 0x20 || text[i] > 0x7e) && text[i] != '\n' && text[i] != '\t') {
                text[i] = '?';
            }
        }
        if (!write_file(text, len)) {
            printf("Cannot write the log that ReBIOS saved last time to %s\n", FILE_NAME);
            efi_bs->FreePool(text);
            return;
        }
    }
    if (start) {
        efi_rt->SetVariable(start_variable, &guid, ATTRIBUTES, 0, NULL);
    }
    if (end) {
        efi_rt->SetVariable(end_variable, &guid, ATTRIBUTES, 0, NULL);
    }
    if (len != 0) {
        printf("Wrote the log that ReBIOS saved last time to %s\n", FILE_NAME);
    }
    efi_bs->FreePool(text);
}
