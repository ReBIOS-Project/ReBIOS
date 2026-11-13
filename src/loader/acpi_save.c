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
#include <uacpi/acpi.h>
#include <loader/acpi_save.h>
#include <handoff.h>
#include <hw/acpi.h>
#include <lib/log.h>
#include <lib/printf.h>
#include <lib/util.h>
#include <loader/efi_glue.h>
#include <loader/files.h>
#include <loader/fwtables.h>
#include <loader/loader.h>
#include <memory.h>

#define HOST_FILE "rebios-acpi.txt"
#define GUEST_FILE "rebios-guest-acpi.txt"

// Where the core keeps the guest's tables: the middle of the largest free
// memory from 16 MiB up to 4 GiB, away from both the regions that firmware
// uses at fixed low addresses while it starts, such as OVMF's decompression
// scratch, and what it hands out from the top down, so that it is still there
// for the next start to find.
#define AREA_FLOOR ((uint64_t)0x1000000)
#define AREA_CEILING ((uint64_t)0x100000000)

// Far more than any table, or machine, has, so that a corrupt length is
// refused.
#define TABLE_MAX ((uint32_t)0x1000000)
#define TABLES_MAX 256

// acpidump's text, which acpixtract reads: a line naming each table and its
// address, a line for each 16 bytes with their offset, the bytes in hex, and
// the bytes as text, then an empty line (Linux 7.3's acpidump, apdump.c's
// ap_dump_table_buffer(), and ACPICA's acpi_ut_dump_buffer_to_file()).
#define LINE_BYTES 16
#define HEADER_CHARS 26
#define OFFSET_CHARS 8
#define LINE_CHARS (OFFSET_CHARS + 2 + LINE_BYTES * 3 + 1 + LINE_BYTES + 1)

struct table {
    uint64_t address;
    const uint8_t *bytes;
    uint32_t length;
};

static struct table tables[TABLES_MAX];
static size_t table_count;

// Adds a table, but for one at an address added before.
static void add(uint64_t address, const uint8_t *bytes, uint32_t length) {
    if (address == 0 || table_count == TABLES_MAX) {
        return;
    }
    for (size_t i = 0; i < table_count; i++) {
        if (tables[i].address == address) {
            return;
        }
    }
    tables[table_count++] = (struct table){ .address = address, .bytes = bytes, .length = length };
}

static char *put_hex(char *p, uint64_t value, size_t digits) {
    static const char hex[] = "0123456789ABCDEF";
    for (size_t i = 0; i < digits; i++) {
        p[i] = hex[(value >> (4 * (digits - 1 - i))) & 0xf];
    }
    return p + digits;
}

static char text_char(uint8_t c, char other) {
    return c >= 0x20 && c <= 0x7e ? (char)c : other;
}

// Puts a table's text at p, under its first 4 bytes, as acpidump prints a
// table's signature, the RSDP's "RSD " too; returns where it ends.
static char *dump_table(char *p, const struct table *t) {
    for (size_t i = 0; i < 4; i++) {
        *p++ = i < t->length ? text_char(t->bytes[i], '?') : ' ';
    }
    memcpy(p, " @ 0x", 5);
    p = put_hex(p + 5, t->address, 16);
    *p++ = '\n';
    for (size_t at = 0; at < t->length; at += LINE_BYTES) {
        // At least 4 digits, right-aligned in 8 columns.
        size_t digits = 4;
        while (digits < OFFSET_CHARS && (at >> (4 * digits)) != 0) {
            digits++;
        }
        memset(p, ' ', OFFSET_CHARS - digits);
        p = put_hex(p + OFFSET_CHARS - digits, at, digits);
        *p++ = ':';
        *p++ = ' ';
        size_t n = t->length - at < LINE_BYTES ? t->length - at : LINE_BYTES;
        for (size_t i = 0; i < LINE_BYTES; i++) {
            if (i < n) {
                p = put_hex(p, t->bytes[at + i], 2);
                *p++ = ' ';
            } else {
                memset(p, ' ', 3);
                p += 3;
            }
        }
        *p++ = ' ';
        for (size_t i = 0; i < n; i++) {
            *p++ = text_char(t->bytes[at + i], '.');
        }
        *p++ = '\n';
    }
    *p++ = '\n';
    return p;
}

// Writes the tables added to the file name, what saying what they are.
static void write_tables(const char *name, const char *what) {
    size_t size = 0;
    for (size_t i = 0; i < table_count; i++) {
        size += HEADER_CHARS + (tables[i].length + LINE_BYTES - 1) / LINE_BYTES * LINE_CHARS + 1;
    }
    char *text = efi_glue_alloc_pool(size);
    if (text == NULL) {
        log_notice("No memory to write %s to %s\n", what, name);
        return;
    }
    char *end = text;
    for (size_t i = 0; i < table_count; i++) {
        end = dump_table(end, &tables[i]);
    }
    char where[256];
    snprintf(where, sizeof(where), "%s", name);
    efi_status status;
    const char *why = files_write_beside(
        name, text, (size_t)(end - text), where, sizeof(where), &status
    );
    if (why != NULL) {
        log_notice(
            "Cannot write %s to %s: %s, status %#llx\n", what, where, why,
            (unsigned long long)status
        );
    } else {
        log_notice("Wrote %s, %zu of them, to %s\n", what, table_count, where);
    }
    efi_bs->free_pool(text);
}

