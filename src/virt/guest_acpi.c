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

#define RSDP_CHECKSUM 8
#define RSDP_REVISION 15
#define RSDP_RSDT 16
#define RSDP_LENGTH 20
#define RSDP_XSDT 24
#define RSDP_EXTENDED_CHECKSUM 32
#define RSDP_V1_SIZE 20

#define SDT_LENGTH 4
#define SDT_REVISION 8
#define SDT_CHECKSUM 9
#define SDT_HEADER_SIZE 36

#define FADT_FIRMWARE_CTRL 36
#define FADT_DSDT 40
#define FADT_X_FIRMWARE_CTRL 132
#define FADT_X_DSDT 140
#define FACS_LENGTH 4
#define FACS_MIN_SIZE 64

#define WPBT_HANDOFF_SIZE 36
#define WPBT_HANDOFF_ADDRESS 40
#define WPBT_HANDOFF_MAX (64u << 20)

#define TABLE_ALIGN 16

// The IOMMU's tables, as the guest must never take the IOMMU over, those only
// UEFI systems have or whose code lives in UEFI runtime services memory, and
// those that describe the host's memory, which the guest's is not.
static const char *const left_out[] = {
    "DMAR", "IVRS", "UEFI", "BGRT", "PRMT", "SRAT", "SLIT", "HMAT", "MSCT",
};

// What goes into the guest's copy: a table, or the WPBT's binary.
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

