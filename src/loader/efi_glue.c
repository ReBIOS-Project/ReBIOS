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
#include <loader/efi_glue.h>

efi_handle efi_image_handle;
struct efi_system_table *efi_st;
struct efi_boot_services *efi_bs;
struct efi_runtime_services *efi_rt;
bool efi_glue_boot_services_exited;

uintptr_t efi_glue_alloc_pages(size_t count, uintptr_t max_addr) {
    return efi_glue_alloc_pages_typed(count, max_addr, EFI_LOADER_DATA);
}

// Some firmware leaves memory above 4 GiB out of the page tables the loader
// runs on until it has its own, so that is where an address-free allocation
// goes last.
uintptr_t efi_glue_alloc_pages_typed(size_t count, uintptr_t max_addr, uint32_t type) {
    efi_physical_address addr = max_addr == 0 ? UINT32_MAX : max_addr;
    if (efi_bs->allocate_pages(EFI_ALLOCATE_MAX_ADDRESS, type, count, &addr) == EFI_SUCCESS) {
        return addr;
    }
    if (max_addr != 0
     || efi_bs->allocate_pages(EFI_ALLOCATE_ANY_PAGES, type, count, &addr) != EFI_SUCCESS) {
        return 0;
    }
    return addr;
}

void *efi_glue_alloc_pool(size_t size) {
    void *buf;
    if (efi_bs->allocate_pool(EFI_LOADER_DATA, size, &buf) != EFI_SUCCESS) {
        return NULL;
    }
    return buf;
}

efi_handle *efi_glue_locate_handles(struct efi_guid *protocol, size_t *count) {
    efi_handle *handles;

    // LocateHandleBuffer() came with EFI 1.10, as did the protocols of
    // interest.
    if (efi_bs->hdr.revision < EFI_1_10_SYSTEM_TABLE_REVISION
     || efi_bs->locate_handle_buffer(EFI_BY_PROTOCOL, protocol, NULL, count, &handles)
         != EFI_SUCCESS) {
        *count = 0;
        return NULL;
    }
    return handles;
}
