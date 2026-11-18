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
// You should have received a copy of the GNU General Public License along with
// ReBIOS. If not, see <https://www.gnu.org/licenses/>.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <uacpi/acpi.h>
#include <virt/acpi_save.h>
#include <core.h>
#include <handoff.h>
#include <hw/acpi.h>
#include <lib/log.h>
#include <lib/util.h>
#include <memory.h>
#include <sys/cache.h>
#include <virt/guest_mem.h>

// Far more than any table the guest gets, so that a corrupt length is refused.
#define TABLE_MAX ((uint32_t)0x1000000)
#define TABLES_MAX 256
// Far beyond any revision's RSDP.
#define RSDP_MAX_SIZE 4096

static uint8_t *records;
static uint64_t room;
static uint64_t used;
static uint32_t count;
static uint64_t kept[TABLES_MAX];
static bool left_out;

// Copies length bytes of the guest's table at address, but for one copied
// before.
static void keep(uint64_t address, uint32_t length) {
    for (size_t i = 0; i < count; i++) {
        if (kept[i] == address) {
            return;
        }
    }
    struct handoff_acpi_save_table record = { .address = address, .length = length };
    size_t record_size = sizeof(struct handoff_acpi_save_table);
    uint64_t size = record_size + align_up(length, 8);
    if (count == TABLES_MAX || size > room - used) {
        left_out = true;
        return;
    }
    uint8_t *at = records + used;
    if (!guest_mem_phys_read(address, at + record_size, length)) {
        return;
    }
    memcpy(record.signature, at + record_size, sizeof(record.signature));
    memcpy(at, &record, record_size);
    memset(at + record_size + length, 0, align_up(length, 8) - length);
    used += size;
    kept[count++] = address;
}

// The header of the guest's table at address, and its length, or 0 for none
// that it could be.
static uint32_t guest_header(uint64_t address, struct acpi_sdt_hdr *header) {
    if (address == 0 || !guest_mem_phys_read(address, header, sizeof(struct acpi_sdt_hdr))) {
        return 0;
    }
    uint32_t length = header->length;
    return length >= ACPI_SDT_HEADER_SIZE && length <= TABLE_MAX ? length : 0;
}

// Keeps the guest's table at address, and the DSDT and FACS of a FADT.
static void keep_table(uint64_t address) {
    struct acpi_sdt_hdr header;
    uint32_t length = guest_header(address, &header);
    if (length == 0) {
        return;
    }
    keep(address, length);
    if (memcmp(header.signature, ACPI_FADT_SIGNATURE, sizeof(header.signature)) != 0) {
        return;
    }
    struct acpi_fadt fadt;
    memset(&fadt, 0, sizeof(struct acpi_fadt));
    size_t fadt_size = length < sizeof(struct acpi_fadt) ? length : sizeof(struct acpi_fadt);
    if (!guest_mem_phys_read(address, &fadt, fadt_size)) {
        return;
    }
    keep_table(fadt.x_dsdt);
    keep_table(fadt.dsdt);
    keep_table(fadt.x_firmware_ctrl);
    keep_table(fadt.firmware_ctrl);
}

// Keeps an XSDT or RSDT of the guest's, and the tables it lists.
static void keep_root(uint64_t address, const char *signature, size_t entry_size) {
    struct acpi_sdt_hdr header;
    uint32_t length = guest_header(address, &header);
    if (length == 0) {
        return;
    }
    keep(address, length);
    if (memcmp(header.signature, signature, sizeof(header.signature)) != 0) {
        return;
    }
    for (size_t off = ACPI_SDT_HEADER_SIZE; off + entry_size <= length; off += entry_size) {
        uint64_t entry = 0;
        if (!guest_mem_phys_read(address + off, &entry, entry_size)) {
            return;
        }
        keep_table(entry);
    }
}

void acpi_save_guest(uint32_t rsdp) {
    const struct handoff *h = core_handoff;
    if (h->acpi_save == 0 || h->acpi_save_size <= sizeof(struct handoff_acpi_save)) {
        return;
    }
    struct handoff_acpi_save *head = (struct handoff_acpi_save *)(uintptr_t)h->acpi_save;
    records = (uint8_t *)(head + 1);
    room = h->acpi_save_size - sizeof(struct handoff_acpi_save);
    struct acpi_rsdp r;
    if (!guest_mem_phys_read(rsdp, &r, sizeof(struct acpi_rsdp))) {
        return;
    }
    uint32_t length = (uint32_t)ACPI_RSDP_V1_SIZE;
    if (r.revision >= 2 && r.length >= sizeof(struct acpi_rsdp) && r.length <= RSDP_MAX_SIZE) {
        length = r.length;
    }
    keep(rsdp, length);
    if (r.revision >= 2) {
        keep_root(r.xsdt_addr, ACPI_XSDT_SIGNATURE, sizeof(uint64_t));
    }
    keep_root(r.rsdt_addr, ACPI_RSDT_SIGNATURE, sizeof(uint32_t));

    head->size = used;
    head->hash = fnv1a64(records, used);
    head->count = count;
    head->reserved = 0;
    // The copy goes to memory before the magic that marks it whole.
    cache_write_back(head, sizeof(struct handoff_acpi_save) + used);
    memcpy(head->magic, HANDOFF_ACPI_SAVE_MAGIC, sizeof(head->magic));
    cache_write_back(head->magic, sizeof(head->magic));
    if (left_out) {
        printf("Some of the guest's ACPI tables do not fit in the memory kept for them\n");
    }
    printf(
        "Kept %u of the guest's ACPI tables, %llu bytes, at %#llx, for the next start to write "
        "out\n",
        count, (unsigned long long)used, (unsigned long long)h->acpi_save
    );
}
