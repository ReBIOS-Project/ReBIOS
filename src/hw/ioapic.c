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
#include <uacpi/acpi.h>
#include <uacpi/tables.h>
#include <memory.h>
#include <core.h>
#include <hw/acpi.h>
#include <hw/ioapic.h>
#include <lib/log.h>

#define IOAPIC_REGSEL 0x00
#define IOAPIC_WINDOW 0x10
#define IOAPIC_WINDOW_END 0x14

#define IOAPIC_REG_VERSION 0x01
#define IOAPIC_REG_REDIR_LOW(n) (0x10 + 2 * (n))

#define IOAPIC_REDIR_MASKED (1u << 16)

static uint32_t ioapic_read(uintptr_t base, uint32_t reg) {
    *(volatile uint32_t *)(base + IOAPIC_REGSEL) = reg;
    return *(volatile uint32_t *)(base + IOAPIC_WINDOW);
}

static void ioapic_write(uintptr_t base, uint32_t reg, uint32_t value) {
    *(volatile uint32_t *)(base + IOAPIC_REGSEL) = reg;
    *(volatile uint32_t *)(base + IOAPIC_WINDOW) = value;
}

static void ioapic_mask(const struct acpi_madt_ioapic *entry) {
    uintptr_t base = entry->address;
    if (base + IOAPIC_WINDOW_END > core_handoff->map_top) {
        printf("I/O APIC %u at %#llx is out of reach\n", entry->id, (unsigned long long)base);
        return;
    }

    uint32_t entries = ((ioapic_read(base, IOAPIC_REG_VERSION) >> 16) & 0xff) + 1;
    uint32_t masked = 0;
    for (uint32_t i = 0; i < entries; i++) {
        uint32_t low = ioapic_read(base, IOAPIC_REG_REDIR_LOW(i));
        if (!(low & IOAPIC_REDIR_MASKED)) {
            ioapic_write(base, IOAPIC_REG_REDIR_LOW(i), low | IOAPIC_REDIR_MASKED);
            masked++;
        }
    }
    printf("I/O APIC %u at %#llx, GSIs %u-%u: masked %u entries\n", entry->id,
           (unsigned long long)base, entry->gsi_base, entry->gsi_base + entries - 1, masked);
}

void ioapic_mask_all(void) {
    struct uacpi_table table;
    if (!acpi_tables_ready()
        || uacpi_table_find_by_signature(ACPI_MADT_SIGNATURE, &table) != UACPI_STATUS_OK) {
        printf("No MADT, so no I/O APICs to mask\n");
        return;
    }

    const uint8_t *madt = table.ptr;
    uint32_t length = table.hdr->length;
    for (uint32_t off = sizeof(struct acpi_madt); off + sizeof(struct acpi_entry_hdr) <= length;) {
        const struct acpi_entry_hdr *hdr = (const void *)(madt + off);
        if (hdr->length < sizeof(*hdr) || hdr->length > length - off) {
            break;
        }
        if (hdr->type == ACPI_MADT_ENTRY_TYPE_IOAPIC
            && hdr->length >= sizeof(struct acpi_madt_ioapic)) {
            struct acpi_madt_ioapic entry;
            memcpy(&entry, hdr, sizeof(entry));
            ioapic_mask(&entry);
        }
        off += hdr->length;
    }
    uacpi_table_unref(&table);
}
