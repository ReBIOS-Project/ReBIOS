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

#include <stdint.h>
#include <stdbool.h>
#include <efi.h>
#include <loader/efi_glue.h>

EFI_HANDLE efi_image_handle;
EFI_SYSTEM_TABLE *efi_st;
EFI_BOOT_SERVICES *efi_bs;
EFI_RUNTIME_SERVICES *efi_rt;
bool efi_boot_services_exited;

uint64_t efi_alloc_pages(uint64_t count, uint64_t max_addr) {
    return efi_alloc_pages_typed(count, max_addr, EfiLoaderData);
}

uint64_t efi_alloc_pages_typed(uint64_t count, uint64_t max_addr, EFI_MEMORY_TYPE type) {
    EFI_PHYSICAL_ADDRESS addr = max_addr;
    EFI_ALLOCATE_TYPE alloc_type = max_addr == 0 ? EfiAllocateAnyPages : EfiAllocateMaxAddress;

    if (efi_bs->AllocatePages(alloc_type, type, count, &addr) != EFI_SUCCESS) {
        return 0;
    }
    return addr;
}

EFI_HANDLE *efi_locate_handles(EFI_GUID *protocol, EFI_UINTN *count) {
    EFI_HANDLE *handles;

    // LocateHandleBuffer() came with EFI 1.10, as did the protocols of interest.
    if (efi_bs->Hdr.Revision < EFI_1_10_SYSTEM_TABLE_REVISION
        || efi_bs->LocateHandleBuffer(EfiByProtocol, protocol, NULL, count,
                                      &handles) != EFI_SUCCESS) {
        *count = 0;
        return NULL;
    }
    return handles;
}
