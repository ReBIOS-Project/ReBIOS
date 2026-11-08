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

#ifndef SYS__EFI_RUNTIME_H
#define SYS__EFI_RUNTIME_H

#include <stdbool.h>
#include <efi.h>

// Before the first call.
void efi_runtime_init(void);

// The firmware's variable services, which the core calls one at a time, as
// UEFI 2.11, 8.1 asks. Without wait, they return EFI_NOT_READY rather than
// wait for a call in progress on another processor.
EFI_STATUS efi_runtime_query_variable_info(
    EFI_UINT32 attributes, EFI_UINT64 *storage, EFI_UINT64 *remaining, EFI_UINT64 *max_size,
    bool wait
);
EFI_STATUS efi_runtime_set_variable(
    EFI_CHAR16 *name, EFI_GUID *guid, EFI_UINT32 attributes, EFI_UINTN size, void *data, bool wait
);

#endif
