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
#include <core.h>
#include <handoff.h>
#include <lib/log.h>
#include <loader/bootdev.h>
#include <loader/config.h>
#include <loader/console.h>
#include <loader/display.h>
#include <loader/drives.h>
#include <loader/efi_glue.h>
#include <loader/fwtables.h>
#include <loader/iommu_check.h>
#include <loader/loader.h>
#include <loader/oprom.h>
#include <loader/paging.h>
#include <loader/video.h>
#include <memory.h>
#include <sys/x86.h>

#define CORE_HEAP_SIZE ((uint64_t)64 << 20)

struct handoff *loader_handoff;

static EFI_GUID loaded_image_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;

static size_t page_count(size_t size) {
    return (size + 4095) / 4096;
}

static void reserve(uintptr_t base, size_t pages) {
    if (loader_handoff->reserved_count == HANDOFF_MAX_RESERVED) {
        panic("Too many hypervisor memory ranges\n");
    }
    loader_handoff->reserved[loader_handoff->reserved_count].base = base;
    loader_handoff->reserved[loader_handoff->reserved_count].size = pages
        * 4096;
    loader_handoff->reserved_count++;
}

uint64_t loader_alloc_reserved(size_t pages, uintptr_t max_addr) {
    uintptr_t base = efi_alloc_pages(pages, max_addr);
    if (base != 0) {
        reserve(base, pages);
    }
    return base;
}

// Timed against the firmware's delay service, whatever the TSC's frequency
// enumeration says, which not every processor provides.
static uint64_t measure_tsc_hz(void) {
    uintptr_t start = rdtsc();
    efi_bs->Stall(20000);
    return (rdtsc() - start) * 50;
}

// The memory map buffer must exist before the map that goes into it is taken,
// so it gets slack for the descriptors its own allocation adds. It is only
// recorded as reserved once final, so that it is not counted twice.
static bool exit_boot_services(void) {
    EFI_UINTN map_key;
    size_t desc_size;
    EFI_UINT32 desc_version;
    uintptr_t buf = 0;
    size_t buf_size = 0;

    // Room for the buffer's own entry.
    if (loader_handoff->reserved_count == HANDOFF_MAX_RESERVED) {
        return false;
    }

    for (size_t attempt = 0; attempt < 16; attempt++) {
        size_t size = buf_size;
        EFI_STATUS status = efi_bs->GetMemoryMap(
            &size, (EFI_MEMORY_DESCRIPTOR *)(uintptr_t)buf, &map_key,
            &desc_size, &desc_version
        );
        if (status == EFI_BUFFER_TOO_SMALL) {
            if (buf != 0) {
                efi_bs->FreePages(buf, page_count(buf_size));
            }
            buf_size = (size + 16 * desc_size + 4095) & ~(uint64_t)4095;
            buf = efi_alloc_pages(page_count(buf_size), 0);
            if (buf == 0) {
                return false;
            }
            continue;
        }
        if (status != EFI_SUCCESS
         || desc_size < sizeof(EFI_MEMORY_DESCRIPTOR)) {
            return false;
        }

        if (efi_bs->ExitBootServices(efi_image_handle, map_key)
         == EFI_SUCCESS) {
            efi_boot_services_exited = true;
            reserve(buf, page_count(buf_size));
            loader_handoff->efi_mmap = buf;
            loader_handoff->efi_mmap_size = size;
            loader_handoff->efi_mmap_desc_size = desc_size;
            loader_handoff->efi_mmap_desc_version = desc_version;
            return true;
        }
    }

    return false;
}

static uintptr_t memory_map_top(void) {
    size_t size = 0;
    EFI_UINTN map_key;
    size_t desc_size;
    EFI_UINT32 desc_version;
    uintptr_t top = (uint64_t)1 << 32;

    efi_bs->GetMemoryMap(&size, NULL, &map_key, &desc_size, &desc_version);
    size += 16 * desc_size;
    size_t pages = page_count(size);
    uintptr_t buf = efi_alloc_pages(pages, 0);
    if (buf == 0) {
        return top;
    }
    EFI_STATUS status = efi_bs->GetMemoryMap(
        &size, (EFI_MEMORY_DESCRIPTOR *)(uintptr_t)buf, &map_key, &desc_size,
        &desc_version
    );
    if (status == EFI_SUCCESS && desc_size >= sizeof(EFI_MEMORY_DESCRIPTOR)) {
        for (size_t off = 0; off + desc_size <= size; off += desc_size) {
            EFI_MEMORY_DESCRIPTOR *desc
                = (EFI_MEMORY_DESCRIPTOR *)(uintptr_t)(buf + off);
            uintptr_t end = desc->PhysicalStart + desc->NumberOfPages * 4096;
            if (end > top) {
                top = end;
            }
        }
    }
    efi_bs->FreePages(buf, pages);
    return top;
}

