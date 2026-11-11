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

#ifndef LOADER__FILES_H
#define LOADER__FILES_H

#include <stdbool.h>
#include <efi.h>

// The root directory of the volume that the loader's image was started from,
// and the image's file path on it, or false where the firmware does not give
// them.
bool files_open_image_volume(
    EFI_FILE_PROTOCOL **root,
    const EFI_DEVICE_PATH_PROTOCOL **image_path
);
// The absolute path of name in the directory of the image's file path, in a
// pool allocation, or NULL if that directory is unknown.
EFI_CHAR16 *files_sibling_path(const EFI_DEVICE_PATH_PROTOCOL *image_path, const char *name);

#endif
