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

extern efi_handle efi_image_handle;
extern struct efi_system_table *efi_st;
extern struct efi_boot_services *efi_bs;
extern struct efi_runtime_services *efi_rt;
extern bool efi_glue_boot_services_exited;

// Returns 0 on failure. max_addr is inclusive; 0 means anywhere.
uintptr_t efi_glue_alloc_pages(size_t count, uintptr_t max_addr);
uintptr_t efi_glue_alloc_pages_typed(size_t count, uintptr_t max_addr, uint32_t type);

// Loader data, to free with FreePool(), or NULL on failure.
void *efi_glue_alloc_pool(size_t size);

// Returns the handles with the protocol, in handle order, in pool memory to
// free with FreePool(), or NULL if there are none.
efi_handle *efi_glue_locate_handles(struct efi_guid *protocol, size_t *count);

#endif