EFI_STATUS efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *system_table) {
    efi_image_handle = image_handle;
    efi_st = system_table;
    efi_bs = system_table->BootServices;
    efi_rt = system_table->RuntimeServices;

    efi_bs->SetWatchdogTimer(0, 0, 0, NULL);

    printf("ReBIOS\n");
    config_load();

    uintptr_t handoff_addr = efi_alloc_pages(
        page_count(sizeof(struct handoff)), 0
    );
    if (handoff_addr == 0) {
        panic("Cannot allocate the handoff\n");
    }
    loader_handoff = (struct handoff *)(uintptr_t)handoff_addr;
    memset(loader_handoff, 0, sizeof(struct handoff));
    reserve(handoff_addr, page_count(sizeof(struct handoff)));

    video_find_framebuffer();
    console_init();
    log_init(
        config.serial ? config.serial_port : 0, config.serial_baud,
        config.verbose
    );

    if (!iommu_check()) {
        return EFI_UNSUPPORTED;
    }

    // The core runs from this image, which stays where the firmware put it.
    EFI_LOADED_IMAGE_PROTOCOL *image;
    EFI_STATUS status = efi_bs->HandleProtocol(
        image_handle, &loaded_image_guid, (EFI_VOID **)&image
    );
    if (status != EFI_SUCCESS) {
        panic("Cannot find where ReBIOS is loaded\n");
    }
    reserve((uintptr_t)image->ImageBase, page_count(image->ImageSize));

    uintptr_t map_top = memory_map_top();
    size_t table_pages;
    uintptr_t pml4 = paging_build_identity_map(&map_top, &table_pages);
    if (pml4 == 0) {
        panic("Cannot build the core's page tables\n");
    }
    reserve(pml4, table_pages);

    // Long mode on the firmware's terms: under 5-level paging, the tables are
    // reached through a PML5 of one entry, as LA57 cannot change in long mode.
    uint64_t cr3 = pml4;
    if ((read_cr4() & CR4_LA57) != 0) {
        cr3 = loader_alloc_reserved(1, UINT32_MAX);
        if (cr3 == 0) {
            panic("Cannot allocate a PML5\n");
        }
        memset((void *)(uintptr_t)cr3, 0, 4096);
        *(uint64_t *)(uintptr_t)cr3 = pml4 | 3;
    }

    // Where SIPIs can start the other processors in real mode.
    uint64_t ap_boot = loader_alloc_reserved(1, 0xfffff);
    if (ap_boot == 0) {
        printf("No page below 1 MiB to start the other processors in\n");
    }

    size_t heap_size = CORE_HEAP_SIZE + (config.nested ? NESTED_HEAP_SIZE : 0);
    uint64_t heap = loader_alloc_reserved(page_count(heap_size), 0);
    if (heap == 0) {
        panic("Cannot allocate the core's heap\n");
    }

    loader_handoff->efi_system_table = (uintptr_t)system_table;
    loader_handoff->efi_rt = (uintptr_t)efi_rt;
    loader_handoff->pml4 = pml4;
    loader_handoff->map_top = map_top;
    loader_handoff->ap_boot = ap_boot;
    loader_handoff->heap_base = heap;
    loader_handoff->heap_size = heap_size;
    loader_handoff->tsc_hz = measure_tsc_hz();
    config_to_handoff(&loader_handoff->config);
    fwtables_collect();
    drives_connect();
    bootdev_collect();
    drives_collect();
    video_init();
    // Firmware may put the framebuffer above all memory.
    const struct handoff_framebuffer *fb = &loader_handoff->video.framebuffer;
    size_t fb_size = (uint64_t)fb->pitch * fb->height;
    if (fb->size > fb_size) {
        fb_size = fb->size;
    }
    if (fb->present && !paging_map_device(pml4, fb->base, fb_size)) {
        panic("Cannot map the framebuffer\n");
    }
    display_collect();
    oprom_collect();

    printf("Map top %#llx\n", (unsigned long long)map_top);

    video_exit_boot_services();
    if (!exit_boot_services()) {
        panic("Cannot exit boot services\n");
    }

    // The new tables identity map this code and its stack, like the
    // firmware's.
    cli();
    write_cr3(cr3);
    core_main(loader_handoff);
}
