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
#include <lib/heap.h>
#include <lib/log.h>
#include <lib/spinlock.h>

#define IOAPIC_REGSEL 0x00
#define IOAPIC_WINDOW 0x10
#define IOAPIC_EOI 0x40
#define IOAPIC_PAGE_SIZE 0x1000

static struct ioapic *ioapics;
static uint32_t count;

static uint32_t reg_read(struct ioapic *io, uint32_t reg) {
    *(volatile uint32_t *)(uintptr_t)(io->base + IOAPIC_REGSEL) = reg;
    return *(volatile uint32_t *)(uintptr_t)(io->base + IOAPIC_WINDOW);
}

static void reg_write(struct ioapic *io, uint32_t reg, uint32_t value) {
    *(volatile uint32_t *)(uintptr_t)(io->base + IOAPIC_REGSEL) = reg;
    *(volatile uint32_t *)(uintptr_t)(io->base + IOAPIC_WINDOW) = value;
}

uint64_t ioapic_rte_read(struct ioapic *io, uint32_t pin) {
    spinlock_acquire(&io->lock);
    uint64_t low = reg_read(io, IOAPIC_REG_REDIR(pin));
    uint64_t high = reg_read(io, IOAPIC_REG_REDIR(pin) + 1);
    spinlock_release(&io->lock);
    return high << 32 | low;
}

void ioapic_rte_write(struct ioapic *io, uint32_t pin, uint64_t rte) {
    spinlock_acquire(&io->lock);
    reg_write(io, IOAPIC_REG_REDIR(pin) + 1, (uint32_t)(rte >> 32));
    reg_write(io, IOAPIC_REG_REDIR(pin), (uint32_t)rte);
    spinlock_release(&io->lock);
}

void ioapic_eoi(struct ioapic *io, uint8_t vector) {
    if ((io->version & 0xff) >= IOAPIC_VERSION_EOI) {
        *(volatile uint32_t *)(uintptr_t)(io->base + IOAPIC_EOI) = vector;
    }
}

uint32_t ioapic_count(void) {
    return count;
}

struct ioapic *ioapic_get(uint32_t index) {
    return index < count ? &ioapics[index] : NULL;
}

struct ioapic *ioapic_for_gsi(uint32_t gsi, uint32_t *pin) {
    for (uint32_t i = 0; i < count; i++) {
        if (gsi >= ioapics[i].gsi_base && gsi - ioapics[i].gsi_base < ioapics[i].pins) {
            *pin = gsi - ioapics[i].gsi_base;
            return &ioapics[i];
        }
    }
    return NULL;
}

static void add(const struct acpi_madt_ioapic *entry) {
    uint64_t base = entry->address;
    if (base + IOAPIC_PAGE_SIZE > core_handoff->map_top) {
        printf("I/O APIC %u at %#llx is out of reach\n", entry->id, (unsigned long long)base);
        return;
    }
    struct ioapic *io = &ioapics[count];
    *io = (struct ioapic){
        .base = base,
        .gsi_base = entry->gsi_base,
        .id = entry->id,
    };
    io->id_reg = reg_read(io, IOAPIC_REG_ID);
    io->version = reg_read(io, IOAPIC_REG_VERSION);
    io->arbitration = reg_read(io, IOAPIC_REG_ARBITRATION);
    io->pins = ((io->version >> 16) & 0xff) + 1;
    count++;

    uint32_t masked = 0;
    for (uint32_t i = 0; i < io->pins; i++) {
        uint32_t low = reg_read(io, IOAPIC_REG_REDIR(i));
        if (!(low & IOAPIC_RTE_MASKED)) {
            reg_write(io, IOAPIC_REG_REDIR(i), low | (uint32_t)IOAPIC_RTE_MASKED);
            masked++;
        }
    }
    printf("I/O APIC %u at %#llx, GSIs %u-%u: masked %u entries\n", entry->id,
           (unsigned long long)base, entry->gsi_base, entry->gsi_base + io->pins - 1, masked);
}

void ioapic_init(void) {
    struct uacpi_table table;
    if (!acpi_tables_ready()
        || uacpi_table_find_by_signature(ACPI_MADT_SIGNATURE, &table) != UACPI_STATUS_OK) {
        printf("No MADT, so no I/O APICs\n");
        return;
    }

    const uint8_t *madt = table.ptr;
    uint32_t length = table.hdr->length;
    // At most one I/O APIC per entry.
    ioapics = kmalloc(length / sizeof(struct acpi_madt_ioapic) * sizeof(*ioapics));
    for (uint32_t off = sizeof(struct acpi_madt); off + sizeof(struct acpi_entry_hdr) <= length;) {
        const struct acpi_entry_hdr *hdr = (const void *)(madt + off);
        if (hdr->length < sizeof(*hdr) || hdr->length > length - off) {
            break;
        }
        if (hdr->type == ACPI_MADT_ENTRY_TYPE_IOAPIC
            && hdr->length >= sizeof(struct acpi_madt_ioapic)) {
            struct acpi_madt_ioapic entry;
            memcpy(&entry, hdr, sizeof(entry));
            add(&entry);
        }
        off += hdr->length;
    }
    uacpi_table_unref(&table);
}
