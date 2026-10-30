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
#include <loader/pci_rom.h>

// Offsets into an image's ROM header.
#define ROM_HEADER_PCIR 0x18
#define ROM_HEADER_SIZE 0x1a

// Offsets into the PCI data structure.
#define PCIR_VENDOR_ID 0x04
#define PCIR_DEVICE_ID 0x06
#define PCIR_DEVICE_LIST 0x08
#define PCIR_REVISION 0x0c
#define PCIR_IMAGE_LENGTH 0x10
#define PCIR_CODE_TYPE 0x14
#define PCIR_INDICATOR 0x15
// The revision 0 structure, which later revisions only extend.
#define PCIR_SIZE 0x18

#define PCIR_INDICATOR_LAST 0x80
#define CODE_TYPE_PC_AT 0x00
#define IMAGE_LENGTH_UNIT 512

static uint16_t read16(const uint8_t *p) {
    return (uint16_t)(p[0] | p[1] << 8);
}

// Looks for the device in the 0-terminated list of device IDs that a PCI data
// structure of revision 3 or later may point to, which must lie in the image.
static bool device_listed(
    const uint8_t *image, size_t image_size, uint64_t pcir, uint16_t device_id
) {
    uint16_t list = read16(image + pcir + PCIR_DEVICE_LIST);
    if (list == 0) {
        return false;
    }

    for (size_t off = pcir + list; off + 2 <= image_size; off += 2) {
        uint16_t id = read16(image + off);
        if (id == 0) {
            return false;
        }
        if (id == device_id) {
            return true;
        }
    }
    return false;
}

bool pci_rom_find_legacy_image(
    const void *rom, size_t rom_size, uint16_t vendor_id, uint16_t device_id,
    const void **image, size_t *size
) {
    const uint8_t *found = NULL;
    size_t found_size = 0;
    bool found_revision_3 = false;

    if (rom == NULL) {
        return false;
    }

    for (size_t off = 0; rom_size - off >= ROM_HEADER_SIZE; ) {
        const uint8_t *img = (const uint8_t *)rom + off;
        uint64_t left = rom_size - off;

        if (img[0] != 0x55 || img[1] != 0xaa) {
            break;
        }

        uint64_t pcir = read16(img + ROM_HEADER_PCIR);
        if (pcir + PCIR_SIZE > left) {
            break;
        }
        const uint8_t *data = img + pcir;
        if (data[0] != 'P' || data[1] != 'C' || data[2] != 'I'
         || data[3] != 'R') {
            break;
        }

        // The image must hold its own headers, so that it is never empty, and
        // fit in the ROM.
        size_t img_size = (uint64_t)read16(data + PCIR_IMAGE_LENGTH)
            * IMAGE_LENGTH_UNIT;
        if (img_size < pcir + PCIR_SIZE || img_size > left) {
            break;
        }

        bool revision_3 = data[PCIR_REVISION] >= 3;
        if (data[PCIR_CODE_TYPE] == CODE_TYPE_PC_AT
         && read16(data + PCIR_VENDOR_ID) == vendor_id
         && (read16(data + PCIR_DEVICE_ID) == device_id
          || (revision_3 && device_listed(img, img_size, pcir, device_id)))
         && (found == NULL || (revision_3 && !found_revision_3))) {
            found = img;
            found_size = (uint32_t)img_size;
            found_revision_3 = revision_3;
        }

        if ((data[PCIR_INDICATOR] & PCIR_INDICATOR_LAST) != 0) {
            break;
        }
        off += img_size;
    }

    if (found == NULL) {
        return false;
    }
    *image = found;
    *size = found_size;
    return true;
}
