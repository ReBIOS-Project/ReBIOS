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
#include <lib/log.h>
#include <loader/cbfs.h>

#define FLASH_TOP 0x100000000ull
// x86 chipsets decode the top 16MiB below 4GiB for the boot flash, so reads
// there cannot reach device registers, unlike reads further down.
#define FLASH_WINDOW (16ull << 20)

#define MASTER_HEADER_POINTER (FLASH_TOP - 4)
#define MASTER_HEADER_MAGIC 0x4f524243
#define MASTER_HEADER_SIZE 32
#define MASTER_HEADER_ROMSIZE 8
#define MASTER_HEADER_ALIGN 16
#define MASTER_HEADER_OFFSET 20
#define DEFAULT_ALIGN 64

#define FILE_MAGIC "LARCHIVE"
#define FILE_MAGIC_SIZE 8
#define FILE_HEADER_SIZE 24
#define FILE_HEADER_LEN 8
#define FILE_HEADER_TYPE 12
#define FILE_HEADER_ATTRIBUTES 16
#define FILE_HEADER_DATA 20

#define FILE_TYPE_DELETED 0x00000000
#define FILE_TYPE_NULL 0xffffffff

#define ATTRIBUTE_HEADER_SIZE 8
#define ATTRIBUTE_TAG_UNUSED 0x00000000
#define ATTRIBUTE_TAG_UNUSED2 0xffffffff
#define ATTRIBUTE_TAG_COMPRESSION 0x42435a4c
#define COMPRESSION_ATTRIBUTE_SIZE 16

static uint8_t flash_read8(uint64_t addr) {
    return *(volatile const uint8_t *)(uintptr_t)addr;
}

static uint32_t flash_read_be32(uint64_t addr) {
    return (uint32_t)flash_read8(addr) << 24 | (uint32_t)flash_read8(addr + 1) << 16
           | (uint32_t)flash_read8(addr + 2) << 8 | flash_read8(addr + 3);
}

static bool file_magic_matches(uint64_t file) {
    const char *magic = FILE_MAGIC;
    for (int i = 0; i < FILE_MAGIC_SIZE; i++) {
        if (flash_read8(file + i) != (uint8_t)magic[i]) {
            return false;
        }
    }
    return true;
}

// The name must be terminated within the space it is given.
static bool name_matches(uint64_t addr, uint64_t space, const char *name) {
    for (uint64_t i = 0; i < space; i++) {
        uint8_t c = flash_read8(addr + i);
        if (c != (uint8_t)name[i]) {
            return false;
        }
        if (c == 0) {
            return true;
        }
    }
    return false;
}

// Attributes, each a big-endian tag and size, fill the metadata from the
// attributes offset to the data offset, padding being tagged 0 or all ones.
static bool file_compressed(uint64_t file, uint64_t attribute, uint64_t data) {
    if (attribute == 0) {
        return false;
    }

    while (attribute + ATTRIBUTE_HEADER_SIZE <= data) {
        uint32_t tag = flash_read_be32(file + attribute);
        uint64_t size = flash_read_be32(file + attribute + 4);
        if (tag == ATTRIBUTE_TAG_UNUSED || tag == ATTRIBUTE_TAG_UNUSED2
            || size < ATTRIBUTE_HEADER_SIZE || attribute + size > data) {
            break;
        }
        if (tag == ATTRIBUTE_TAG_COMPRESSION && size >= COMPRESSION_ATTRIBUTE_SIZE) {
            return flash_read_be32(file + attribute + 8) != 0;
        }
        attribute += size;
    }
    return false;
}

bool cbfs_find_file(const char *name, const void **data, uint32_t *size) {
    uint32_t pointer = *(volatile const uint32_t *)(uintptr_t)MASTER_HEADER_POINTER;
    if (pointer % 4 != 0) {
        return false;
    }

    // Whether the pointer is the header's address or its negative offset from
    // 4GiB, sign-extending it and adding 4GiB yields the address.
    uint64_t header = FLASH_TOP + (uint64_t)(int64_t)(int32_t)pointer;
    if (header < FLASH_TOP - FLASH_WINDOW || header > FLASH_TOP - MASTER_HEADER_SIZE
        || flash_read_be32(header) != MASTER_HEADER_MAGIC) {
        return false;
    }

    uint64_t rom_size = flash_read_be32(header + MASTER_HEADER_ROMSIZE);
    uint64_t align = flash_read_be32(header + MASTER_HEADER_ALIGN);
    uint64_t offset = flash_read_be32(header + MASTER_HEADER_OFFSET);
    uint64_t rom = FLASH_TOP - rom_size;
    if (align == 0) {
        align = DEFAULT_ALIGN;
    }
    if (header < rom || offset >= rom_size || rom + offset < FLASH_TOP - FLASH_WINDOW) {
        printf("Ignoring the unusable CBFS master header at %#llx\n", (unsigned long long)header);
        return false;
    }

    uint64_t file = rom + offset;
    while (file + FILE_HEADER_SIZE <= FLASH_TOP && file_magic_matches(file)) {
        uint64_t len = flash_read_be32(file + FILE_HEADER_LEN);
        uint32_t type = flash_read_be32(file + FILE_HEADER_TYPE);
        uint64_t attributes = flash_read_be32(file + FILE_HEADER_ATTRIBUTES);
        uint64_t data_offset = flash_read_be32(file + FILE_HEADER_DATA);
        uint64_t name_end = attributes != 0 ? attributes : data_offset;

        // Data overlapping the header would also keep the walk from advancing.
        if (data_offset < FILE_HEADER_SIZE || name_end < FILE_HEADER_SIZE
            || name_end > data_offset || file + data_offset + len > FLASH_TOP) {
            break;
        }

        if (type != FILE_TYPE_DELETED && type != FILE_TYPE_NULL
            && name_matches(file + FILE_HEADER_SIZE, name_end - FILE_HEADER_SIZE, name)) {
            if (file_compressed(file, attributes, data_offset)) {
                printf("The CBFS file %s is compressed, which is not supported\n", name);
                return false;
            }
            *data = (const void *)(uintptr_t)(file + data_offset);
            *size = (uint32_t)len;
            return true;
        }

        uint64_t end = file + data_offset + len - rom;
        file = rom + (end + align - 1) / align * align;
    }

    return false;
}
