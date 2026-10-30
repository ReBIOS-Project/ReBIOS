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

#ifndef LOADER__EFI_GLUE_H
#define LOADER__EFI_GLUE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <efi.h>

extern EFI_HANDLE efi_image_handle;
extern EFI_SYSTEM_TABLE *efi_st;
extern EFI_BOOT_SERVICES *efi_bs;
extern EFI_RUNTIME_SERVICES *efi_rt;
extern bool efi_boot_services_exited;

// Returns 0 on failure. max_addr is inclusive; 0 means anywhere.
uintptr_t efi_alloc_pages(size_t count, uintptr_t max_addr);
uintptr_t efi_alloc_pages_typed(
    size_t count, uintptr_t max_addr, EFI_MEMORY_TYPE type
);

// Returns the handles with the protocol, in handle order, in pool memory to
// free with FreePool(), or NULL if there are none.
EFI_HANDLE *efi_locate_handles(EFI_GUID *protocol, EFI_UINTN *count);

#endif
