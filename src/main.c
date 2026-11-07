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
#include <lib/util.h>
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
#include <loader/pci_io.h>
#include <loader/video.h>
#include <memory.h>
#include <sys/x86.h>

#define CORE_HEAP_SIZE ((uint64_t)64 << 20)
// The page tables the processor runs the guest's guests with, which the
// hypervisor builds from the guest's and its own and drops when they outgrow
// their share: 1/NESTED_HEAP_SHARE of the RAM, at most NESTED_HEAP_MAX.
#define NESTED_HEAP_SHARE 32
#define NESTED_HEAP_MAX ((uint64_t)128 << 20)

struct handoff *loader_handoff;

static EFI_GUID loaded_image_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;

static void reserve(uintptr_t base, size_t pages) {
    if (loader_handoff->reserved_count == HANDOFF_MAX_RESERVED) {
        panic("Too many hypervisor memory ranges\n");
    }
    loader_handoff->reserved[loader_handoff->reserved_count].base = base;
    loader_handoff->reserved[loader_handoff->reserved_count].size = pages * 4096;
    loader_handoff->reserved_count++;
}

uint64_t loader_alloc_reserved(size_t pages, uintptr_t max_addr) {
    uintptr_t base = efi_glue_alloc_pages(pages, max_addr);
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
            &size, (EFI_MEMORY_DESCRIPTOR *)(uintptr_t)buf, &map_key, &desc_size, &desc_version
        );
        if (status == EFI_BUFFER_TOO_SMALL) {
            if (buf != 0) {
                efi_bs->FreePages(buf, EFI_SIZE_TO_PAGES(buf_size));
            }
            buf_size = align_up(size + 16 * desc_size, EFI_PAGE_SIZE);
            buf = efi_glue_alloc_pages(EFI_SIZE_TO_PAGES(buf_size), 0);
            if (buf == 0) {
                return false;
            }
            continue;
        }
        if (status != EFI_SUCCESS || desc_size < sizeof(EFI_MEMORY_DESCRIPTOR)) {
            return false;
        }

        if (efi_bs->ExitBootServices(efi_image_handle, map_key) == EFI_SUCCESS) {
            efi_glue_boot_services_exited = true;
            reserve(buf, EFI_SIZE_TO_PAGES(buf_size));
            loader_handoff->efi_mmap = buf;
            loader_handoff->efi_mmap_size = size;
            loader_handoff->efi_mmap_desc_size = desc_size;
            loader_handoff->efi_mmap_desc_version = desc_version;
            return true;
        }
    }

    return false;
}

// The memory the OS may use once boot services end (UEFI 2.11, Table 7.6).
static bool ram_type(uint32_t type) {
    return type == EfiLoaderCode || type == EfiLoaderData || type == EfiBootServicesCode
        || type == EfiBootServicesData || type == EfiConventionalMemory;
}

// The top of the memory map, 4 GiB at least, and the RAM in it.
static uintptr_t memory_map_top(uint64_t *ram) {
    size_t size = 0;
    EFI_UINTN map_key;
    size_t desc_size;
    EFI_UINT32 desc_version;
    uintptr_t top = FOUR_GIB;

    *ram = 0;
    efi_bs->GetMemoryMap(&size, NULL, &map_key, &desc_size, &desc_version);
    size += 16 * desc_size;
    size_t pages = EFI_SIZE_TO_PAGES(size);
    uintptr_t buf = efi_glue_alloc_pages(pages, 0);
    if (buf == 0) {
        return top;
    }
    EFI_STATUS status = efi_bs->GetMemoryMap(
        &size, (EFI_MEMORY_DESCRIPTOR *)(uintptr_t)buf, &map_key, &desc_size, &desc_version
    );
    if (status == EFI_SUCCESS && desc_size >= sizeof(EFI_MEMORY_DESCRIPTOR)) {
        for (size_t off = 0; off + desc_size <= size; off += desc_size) {
            EFI_MEMORY_DESCRIPTOR *desc = (EFI_MEMORY_DESCRIPTOR *)(uintptr_t)(buf + off);
            uintptr_t end = desc->PhysicalStart + desc->NumberOfPages * 4096;
            if (end > top) {
                top = end;
            }
            if (ram_type(desc->Type)) {
                *ram += desc->NumberOfPages * 4096;
            }
        }
    }
    efi_bs->FreePages(buf, pages);
    return top;
}

