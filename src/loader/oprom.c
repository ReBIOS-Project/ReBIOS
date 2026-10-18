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
#include <stddef.h>
#include <stdbool.h>
#include <efi.h>
#include <memory.h>
#include <handoff.h>
#include <lib/log.h>
#include <loader/efi_glue.h>
#include <loader/loader.h>
#include <loader/oprom.h>
#include <loader/pci_io.h>
#include <loader/pci_rom.h>

struct found_rom {
    const struct pci_io_device *dev;
    const void *image;
    uint32_t size;
};

static uint64_t page_count(uint64_t size) {
    return (size + 4095) / 4096;
}

static void sort_by_address(struct found_rom *roms, size_t count) {
    for (size_t i = 1; i < count; i++) {
        struct found_rom rom = roms[i];
        size_t j = i;
        for (; j > 0 && pci_address_compare(roms[j - 1].dev->address, rom.dev->address) > 0; j--) {
            roms[j] = roms[j - 1];
        }
        roms[j] = rom;
    }
}

void oprom_collect(void) {
    struct handoff_video *video = &loader_handoff->video;
    EFI_UINTN dev_count;
    struct found_rom *roms;
    size_t count = 0;

    loader_handoff->pci_rom_count = 0;
    memset(loader_handoff->pci_roms, 0, sizeof(loader_handoff->pci_roms));

    struct pci_io_device *devices = pci_io_devices(&dev_count);
    if (devices == NULL) {
        return;
    }
    if (efi_bs->AllocatePool(EfiLoaderData, dev_count * sizeof(*roms),
                             (void **)&roms) != EFI_SUCCESS) {
        printf("Cannot allocate memory to list the option ROMs\n");
        efi_bs->FreePool(devices);
        return;
    }

    for (EFI_UINTN i = 0; i < dev_count; i++) {
        const struct pci_io_device *dev = &devices[i];
        if (dev->io->RomImage == NULL || dev->io->RomSize == 0
            || dev->base_class == PCI_CLASS_DISPLAY
            || (video->vga_present && pci_address_equal(dev->address, video->vga_device))) {
            continue;
        }
        if (pci_rom_find_legacy_image(dev->io->RomImage, dev->io->RomSize, dev->vendor_id,
                                      dev->device_id, &roms[count].image, &roms[count].size)) {
            roms[count].dev = dev;
            count++;
        }
    }

    // Sorted before any are dropped, so that neither depends on the
    // firmware's handle order.
    sort_by_address(roms, count);
    for (size_t i = HANDOFF_MAX_PCI_ROMS; i < count; i++) {
        printf("Too many option ROMs, skipping that of " PCI_ADDRESS_FMT " (%04x:%04x)\n",
               PCI_ADDRESS_ARGS(roms[i].dev->address), roms[i].dev->vendor_id,
               roms[i].dev->device_id);
    }
    if (count > HANDOFF_MAX_PCI_ROMS) {
        count = HANDOFF_MAX_PCI_ROMS;
    }

    // One allocation for all of them, as the hypervisor's memory ranges are
    // few.
    uint64_t pages = 0;
    for (size_t i = 0; i < count; i++) {
        pages += page_count(roms[i].size);
    }
    uint64_t at = count > 0 ? loader_alloc_reserved(pages, 0) : 0;
    if (count > 0 && at == 0) {
        printf("Cannot allocate %llu pages for the option ROMs\n", (unsigned long long)pages);
        count = 0;
    }

    for (size_t i = 0; i < count; i++) {
        const struct pci_io_device *dev = roms[i].dev;
        struct handoff_pci_rom *rom = &loader_handoff->pci_roms[i];
        uint64_t space = page_count(roms[i].size) * 4096;

        memcpy((void *)(uintptr_t)at, roms[i].image, roms[i].size);
        memset((uint8_t *)(uintptr_t)at + roms[i].size, 0, space - roms[i].size);

        rom->address = dev->address;
        rom->vendor_id = dev->vendor_id;
        rom->device_id = dev->device_id;
        rom->base_class = dev->base_class;
        rom->subclass = dev->subclass;
        rom->prog_if = dev->prog_if;
        rom->size = roms[i].size;
        rom->image = at;

        printf("Option ROM of " PCI_ADDRESS_FMT " (%04x:%04x), %u bytes\n",
               PCI_ADDRESS_ARGS(dev->address), dev->vendor_id, dev->device_id, roms[i].size);
        at += space;
    }
    loader_handoff->pci_rom_count = (uint32_t)count;

    efi_bs->FreePool(roms);
    efi_bs->FreePool(devices);
}