// The length of the firmware's table at address, or 0 for none that it could
// be.
static uint32_t host_length(uint64_t address) {
    if (address == 0) {
        return 0;
    }
    uint32_t length = read_le32((const uint8_t *)(uintptr_t)address + ACPI_SDT_LENGTH);
    return length >= ACPI_SDT_HEADER_SIZE && length <= TABLE_MAX ? length : 0;
}

// Adds the firmware's table at address, and the DSDT and FACS of a FADT.
static void add_host(uint64_t address) {
    uint32_t length = host_length(address);
    if (length == 0) {
        return;
    }
    const uint8_t *table = (const uint8_t *)(uintptr_t)address;
    add(address, table, length);
    if (memcmp(table, ACPI_FADT_SIGNATURE, 4) != 0) {
        return;
    }
    struct acpi_fadt fadt;
    memset(&fadt, 0, sizeof(struct acpi_fadt));
    memcpy(&fadt, table, length < sizeof(struct acpi_fadt) ? length : sizeof(struct acpi_fadt));
    add_host(fadt.x_dsdt);
    add_host(fadt.dsdt);
    add_host(fadt.x_firmware_ctrl);
    add_host(fadt.firmware_ctrl);
}

// Adds an XSDT or RSDT, and the tables it lists.
static void add_root(uint64_t address, const char *signature, size_t entry_size) {
    uint32_t length = host_length(address);
    if (length == 0) {
        return;
    }
    const uint8_t *root = (const uint8_t *)(uintptr_t)address;
    add(address, root, length);
    if (memcmp(root, signature, 4) != 0) {
        return;
    }
    for (size_t off = ACPI_SDT_HEADER_SIZE; off + entry_size <= length; off += entry_size) {
        add_host(entry_size == sizeof(uint64_t) ? read_le64(root + off) : read_le32(root + off));
    }
}

static void write_host(void) {
    table_count = 0;
    const struct acpi_rsdp *rsdp = (const struct acpi_rsdp *)fwtables_acpi_rsdp();
    if (rsdp == NULL) {
        log_notice("No ACPI tables of the firmware's to write to %s\n", HOST_FILE);
        return;
    }
    uint32_t rsdp_length = rsdp->revision >= 2 ? rsdp->length : (uint32_t)ACPI_RSDP_V1_SIZE;
    add((uintptr_t)rsdp, (const uint8_t *)rsdp, rsdp_length);
    if (rsdp->revision >= 2) {
        add_root(rsdp->xsdt_addr, ACPI_XSDT_SIGNATURE, sizeof(uint64_t));
    }
    add_root(rsdp->rsdt_addr, ACPI_RSDT_SIGNATURE, sizeof(uint32_t));
    write_tables(HOST_FILE, "the firmware's ACPI tables");
}

// The firmware's memory map, in a pool allocation, or NULL.
static uint8_t *memory_map(size_t *size, size_t *desc_size) {
    size_t map_key;
    uint32_t desc_version;
    *size = 0;
    *desc_size = 0;
    efi_bs->get_memory_map(size, NULL, &map_key, desc_size, &desc_version);
    // Room for the descriptors that the allocation itself may add.
    *size += 16 * *desc_size;
    uint8_t *map = efi_glue_alloc_pool(*size);
    if (map == NULL) {
        return NULL;
    }
    efi_status status = efi_bs->get_memory_map(
        size, (struct efi_memory_descriptor *)map, &map_key, desc_size, &desc_version
    );
    if (status != EFI_SUCCESS || *desc_size < sizeof(struct efi_memory_descriptor)) {
        efi_bs->free_pool(map);
        return NULL;
    }
    return map;
}

// The pages of a descriptor's free memory from AREA_FLOOR up to AREA_CEILING:
// false where it has none.
static bool free_part(const struct efi_memory_descriptor *d, uint64_t *base, uint64_t *end) {
    if (d->type != EFI_CONVENTIONAL_MEMORY) {
        return false;
    }
    uint64_t top = d->physical_start + d->number_of_pages * EFI_PAGE_SIZE;
    *base = page_up(d->physical_start > AREA_FLOOR ? d->physical_start : AREA_FLOOR);
    *end = top < AREA_CEILING ? top : AREA_CEILING;
    return *base < *end;
}

