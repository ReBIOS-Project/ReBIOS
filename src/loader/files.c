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
#include <loader/files.h>
#include <lib/util.h>
#include <loader/efi_glue.h>

static struct efi_guid loaded_image_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
static struct efi_guid simple_file_system_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;

// Device path nodes are byte-packed, so their fields may be unaligned.
static size_t node_length(const uint8_t *node) {
    return read_le16(node + 2);
}

static bool is_file_path_node(const uint8_t *node) {
    return node[0] == EFI_MEDIA_DEVICE_PATH && node[1] == EFI_MEDIA_FILEPATH_DP;
}

// The room for the path name in a file path node, in characters.
static size_t node_path_chars(const uint8_t *node) {
    return (node_length(node) - EFI_SIZE_OF_FILEPATH_DEVICE_PATH) / sizeof(uint16_t);
}

// The absolute path of name in the directory of the image's file path, in a
// pool allocation, or NULL if that directory is unknown.
uint16_t *files_sibling_path(const struct efi_device_path_protocol *image_path, const char *name) {
    if (image_path == NULL) {
        return NULL;
    }

    size_t path_chars = 0;
    bool found = false;
    const uint8_t *node = (const uint8_t *)image_path;
    for (; node[0] != EFI_END_DEVICE_PATH_TYPE; node += node_length(node)) {
        if (node_length(node) < sizeof(struct efi_device_path_protocol)) {
            return NULL;
        }
        if (is_file_path_node(node)) {
            path_chars += node_path_chars(node);
            found = true;
        }
    }
    if (!found) {
        return NULL;
    }

    size_t name_len = 0;
    while (name[name_len] != 0) {
        name_len++;
    }
    // A leading separator, the image's path, the name, and a NUL.
    uint16_t *path = efi_glue_alloc_pool((path_chars + name_len + 2) * sizeof(uint16_t));
    if (path == NULL) {
        return NULL;
    }

    // Open() only knows backslashes, and the nodes may each begin and end with
    // one, so separators become single backslashes.
    size_t len = 1;
    size_t dir_len = 1;
    path[0] = '\\';
    node = (const uint8_t *)image_path;
    for (; node[0] != EFI_END_DEVICE_PATH_TYPE; node += node_length(node)) {
        if (!is_file_path_node(node)) {
            continue;
        }
        for (size_t i = 0; i < node_path_chars(node); i++) {
            uint16_t c = read_le16(node + EFI_SIZE_OF_FILEPATH_DEVICE_PATH + i * 2);
            if (c == 0) {
                break;
            }
            if (c == '/' || c == '\\') {
                if (path[len - 1] == '\\') {
                    continue;
                }
                c = '\\';
                dir_len = len + 1;
            }
            path[len++] = c;
        }
    }

    for (size_t i = 0; i <= name_len; i++) {
        path[dir_len + i] = (unsigned char)name[i];
    }
    return path;
}

bool files_open_image_volume(
    struct efi_file_protocol **root,
    const struct efi_device_path_protocol **image_path
) {
    struct efi_loaded_image_protocol *image;
    efi_status status = efi_bs->handle_protocol(
        efi_image_handle, &loaded_image_guid, (void **)&image
    );
    if (status != EFI_SUCCESS || image == NULL || image->device_handle == NULL) {
        return false;
    }
    struct efi_simple_file_system_protocol *volume;
    status = efi_bs->handle_protocol(
        image->device_handle, &simple_file_system_guid, (void **)&volume
    );
    if (status != EFI_SUCCESS || volume == NULL
     || volume->open_volume(volume, root) != EFI_SUCCESS) {
        return false;
    }
    *image_path = image->file_path;
    return true;
}

void files_path_text(char *out, size_t size, const uint16_t *path) {
    size_t i = 0;
    for (; i + 1 < size && path[i] != 0; i++) {
        out[i] = path[i] >= 0x20 && path[i] < 0x7f ? (char)path[i] : '?';
    }
    out[i] = 0;
}
