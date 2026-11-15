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

#ifndef LOADER__FILES_H
#define LOADER__FILES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <efi.h>

// The root directory of the volume that the loader's image was started from,
// and the image's file path on it, or false where the firmware does not give
// them.
bool files_open_image_volume(
    struct efi_file_protocol **root,
    const struct efi_device_path_protocol **image_path
);
// The absolute path of name in the directory of the image's file path, in a
// pool allocation, or NULL if that directory is unknown.
uint16_t *files_sibling_path(const struct efi_device_path_protocol *image_path, const char *name);
// Writes size bytes of data to the file name beside the loader's image, in
// place of any there, and the file's path to where, cut short to fit in
// where_size bytes. Returns NULL once it is written, else why it is not, with
// the firmware's status in *status.
const char *files_write_beside(
    const char *name,
    const void *data,
    size_t size,
    char *where,
    size_t where_size,
    efi_status *status
);
// A file's path as text for the log, cut short to fit in size bytes.
void files_path_text(char *out, size_t size, const uint16_t *path);

#endif