// The tables of the copy that the core kept, records of count tables over size
// bytes: false where they do not fit in it.
static bool add_kept(const uint8_t *records, uint64_t size, uint32_t count) {
    table_count = 0;
    uint64_t at = 0;
    for (size_t i = 0; i < count; i++) {
        struct handoff_acpi_save_table record;
        if (size - at < sizeof(struct handoff_acpi_save_table)) {
            return false;
        }
        memcpy(&record, records + at, sizeof(struct handoff_acpi_save_table));
        at += sizeof(struct handoff_acpi_save_table);
        if (align_up(record.length, 8) > size - at) {
            return false;
        }
        add(record.address, records + at, record.length);
        at += align_up(record.length, 8);
    }
    return true;
}

// Writes out the copy that the core kept at page, if there is one there, in
// free memory up to end: false where there is none.
static bool write_kept(uintptr_t page, uint64_t end) {
    const struct handoff_acpi_save *head = (const struct handoff_acpi_save *)(uintptr_t)page;
    if (memcmp(head->magic, HANDOFF_ACPI_SAVE_MAGIC, sizeof(head->magic)) != 0) {
        return false;
    }
    uint64_t size = head->size;
    uint64_t head_size = sizeof(struct handoff_acpi_save);
    if (size > HANDOFF_ACPI_SAVE_SIZE - head_size || size > end - page - head_size) {
        return false;
    }
    // The pages are the loader's while it reads them, so that writing the file
    // cannot take them.
    size_t pages = page_count(head_size + size);
    efi_physical_address taken = page;
    efi_status status = efi_bs->allocate_pages(
        EFI_ALLOCATE_ADDRESS, EFI_LOADER_DATA, pages, &taken
    );
    if (status != EFI_SUCCESS) {
        return false;
    }
    const uint8_t *records = (const uint8_t *)(head + 1);
    if (fnv1a64(records, size) == head->hash && add_kept(records, size, head->count)) {
        write_tables(GUEST_FILE, "the ACPI tables that ReBIOS gave the OS last time");
    } else {
        log_notice(
            "The ACPI tables that ReBIOS gave the OS last time are damaged, and not written to "
            "%s\n", GUEST_FILE
        );
    }
    // Once is enough.
    memset((void *)(uintptr_t)page, 0, sizeof(head->magic));
    efi_bs->free_pages(page, pages);
    return true;
}

static void write_guest(void) {
    size_t size;
    size_t desc_size;
    uint8_t *map = memory_map(&size, &desc_size);
    if (map == NULL) {
        return;
    }
    bool found = false;
    for (size_t off = 0; off + desc_size <= size && !found; off += desc_size) {
        uint64_t base;
        uint64_t end;
        if (!free_part((const struct efi_memory_descriptor *)(map + off), &base, &end)) {
            continue;
        }
        for (uintptr_t page = base; page + EFI_PAGE_SIZE <= end && !found; page += EFI_PAGE_SIZE) {
            found = write_kept(page, end);
        }
    }
    efi_bs->free_pool(map);
    if (!found) {
        log_notice(
            "No ACPI tables from ReBIOS's last start for %s: save_acpi was off then, or the "
            "machine was turned off since\n", GUEST_FILE
        );
    }
}

static void reserve_area(void) {
    size_t size;
    size_t desc_size;
    uint8_t *map = memory_map(&size, &desc_size);
    if (map == NULL) {
        return;
    }
    uint64_t largest = 0;
    uint64_t largest_size = 0;
    for (size_t off = 0; off + desc_size <= size; off += desc_size) {
        uint64_t base;
        uint64_t end;
        if (free_part((const struct efi_memory_descriptor *)(map + off), &base, &end)
         && end - base > largest_size) {
            largest = base;
            largest_size = end - base;
        }
    }
    efi_bs->free_pool(map);
    uint64_t area = 0;
    if (largest_size >= HANDOFF_ACPI_SAVE_SIZE) {
        area = largest + align_down((largest_size - HANDOFF_ACPI_SAVE_SIZE) / 2, EFI_PAGE_SIZE);
    }
    size_t pages = page_count(HANDOFF_ACPI_SAVE_SIZE);
    efi_physical_address taken = area;
    efi_status status = EFI_NOT_FOUND;
    if (area != 0) {
        status = efi_bs->allocate_pages(EFI_ALLOCATE_ADDRESS, EFI_LOADER_DATA, pages, &taken);
    }
    if (status != EFI_SUCCESS) {
        log_notice("No memory to keep the ACPI tables that ReBIOS gives the OS in\n");
        return;
    }
    loader_reserve(area, pages);
    memset((void *)(uintptr_t)area, 0, sizeof(struct handoff_acpi_save));
    loader_handoff->acpi_save = area;
    loader_handoff->acpi_save_size = HANDOFF_ACPI_SAVE_SIZE;
}

void acpi_save_start(void) {
    write_guest();
    write_host();
    reserve_area();
}
