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
#include <stddef.h>
#include <stdbool.h>
#include <efi.h>
#include <memory.h>
#include <core.h>
#include <handoff.h>
#include <lib/log.h>
#include <loader/efi_glue.h>
#include <loader/paging.h>
#include <sys/x86.h>

#define CORE_HEAP_SIZE (64ull << 20)

static uint64_t page_count(uint64_t size) {
    return (size + 4095) / 4096;
}

// The memory map buffer must exist before the map that goes into it is taken,
// so it gets slack for the descriptors its own allocation adds.
static bool exit_boot_services(struct handoff *handoff) {
    EFI_UINTN map_size = 0;
    EFI_UINTN map_key;
    EFI_UINTN desc_size;
    EFI_UINT32 desc_version;
    uint64_t buf = 0;
    uint64_t buf_size = 0;

    for (int attempt = 0; attempt < 16; attempt++) {
        EFI_UINTN size = buf_size;
        EFI_STATUS status = efi_bs->GetMemoryMap(&size, (EFI_MEMORY_DESCRIPTOR *)(uintptr_t)buf,
                                                 &map_key, &desc_size, &desc_version);
        if (status == EFI_BUFFER_TOO_SMALL) {
            if (buf != 0) {
                efi_bs->FreePages(buf, page_count(buf_size));
            }
            buf_size = (size + 16 * desc_size + 4095) & ~4095ull;
            buf = efi_alloc_pages(page_count(buf_size), 0);
            if (buf == 0) {
                return false;
            }
            continue;
        }
        if (status != EFI_SUCCESS) {
            return false;
        }
        map_size = size;

        if (efi_bs->ExitBootServices(efi_image_handle, map_key) == EFI_SUCCESS) {
            efi_boot_services_exited = true;
            handoff->efi_mmap = buf;
            handoff->efi_mmap_size = map_size;
            handoff->efi_mmap_desc_size = desc_size;
            handoff->efi_mmap_desc_version = desc_version;
            return true;
        }
    }

    return false;
}

static uint64_t memory_map_top(void) {
    EFI_UINTN size = 0;
    EFI_UINTN map_key;
    EFI_UINTN desc_size;
    EFI_UINT32 desc_version;
    uint64_t top = 1ull << 32;

    efi_bs->GetMemoryMap(&size, NULL, &map_key, &desc_size, &desc_version);
    size += 16 * desc_size;
    uint64_t buf = efi_alloc_pages(page_count(size), 0);
    if (buf == 0) {
        return top;
    }
    if (efi_bs->GetMemoryMap(&size, (EFI_MEMORY_DESCRIPTOR *)(uintptr_t)buf, &map_key,
                             &desc_size, &desc_version) == EFI_SUCCESS) {
        for (EFI_UINTN off = 0; off + desc_size <= size; off += desc_size) {
            EFI_MEMORY_DESCRIPTOR *desc = (EFI_MEMORY_DESCRIPTOR *)(uintptr_t)(buf + off);
            uint64_t end = desc->PhysicalStart + desc->NumberOfPages * 4096;
            if (end > top) {
                top = end;
            }
        }
    }
    efi_bs->FreePages(buf, page_count(size));
    return top;
}

EFI_STATUS efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *system_table) {
    efi_image_handle = image_handle;
    efi_st = system_table;
    efi_bs = system_table->BootServices;
    efi_rt = system_table->RuntimeServices;

    efi_bs->SetWatchdogTimer(0, 0, 0, NULL);

    log_init(0x3f8, 115200);
    printf("ReBIOS\n");

    uint64_t map_top = memory_map_top();
    uint64_t pml4 = paging_build_identity_map(&map_top);
    if (pml4 == 0) {
        panic("Cannot build the core's page tables\n");
    }

    uint64_t heap = efi_alloc_pages(page_count(CORE_HEAP_SIZE), 0);
    if (heap == 0) {
        panic("Cannot allocate the core's heap\n");
    }

    uint64_t handoff_addr = efi_alloc_pages(page_count(sizeof(struct handoff)), 0);
    if (handoff_addr == 0) {
        panic("Cannot allocate the handoff\n");
    }
    struct handoff *handoff = (struct handoff *)(uintptr_t)handoff_addr;
    memset(handoff, 0, sizeof(*handoff));
    handoff->pml4 = pml4;
    handoff->map_top = map_top;
    handoff->heap_base = heap;
    handoff->heap_size = CORE_HEAP_SIZE;

    printf("Map top %#llx\n", (unsigned long long)map_top);

    if (!exit_boot_services(handoff)) {
        panic("Cannot exit boot services\n");
    }

    // The new tables identity map this code and its stack, like the firmware's.
    cli();
    write_cr3(pml4);
    core_main(handoff);
}
