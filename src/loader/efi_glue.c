// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of ReBIOS.
// Copyright (C) 2026 Mintsuki and contributors
//
// ReBIOS is free software: you can redistribute it and/or modify it under the
// terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version.
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
    EFI_PHYSICAL_ADDRESS addr = max_addr;
    EFI_ALLOCATE_TYPE type = max_addr == 0 ? EfiAllocateAnyPages : EfiAllocateMaxAddress;

    if (efi_bs->AllocatePages(type, EfiLoaderData, count, &addr) != EFI_SUCCESS) {
        return 0;
    }
    return addr;
}