// What an MC146818 can hold, with a century byte, of the firmware's time.
static void record_time(void) {
    EFI_TIME now;
    if (efi_rt->GetTime(&now, NULL) != EFI_SUCCESS) {
        return;
    }
    uint64_t tsc = rdtsc();
    if (now.Year < 1900 || now.Year > 2099 || now.Month < 1 || now.Month > 12 || now.Day < 1
     || now.Day > 31 || now.Hour > 23 || now.Minute > 59 || now.Second > 59) {
        return;
    }
    loader_handoff->time_year = now.Year;
    loader_handoff->time_month = now.Month;
    loader_handoff->time_day = now.Day;
    loader_handoff->time_hour = now.Hour;
    loader_handoff->time_minute = now.Minute;
    loader_handoff->time_second = now.Second;
    loader_handoff->time_tsc = tsc;
    loader_handoff->time_valid = true;
}

EFI_STATUS efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *system_table) {
    efi_image_handle = image_handle;
    efi_st = system_table;
    efi_bs = system_table->BootServices;
    efi_rt = system_table->RuntimeServices;

    efi_bs->SetWatchdogTimer(0, 0, 0, NULL);

    printf("ReBIOS\n");
    config_load();

    uintptr_t handoff_addr = efi_glue_alloc_pages(EFI_SIZE_TO_PAGES(sizeof(struct handoff)), 0);
    if (handoff_addr == 0) {
        panic("Cannot allocate the handoff\n");
    }
    loader_handoff = (struct handoff *)(uintptr_t)handoff_addr;
    memset(loader_handoff, 0, sizeof(struct handoff));
    reserve(handoff_addr, EFI_SIZE_TO_PAGES(sizeof(struct handoff)));

    video_find_framebuffer();
    console_init();
    bool serial_auto = config.serial && config.serial_port == 0;
    if (serial_auto) {
        config.serial_port = fwtables_serial_port();
    }
    // A serial card's port only answers once its I/O decoding is on.
    struct handoff_pci_address serial_card;
    bool serial_on_card = config.serial
        && pci_io_enable_io_port(config.serial_port, &serial_card);
    log_init(config.serial ? config.serial_port : 0, config.serial_baud, config.verbose);
    if (serial_auto && config.serial_port == 0) {
        printf("No serial port in the firmware's SPCR or DBG2 table\n");
    }
    if (serial_on_card) {
        printf(
            "Serial port %#x: on PCI " HANDOFF_PCI_ADDRESS_FMT "\n",
            config.serial_port, HANDOFF_PCI_ADDRESS_ARGS(serial_card)
        );
    }

    if (x86_cpu_vendor() == 0) {
        panic("ReBIOS runs on Intel's and AMD's processors alone\n");
    }
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
    reserve((uintptr_t)image->ImageBase, EFI_SIZE_TO_PAGES(image->ImageSize));

    uint64_t ram;
    uintptr_t map_top = memory_map_top(&ram);
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

    uint64_t nested_heap_size = align_down(ram / NESTED_HEAP_SHARE, EFI_PAGE_SIZE);
    if (nested_heap_size > NESTED_HEAP_MAX) {
        nested_heap_size = NESTED_HEAP_MAX;
    }
    size_t heap_size = CORE_HEAP_SIZE + nested_heap_size;
    uint64_t heap = loader_alloc_reserved(EFI_SIZE_TO_PAGES(heap_size), 0);
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
    loader_handoff->nested_heap_size = nested_heap_size;
    loader_handoff->tsc_hz = measure_tsc_hz();
    record_time();
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
