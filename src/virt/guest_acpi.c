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
#include <uacpi/acpi.h>
#include <memory.h>
#include <core.h>
#include <csm/e820.h>
#include <handoff.h>
#include <lib/heap.h>
#include <lib/log.h>
#include <sys/smp.h>
#include <virt/guest_acpi.h>
#include <virt/guest_mem.h>
#include <virt/layout.h>
#include <virt/vacpi.h>

#define RSDP_CHECKSUM 8
#define RSDP_OEM_ID 9
#define RSDP_REVISION 15
#define RSDP_RSDT 16
#define RSDP_XSDT 24
#define RSDP_V1_SIZE 20

#define SDT_LENGTH 4
#define SDT_REVISION 8
#define SDT_CHECKSUM 9
#define SDT_HEADER_SIZE 36

#define TABLE_ALIGN 16
#define FACS_ALIGN 64

// The tables the guest gets besides those the hypervisor generates: none
// points the guest at host memory or at hardware the hypervisor keeps for
// itself.
static const char *const kept[] = {
    "APIC", "HPET", "MCFG", "MSDM", "SLIC", "SPCR", "DBGP", "DBG2", "WAET", "WSMT", "VFCT", "BOOT",
};

// A table of the guest's, and where it goes among them.
struct piece {
    const uint8_t *src;
    uint32_t length;
    uint32_t offset;
};

static uint32_t read32(const uint8_t *p) {
    uint32_t value;
    memcpy(&value, p, sizeof(value));
    return value;
}

static uint64_t read64(const uint8_t *p) {
    uint64_t value;
    memcpy(&value, p, sizeof(value));
    return value;
}

static void write32(uint8_t *p, uint32_t value) {
    memcpy(p, &value, sizeof(value));
}

static void checksum_set(uint8_t *p, uint32_t size, uint32_t at) {
    uint8_t sum = 0;
    p[at] = 0;
    for (uint32_t i = 0; i < size; i++) {
        sum += p[i];
    }
    p[at] = (uint8_t)-sum;
}

static uint32_t align_up(uint32_t x, uint32_t align) {
    return (x + align - 1) & ~(align - 1);
}

// The table at addr, if it lies within the memory the hypervisor maps.
static const uint8_t *host_table(uint64_t addr) {
    uint64_t top = core_handoff->map_top;
    if (addr == 0 || addr >= top || top - addr < SDT_HEADER_SIZE) {
        return NULL;
    }
    const uint8_t *table = (const void *)(uintptr_t)addr;
    uint32_t length = read32(table + SDT_LENGTH);
    if (length < SDT_HEADER_SIZE || top - addr < length) {
        return NULL;
    }
    return table;
}

static const uint8_t *host_rsdp(void) {
    return (const void *)(uintptr_t)core_handoff->acpi_rsdp;
}

// The firmware's XSDT, or its RSDT if it has none, with the size of entries.
static const uint8_t *host_root(const uint8_t *rsdp, uint32_t *entry_size) {
    if (rsdp[RSDP_REVISION] >= 2) {
        const uint8_t *xsdt = host_table(read64(rsdp + RSDP_XSDT));
        if (xsdt != NULL) {
            *entry_size = 8;
            return xsdt;
        }
    }
    *entry_size = 4;
    return host_table(read32(rsdp + RSDP_RSDT));
}

static uint64_t root_entry(const uint8_t *root, uint32_t entry_size, uint32_t i) {
    const uint8_t *entry = root + SDT_HEADER_SIZE + i * entry_size;
    return entry_size == 8 ? read64(entry) : read32(entry);
}

static uint32_t root_count(const uint8_t *root, uint32_t entry_size) {
    return (read32(root + SDT_LENGTH) - SDT_HEADER_SIZE) / entry_size;
}

// Marks the processors the guest does not get as unusable in a copy of the
// MADT, leaving every structure in place.
static void madt_hide_cpus(uint8_t *madt, uint32_t length) {
    const uint32_t usable = ACPI_PIC_ENABLED | ACPI_PIC_ONLINE_CAPABLE;
    bool changed = false;
    uint32_t offset = sizeof(struct acpi_madt);
    while (offset + sizeof(struct acpi_entry_hdr) <= length) {
        uint8_t *entry = madt + offset;
        uint8_t entry_length = entry[1];
        if (entry_length < sizeof(struct acpi_entry_hdr) || entry_length > length - offset) {
            break;
        }
        offset += entry_length;

        uint32_t id;
        uint8_t *flags;
        if (entry[0] == ACPI_MADT_ENTRY_TYPE_LAPIC
            && entry_length >= sizeof(struct acpi_madt_lapic)) {
            id = entry[offsetof(struct acpi_madt_lapic, id)];
            flags = entry + offsetof(struct acpi_madt_lapic, flags);
        } else if (entry[0] == ACPI_MADT_ENTRY_TYPE_LOCAL_X2APIC
                   && entry_length >= sizeof(struct acpi_madt_x2apic)) {
            id = read32(entry + offsetof(struct acpi_madt_x2apic, id));
            flags = entry + offsetof(struct acpi_madt_x2apic, flags);
        } else {
            continue;
        }
        if ((read32(flags) & usable) != 0 && !smp_guest_cpu(id)) {
            write32(flags, read32(flags) & ~usable);
            printf("Hiding processor %#x from the guest\n", id);
            changed = true;
        }
    }
    if (changed) {
        checksum_set(madt, length, SDT_CHECKSUM);
    }
}

