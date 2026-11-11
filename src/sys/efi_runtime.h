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
// wait for a call in progress on another processor, and EFI_ABORTED for a call
// in which the firmware's code took a page fault, then EFI_UNSUPPORTED.
efi_status efi_runtime_query_variable_info(
    uint32_t attributes,
    uint64_t *storage,
    uint64_t *remaining,
    uint64_t *max_size,
    bool wait
);
efi_status efi_runtime_set_variable(
    uint16_t *name,
    struct efi_guid *guid,
    uint32_t attributes,
    size_t size,
    void *data,
    bool wait
);

// The firmware's reset, which returns if it fails.
void efi_runtime_reset_system(uint32_t type);

#endif