static void write64(uint8_t *p, uint64_t value) {
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

static const uint8_t *host_fadt(void) {
    const uint8_t *rsdp = host_rsdp();
    if (rsdp == NULL) {
        return NULL;
    }
    uint32_t entry_size;
    const uint8_t *root = host_root(rsdp, &entry_size);
    if (root == NULL) {
        return NULL;
    }
    for (uint32_t i = 0; i < root_count(root, entry_size); i++) {
        const uint8_t *table = host_table(root_entry(root, entry_size, i));
        if (table != NULL && memcmp(table, "FACP", 4) == 0) {
            return table;
        }
    }
    return NULL;
}

bool guest_acpi_facs(uint64_t *base, uint64_t *size) {
    const uint8_t *fadt = host_fadt();
    if (fadt == NULL) {
        return false;
    }
    uint64_t facs = 0;
    if (read32(fadt + SDT_LENGTH) >= FADT_X_FIRMWARE_CTRL + 8) {
        facs = read64(fadt + FADT_X_FIRMWARE_CTRL);
    }
    if (facs == 0) {
        facs = read32(fadt + FADT_FIRMWARE_CTRL);
    }
    uint64_t top = core_handoff->map_top;
    if (facs == 0 || facs >= top || top - facs < FACS_MIN_SIZE) {
        return false;
    }
    uint32_t length = read32((const uint8_t *)(uintptr_t)facs + FACS_LENGTH);
    *base = facs;
    *size = length > FACS_MIN_SIZE ? length : FACS_MIN_SIZE;
    return true;
}

// Whether the WPBT points to a binary its copy can take along.
static bool wpbt_usable(const uint8_t *table, uint32_t length) {
    if (length < WPBT_HANDOFF_ADDRESS + 8) {
        return false;
    }
    uint32_t size = read32(table + WPBT_HANDOFF_SIZE);
    uint64_t addr = read64(table + WPBT_HANDOFF_ADDRESS);
    uint64_t top = core_handoff->map_top;
    return size != 0 && size <= WPBT_HANDOFF_MAX && addr != 0 && addr < top && top - addr >= size;
}

static bool table_left_out(const uint8_t *table) {
    for (size_t i = 0; i < sizeof(left_out) / sizeof(left_out[0]); i++) {
        if (memcmp(table, left_out[i], 4) == 0) {
            return true;
        }
    }
    return false;
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

bool guest_acpi_build(uint8_t rsdp[GUEST_ACPI_RSDP_SIZE]) {
    const uint8_t *host = host_rsdp();
    if (host == NULL) {
        return false;
    }
    uint32_t entry_size;
    const uint8_t *root = host_root(host, &entry_size);
    if (root == NULL) {
        printf("The firmware's ACPI root table is unusable\n");
        return false;
    }
    uint32_t count = root_count(root, entry_size);

    // The tables the root lists, then the DSDT and the WPBT's binary, which
    // the copies of the FADT and WPBT point to.
    struct piece *pieces = kmalloc((count + 2) * sizeof(*pieces));
    uint32_t tables = 0;
    uint32_t fadt = UINT32_MAX;
    uint32_t wpbt = UINT32_MAX;
    for (uint32_t i = 0; i < count; i++) {
        uint64_t addr = root_entry(root, entry_size, i);
        const uint8_t *table = host_table(addr);
        if (table == NULL) {
            printf("Leaving the ACPI table at %#llx out of the guest's, as it is unusable\n",
                   (unsigned long long)addr);
            continue;
        }
        if (table_left_out(table)) {
            printf("Leaving the %.4s table out of the guest's ACPI tables\n", (const char *)table);
            continue;
        }
        uint32_t length = read32(table + SDT_LENGTH);
        if (memcmp(table, "WPBT", 4) == 0) {
            if (wpbt != UINT32_MAX || !wpbt_usable(table, length)) {
                printf("Leaving a WPBT out of the guest's ACPI tables\n");
                continue;
            }
            wpbt = tables;
        }
        if (memcmp(table, "FACP", 4) == 0 && fadt == UINT32_MAX) {
            fadt = tables;
        }
        pieces[tables++] = (struct piece){table, length, 0};
    }

    uint32_t all = tables;
    uint32_t dsdt = UINT32_MAX;
    if (fadt != UINT32_MAX) {
        const uint8_t *table = pieces[fadt].src;
        uint64_t addr = 0;
        if (pieces[fadt].length >= FADT_X_DSDT + 8) {
            addr = read64(table + FADT_X_DSDT);
        }
        if (addr == 0) {
            addr = read32(table + FADT_DSDT);
        }
        const uint8_t *dsdt_table = host_table(addr);
        if (dsdt_table != NULL) {
            dsdt = all;
            pieces[all++] = (struct piece){dsdt_table, read32(dsdt_table + SDT_LENGTH), 0};
        } else {
            printf("The FADT points to no usable DSDT\n");
        }
    }
    uint32_t binary = UINT32_MAX;
    if (wpbt != UINT32_MAX) {
        const uint8_t *table = pieces[wpbt].src;
        const void *src = (const void *)(uintptr_t)read64(table + WPBT_HANDOFF_ADDRESS);
        binary = all;
        pieces[all++] = (struct piece){src, read32(table + WPBT_HANDOFF_SIZE), 0};
    }

    bool have_xsdt = host[RSDP_REVISION] >= 2;
    uint32_t offset = 0;
    for (uint32_t i = 0; i < all; i++) {
        pieces[i].offset = offset;
        offset = align_up(offset + pieces[i].length, TABLE_ALIGN);
    }
    uint32_t rsdt_offset = offset;
    uint32_t rsdt_length = SDT_HEADER_SIZE + tables * 4;
    offset = align_up(offset + rsdt_length, TABLE_ALIGN);
    uint32_t xsdt_offset = offset;
    uint32_t xsdt_length = SDT_HEADER_SIZE + tables * 8;
    if (have_xsdt) {
        offset = align_up(offset + xsdt_length, TABLE_ALIGN);
    }
    uint32_t total = offset;

    uint32_t base = layout_take(total, TABLE_ALIGN, E820_ACPI);
    if (base == 0) {
        printf("No room in the guest's memory for its ACPI tables\n");
        kfree(pieces);
        return false;
    }

    uint8_t *copy = kmalloc(total);
    for (uint32_t i = 0; i < all; i++) {
        memcpy(copy + pieces[i].offset, pieces[i].src, pieces[i].length);
    }
    for (uint32_t i = 0; i < tables; i++) {
        uint8_t *table = copy + pieces[i].offset;
        if (memcmp(table, "APIC", 4) == 0) {
            madt_hide_cpus(table, pieces[i].length);
        }
    }
    if (dsdt != UINT32_MAX) {
        uint8_t *table = copy + pieces[fadt].offset;
        uint32_t addr = base + pieces[dsdt].offset;
        write32(table + FADT_DSDT, addr);
        if (pieces[fadt].length >= FADT_X_DSDT + 8) {
            write64(table + FADT_X_DSDT, addr);
        }
        checksum_set(table, pieces[fadt].length, SDT_CHECKSUM);
    }
    if (binary != UINT32_MAX) {
        uint8_t *table = copy + pieces[wpbt].offset;
        write64(table + WPBT_HANDOFF_ADDRESS, base + pieces[binary].offset);
        checksum_set(table, pieces[wpbt].length, SDT_CHECKSUM);
    }

    // The root tables keep the firmware's identification, and an RSDT is
    // there for ACPI 1.0 systems even if the firmware has only an XSDT.
    const uint8_t *rsdt_header = host_table(read32(host + RSDP_RSDT));
    uint8_t *rsdt = copy + rsdt_offset;
    memcpy(rsdt, rsdt_header != NULL ? rsdt_header : root, SDT_HEADER_SIZE);
    memcpy(rsdt, "RSDT", 4);
    write32(rsdt + SDT_LENGTH, rsdt_length);
    rsdt[SDT_REVISION] = 1;
    for (uint32_t i = 0; i < tables; i++) {
        write32(rsdt + SDT_HEADER_SIZE + i * 4, base + pieces[i].offset);
    }
    checksum_set(rsdt, rsdt_length, SDT_CHECKSUM);
    if (have_xsdt) {
        uint8_t *xsdt = copy + xsdt_offset;
        memcpy(xsdt, entry_size == 8 ? root : rsdt, SDT_HEADER_SIZE);
        memcpy(xsdt, "XSDT", 4);
        write32(xsdt + SDT_LENGTH, xsdt_length);
        xsdt[SDT_REVISION] = 1;
        for (uint32_t i = 0; i < tables; i++) {
            write64(xsdt + SDT_HEADER_SIZE + i * 8, base + pieces[i].offset);
        }
        checksum_set(xsdt, xsdt_length, SDT_CHECKSUM);
    }

    guest_phys_write(base, copy, total);
    kfree(copy);
    kfree(pieces);

    memset(rsdp, 0, GUEST_ACPI_RSDP_SIZE);
    memcpy(rsdp, host, have_xsdt ? GUEST_ACPI_RSDP_SIZE : RSDP_V1_SIZE);
    write32(rsdp + RSDP_RSDT, base + rsdt_offset);
    checksum_set(rsdp, RSDP_V1_SIZE, RSDP_CHECKSUM);
    if (have_xsdt) {
        write32(rsdp + RSDP_LENGTH, GUEST_ACPI_RSDP_SIZE);
        write64(rsdp + RSDP_XSDT, base + xsdt_offset);
        checksum_set(rsdp, GUEST_ACPI_RSDP_SIZE, RSDP_EXTENDED_CHECKSUM);
    }
    printf("Guest ACPI tables at %#x, %u bytes, %u listed\n", base, total, tables);
    return true;
}