static bool table_kept(const uint8_t *table) {
    for (size_t i = 0; i < sizeof(kept) / sizeof(kept[0]); i++) {
        if (memcmp(table, kept[i], 4) == 0) {
            return true;
        }
    }
    return false;
}

bool guest_acpi_build(uint8_t rsdp[GUEST_ACPI_RSDP_SIZE]) {
    uint32_t dsdt_length;
    uint8_t *dsdt = vacpi_dsdt(&dsdt_length);
    if (dsdt == NULL) {
        return false;
    }
    const uint8_t *host = host_rsdp();
    uint32_t entry_size = 4;
    const uint8_t *root = host != NULL ? host_root(host, &entry_size) : NULL;
    uint32_t count = root != NULL ? root_count(root, entry_size) : 0;

    // The FADT, the copies, then the DSDT, which only the FADT lists.
    struct piece *pieces = kmalloc((count + 2) * sizeof(*pieces));
    uint32_t tables = 0;
    pieces[tables++] = (struct piece){NULL, VACPI_FADT_SIZE, 0};
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *table = host_table(root_entry(root, entry_size, i));
        if (table != NULL && table_kept(table)) {
            pieces[tables++] = (struct piece){table, read32(table + SDT_LENGTH), 0};
        }
    }
    uint32_t dsdt_piece = tables;
    pieces[dsdt_piece] = (struct piece){dsdt, dsdt_length, 0};

    uint32_t offset = 0;
    for (uint32_t i = 0; i <= dsdt_piece; i++) {
        pieces[i].offset = offset;
        offset = align_up(offset + pieces[i].length, TABLE_ALIGN);
    }
    uint32_t rsdt_offset = offset;
    uint32_t rsdt_length = SDT_HEADER_SIZE + tables * 4;
    uint32_t total = align_up(offset + rsdt_length, TABLE_ALIGN);

    uint32_t base = layout_take(total, TABLE_ALIGN, E820_ACPI);
    // The OS keeps what it shares with itself across reclaiming the tables.
    uint32_t facs = layout_take(VACPI_FACS_SIZE, FACS_ALIGN, E820_NVS);
    if (base == 0 || facs == 0) {
        printf("No room in the guest's memory for its ACPI tables\n");
        kfree(pieces);
        kfree(dsdt);
        return false;
    }

    uint8_t *copy = kmalloc(total);
    for (uint32_t i = 1; i <= dsdt_piece; i++) {
        memcpy(copy + pieces[i].offset, pieces[i].src, pieces[i].length);
    }
    for (uint32_t i = 1; i < tables; i++) {
        uint8_t *table = copy + pieces[i].offset;
        if (memcmp(table, "APIC", 4) == 0) {
            madt_hide_cpus(table, pieces[i].length);
        }
    }
    vacpi_fadt(copy, facs, base + pieces[dsdt_piece].offset);

    const uint8_t *rsdt_header = host != NULL ? host_table(read32(host + RSDP_RSDT)) : NULL;
    uint8_t *rsdt = copy + rsdt_offset;
    if (rsdt_header != NULL || root != NULL) {
        memcpy(rsdt, rsdt_header != NULL ? rsdt_header : root, SDT_HEADER_SIZE);
    } else {
        memcpy(rsdt + 10, "REBIOSREBIOS  ", 14);
    }
    memcpy(rsdt, "RSDT", 4);
    write32(rsdt + SDT_LENGTH, rsdt_length);
    rsdt[SDT_REVISION] = 1;
    for (uint32_t i = 0; i < tables; i++) {
        write32(rsdt + SDT_HEADER_SIZE + i * 4, base + pieces[i].offset);
    }
    checksum_set(rsdt, rsdt_length, SDT_CHECKSUM);

    uint8_t facs_table[VACPI_FACS_SIZE];
    vacpi_facs(facs_table);
    guest_phys_write(facs, facs_table, sizeof(facs_table));
    guest_phys_write(base, copy, total);
    kfree(copy);
    kfree(pieces);
    kfree(dsdt);

    memset(rsdp, 0, GUEST_ACPI_RSDP_SIZE);
    memcpy(rsdp, "RSD PTR ", 8);
    memcpy(rsdp + RSDP_OEM_ID, host != NULL ? host + RSDP_OEM_ID : (const uint8_t *)"REBIOS", 6);
    write32(rsdp + RSDP_RSDT, base + rsdt_offset);
    checksum_set(rsdp, RSDP_V1_SIZE, RSDP_CHECKSUM);
    printf("Guest ACPI tables at %#x, %u bytes, %u listed, FACS at %#x\n", base, total, tables,
           facs);
    return true;
}
