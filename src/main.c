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
#include <loader/last_log.h>
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
// The rounds the TSC is timed in against the firmware's delay service.
#define STALL_ROUND_US 10000
#define STALL_ROUNDS 3
// The memory map buffer's room for descriptors beyond the map it is sized for.
#define MAP_SLACK_BYTES 4096

static struct efi_guid loaded_image_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;

// Timed against the firmware's delay service, whatever the TSC's frequency
// enumeration says, which not every processor provides: the shortest of a few
// rounds, since anything that stops the processor during one can only lengthen
// it.
static uint64_t measure_tsc_hz(void) {
    uint64_t shortest = UINT64_MAX;
    for (size_t i = 0; i < STALL_ROUNDS; i++) {
        uint64_t start = rdtsc();
        efi_bs->stall(STALL_ROUND_US);
        uint64_t ticks = rdtsc() - start;
        if (ticks < shortest) {
            shortest = ticks;
        }
    }
    return shortest * (1000000 / STALL_ROUND_US);
}

// The memory map buffer must exist before the map that goes into it is taken,
// so it gets slack for the descriptors its own allocation adds, and for those
// that appear before the map key can be used: once ExitBootServices() has been
// called, GetMemoryMap() is the only other service left to call (UEFI 2.11,
// 7.4), so the buffer is never replaced after that, as Linux 7.3's stub keeps
// it (efi_exit_boot_services()). It is only recorded as reserved once final,
// so that it is not counted twice.
static bool exit_boot_services(void) {
    size_t map_key;
    size_t desc_size;
    uint32_t desc_version;
    uintptr_t buf = 0;
    size_t buf_size = 0;
    bool called = false;

    // Room for the buffer's own entry.
    if (loader_handoff->reserved_count == HANDOFF_MAX_RESERVED) {
        return false;
    }

    for (size_t attempt = 0; attempt < 16; attempt++) {
        size_t size = buf_size;
        efi_status status = efi_bs->get_memory_map(
            &size, (struct efi_memory_descriptor *)(uintptr_t)buf, &map_key, &desc_size,
            &desc_version
        );
        if (status == EFI_BUFFER_TOO_SMALL && !called) {
            if (buf != 0) {
                efi_bs->free_pages(buf, EFI_SIZE_TO_PAGES(buf_size));
            }
            buf_size = align_up(size + MAP_SLACK_BYTES, EFI_PAGE_SIZE);
            buf = efi_glue_alloc_pages(EFI_SIZE_TO_PAGES(buf_size), 0);
            if (buf == 0) {
                return false;
            }
            continue;
        }
        if (status != EFI_SUCCESS || desc_size < sizeof(struct efi_memory_descriptor)) {
            return false;
        }

        called = true;
        if (efi_bs->exit_boot_services(efi_image_handle, map_key) == EFI_SUCCESS) {
            efi_glue_boot_services_exited = true;
            loader_reserve(buf, EFI_SIZE_TO_PAGES(buf_size));
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
    return type == EFI_LOADER_CODE || type == EFI_LOADER_DATA || type == EFI_BOOT_SERVICES_CODE
        || type == EFI_BOOT_SERVICES_DATA || type == EFI_CONVENTIONAL_MEMORY;
}

// The top of the memory map, 4 GiB at least, and the RAM in it.
static uintptr_t memory_map_top(uint64_t *ram) {
    size_t size = 0;
    size_t map_key;
    size_t desc_size;
    uint32_t desc_version;
    uintptr_t top = FOUR_GIB;

    *ram = 0;
    efi_bs->get_memory_map(&size, NULL, &map_key, &desc_size, &desc_version);
    size += 16 * desc_size;
    size_t pages = EFI_SIZE_TO_PAGES(size);
    uintptr_t buf = efi_glue_alloc_pages(pages, 0);
    if (buf == 0) {
        return top;
    }
    efi_status status = efi_bs->get_memory_map(
        &size, (struct efi_memory_descriptor *)(uintptr_t)buf, &map_key, &desc_size, &desc_version
    );
    if (status == EFI_SUCCESS && desc_size >= sizeof(struct efi_memory_descriptor)) {
        for (size_t off = 0; off + desc_size <= size; off += desc_size) {
            struct efi_memory_descriptor *desc;
            desc = (struct efi_memory_descriptor *)(uintptr_t)(buf + off);
            uintptr_t end = desc->physical_start + desc->number_of_pages * EFI_PAGE_SIZE;
            if (end > top) {
                top = end;
            }
            if (ram_type(desc->type)) {
                *ram += desc->number_of_pages * EFI_PAGE_SIZE;
            }
        }
    }
    efi_bs->free_pages(buf, pages);
    return top;
}

// What an MC146818 can hold, with a century byte, of the firmware's time.
static void record_time(void) {
    struct efi_time now;
    if (efi_rt->get_time(&now, NULL) != EFI_SUCCESS) {
        return;
    }
    uint64_t tsc = rdtsc();
    if (now.year < 1900 || now.year > 2099 || now.month < 1 || now.month > 12 || now.day < 1
     || now.day > 31 || now.hour > 23 || now.minute > 59 || now.second > 59) {
        return;
    }
    loader_handoff->time_year = now.year;
    loader_handoff->time_month = now.month;
    loader_handoff->time_day = now.day;
    loader_handoff->time_hour = now.hour;
    loader_handoff->time_minute = now.minute;
    loader_handoff->time_second = now.second;
    loader_handoff->time_tsc = tsc;
    loader_handoff->time_valid = true;
}

efi_status efi_main(efi_handle image_handle, struct efi_system_table *system_table) {
    efi_image_handle = image_handle;
    efi_st = system_table;
    efi_bs = system_table->boot_services;
    efi_rt = system_table->runtime_services;

    efi_bs->set_watchdog_timer(0, 0, 0, NULL);

    printf("ReBIOS\n");
    config_load();

    uintptr_t handoff_addr = efi_glue_alloc_pages(EFI_SIZE_TO_PAGES(sizeof(struct handoff)), 0);
    if (handoff_addr == 0) {
        panic("Cannot allocate the handoff\n");
    }
    loader_handoff = (struct handoff *)(uintptr_t)handoff_addr;
    memset(loader_handoff, 0, sizeof(struct handoff));
    loader_reserve(handoff_addr, EFI_SIZE_TO_PAGES(sizeof(struct handoff)));

    video_find_framebuffer();
    console_init();
    bool serial_auto = config.serial && config.serial_port == 0;
    uint32_t serial_clock = 0;
    if (serial_auto) {
        config.serial_port = fwtables_serial_port(&serial_clock);
    }
    // A serial card's port only answers once its I/O decoding is on.
    struct handoff_pci_address serial_card;
    bool serial_on_card = config.serial && pci_io_enable_io_port(config.serial_port, &serial_card);
    log_init(
        config.serial ? config.serial_port : 0, config.serial_baud, serial_clock, config.verbose
    );
    if (serial_auto && config.serial_port == 0) {
        printf("No serial port in the firmware's SPCR or DBG2 table\n");
    }
    if (serial_on_card) {
        printf(
            "Serial port %#x: on PCI " HANDOFF_PCI_ADDRESS_FMT "\n",
            config.serial_port, HANDOFF_PCI_ADDRESS_ARGS(serial_card)
        );
    }
    last_log_write(config.save_log);

    if (x86_cpu_vendor() == 0) {
        panic("ReBIOS runs on Intel's and AMD's processors alone\n");
    }
    if (!iommu_check()) {
        return EFI_UNSUPPORTED;
    }

    // The core runs from this image, which stays where the firmware put it.
    struct efi_loaded_image_protocol *image;
    efi_status status = efi_bs->handle_protocol(image_handle, &loaded_image_guid, (void **)&image);
    if (status != EFI_SUCCESS) {
        panic("Cannot find where ReBIOS is loaded\n");
    }
    loader_reserve((uintptr_t)image->image_base, EFI_SIZE_TO_PAGES(image->image_size));

    uint64_t ram;
    uintptr_t map_top = memory_map_top(&ram);
    size_t table_pages;
    uintptr_t pml4 = paging_build_identity_map(&map_top, &table_pages);
    if (pml4 == 0) {
        panic("Cannot build the core's page tables\n");
    }
    loader_reserve(pml4, table_pages);

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
