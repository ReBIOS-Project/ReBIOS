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
#include <uacpi/acpi.h>
#include <virt/guest_acpi.h>
#include <core.h>
#include <csm/e820.h>
#include <handoff.h>
#include <hw/acpi.h>
#include <hw/ioapic.h>
#include <lib/heap.h>
#include <lib/log.h>
#include <lib/util.h>
#include <memory.h>
#include <sys/smp.h>
#include <virt/guest_mem.h>
#include <virt/hpet.h>
#include <virt/layout.h>
#include <virt/pci_plan.h>
#include <virt/vacpi.h>
#include <virt/vlapic.h>

#define TABLE_ALIGN 16
#define FACS_ALIGN 64

// The tables the guest gets as the firmware has them, besides those the
// hypervisor generates: none points the guest at host memory or at hardware
// the hypervisor keeps for itself.
static const char *const kept[] = {
    "MSDM", "SLIC", "SPCR", "DBGP", "DBG2", "WAET", "WSMT", "VFCT", "BOOT"
};

// A table of the guest's, and where it goes among them, from the firmware's or
// made, then freed.
struct piece {
    const uint8_t *src;
    size_t length;
    size_t offset;
    uint8_t *made;
};

static void checksum_set(uint8_t *p, size_t size, uint32_t at) {
    p[at] = 0;
    p[at] = (uint8_t)-checksum8(p, size);
}

// The table at addr, if it lies within the memory the hypervisor maps.
static const uint8_t *host_table(uintptr_t addr) {
    uintptr_t top = core_handoff->map_top;
    if (addr == 0 || addr >= top || top - addr < ACPI_SDT_HEADER_SIZE) {
        return NULL;
    }
    const uint8_t *table = (const void *)(uintptr_t)addr;
    size_t length = read_le32(table + ACPI_SDT_LENGTH);
    if (length < ACPI_SDT_HEADER_SIZE || top - addr < length) {
        return NULL;
    }
    return table;
}

static const uint8_t *host_rsdp(void) {
    return (const void *)(uintptr_t)core_handoff->acpi_rsdp;
}

// The firmware's XSDT, or its RSDT if it has none, with the size of entries.
static const uint8_t *host_root(const uint8_t *rsdp, size_t *entry_size) {
    if (rsdp[ACPI_RSDP_REVISION] >= 2) {
        const uint8_t *xsdt = host_table(read_le64(rsdp + ACPI_RSDP_XSDT));
        if (xsdt != NULL) {
            *entry_size = 8;
            return xsdt;
        }
    }
    *entry_size = 4;
    return host_table(read_le32(rsdp + ACPI_RSDP_RSDT));
}

static uint64_t root_entry(const uint8_t *root, size_t entry_size, size_t i) {
    const uint8_t *entry = root + ACPI_SDT_HEADER_SIZE + i * entry_size;
    return entry_size == 8 ? read_le64(entry) : read_le32(entry);
}

static size_t root_count(const uint8_t *root, size_t entry_size) {
    return (read_le32(root + ACPI_SDT_LENGTH) - ACPI_SDT_HEADER_SIZE) / entry_size;
}

static bool ioapic_given(uintptr_t address) {
    for (size_t i = 0; i < ioapic_count(); i++) {
        if (ioapic_get(i)->base == address) {
            return true;
        }
    }
    return false;
}

// Whether the guest's machine has what an entry of the firmware's MADT
// describes, of the kinds the hypervisor knows: the processors' local APICs,
// the I/O APICs the hypervisor gives the guest, and the interrupt routes. Its
// local APICs are where a PC's are, which no override changes.
static bool madt_entry_kept(const uint8_t *entry, size_t length) {
    switch (entry[0]) {
        case ACPI_MADT_ENTRY_TYPE_LAPIC: {
            return length >= sizeof(struct acpi_madt_lapic);
        }
        case ACPI_MADT_ENTRY_TYPE_LOCAL_X2APIC: {
            return length >= sizeof(struct acpi_madt_x2apic);
        }
        case ACPI_MADT_ENTRY_TYPE_IOAPIC: {
            return length >= sizeof(struct acpi_madt_ioapic)
                && ioapic_given(read_le32(entry + offsetof(struct acpi_madt_ioapic, address)));
        }
        case ACPI_MADT_ENTRY_TYPE_INTERRUPT_SOURCE_OVERRIDE: {
            return length >= sizeof(struct acpi_madt_interrupt_source_override);
        }
        case ACPI_MADT_ENTRY_TYPE_NMI_SOURCE: {
            return length >= sizeof(struct acpi_madt_nmi_source);
        }
        case ACPI_MADT_ENTRY_TYPE_LAPIC_NMI: {
            return length >= sizeof(struct acpi_madt_lapic_nmi);
        }
        case ACPI_MADT_ENTRY_TYPE_LOCAL_X2APIC_NMI: {
            return length >= sizeof(struct acpi_madt_x2apic_nmi);
        }
        default: {
            return false;
        }
    }
}

// Leaves the local APIC of a processor the guest does not get unusable, where
// the namespace still has its processor.
static void processor_hide(uint8_t *entry) {
    const uint32_t usable = ACPI_PIC_ENABLED | ACPI_PIC_ONLINE_CAPABLE;
    uint32_t id;
    uint8_t *flags;
    if (entry[0] == ACPI_MADT_ENTRY_TYPE_LAPIC) {
        id = entry[offsetof(struct acpi_madt_lapic, id)];
        flags = entry + offsetof(struct acpi_madt_lapic, flags);
    } else if (entry[0] == ACPI_MADT_ENTRY_TYPE_LOCAL_X2APIC) {
        id = read_le32(entry + offsetof(struct acpi_madt_x2apic, id));
        flags = entry + offsetof(struct acpi_madt_x2apic, flags);
    } else {
        return;
    }
    if ((read_le32(flags) & usable) != 0 && !smp_guest_cpu(id)) {
        write_le32(flags, read_le32(flags) & ~usable);
        printf("Hiding processor %#x from the guest\n", id);
    }
}

// The guest's interrupt controllers, under the firmware's table's header: the
// 8259 pair the hypervisor always gives, local APICs where a PC's are, and the
// firmware's entries that madt_entry_kept() keeps, in its order.
static uint8_t *madt_make(const uint8_t *madt, size_t *length) {
    size_t host_length = *length;
    if (host_length < sizeof(struct acpi_madt)) {
        return NULL;
    }
    uint8_t *made = kmalloc(host_length);
    memcpy(made, madt, ACPI_SDT_HEADER_SIZE);
    write_le32(
        made + offsetof(struct acpi_madt, local_interrupt_controller_address),
        (uint32_t)VLAPIC_DEFAULT_BASE
    );
    write_le32(
        made + offsetof(struct acpi_madt, flags),
        read_le32(madt + offsetof(struct acpi_madt, flags)) | ACPI_PCAT_COMPAT
    );
    size_t out = sizeof(struct acpi_madt);
    size_t offset = sizeof(struct acpi_madt);
    const struct acpi_entry_hdr *hdr;
    while ((hdr = acpi_madt_next(madt, host_length, &offset)) != NULL) {
        const uint8_t *entry = (const uint8_t *)hdr;
        if (madt_entry_kept(entry, hdr->length)) {
            memcpy(made + out, entry, hdr->length);
            processor_hide(made + out);
            out += hdr->length;
        }
    }
    write_le32(made + ACPI_SDT_LENGTH, (uint32_t)out);
    checksum_set(made, out, ACPI_SDT_CHECKSUM);
    *length = out;
    return made;
}

// The guest's configuration space window, where the PCI plan put it, for the
// guest's one segment, under the firmware's table's header: none where the
// guest has none.
static uint8_t *mcfg_make(const uint8_t *mcfg, size_t *length) {
    const struct pci_plan *plan = pci_plan();
    if (plan == NULL || plan->ecam == 0) {
        return NULL;
    }
    size_t size = sizeof(struct acpi_mcfg) + sizeof(struct acpi_mcfg_allocation);
    uint8_t *made = kmalloc(size);
    memset(made, 0, size);
    memcpy(made, mcfg, ACPI_SDT_HEADER_SIZE);
    struct acpi_mcfg_allocation entry = {
        .address = plan->ecam, .segment = 0, .start_bus = 0, .end_bus = plan->bus_end
    };
    memcpy(made + sizeof(struct acpi_mcfg), &entry, sizeof(struct acpi_mcfg_allocation));
    write_le32(made + ACPI_SDT_LENGTH, (uint32_t)size);
    checksum_set(made, size, ACPI_SDT_CHECKSUM);
    *length = size;
    return made;
}

// The guest's HPET where the firmware's table puts it, with the event timer
// block ID its registers give, under the table's header: none for an HPET
// hidden from the guest.
static uint8_t *hpet_make(const uint8_t *hpet, size_t *length) {
    struct acpi_hpet table;
    if (*length < sizeof(struct acpi_hpet)) {
        return NULL;
    }
    memcpy(&table, hpet, sizeof(struct acpi_hpet));
    if (hpet_hidden(table.address.address)) {
        return NULL;
    }
    uint32_t id;
    if (table.address.address_space_id == ACPI_AS_ID_SYS_MEM
     && hpet_block_id(table.address.address, &id)) {
        table.block_id = id;
    }
    table.hdr.length = sizeof(struct acpi_hpet);
    uint8_t *made = kmalloc(sizeof(struct acpi_hpet));
    memcpy(made, &table, sizeof(struct acpi_hpet));
    checksum_set(made, sizeof(struct acpi_hpet), ACPI_SDT_CHECKSUM);
    *length = sizeof(struct acpi_hpet);
    return made;
}

// The guest's version of a table that the hypervisor makes rather than copies,
// in *made, NULL where the guest has none: false for other tables.
static bool table_made(const uint8_t *table, uint8_t **made, size_t *length) {
    if (memcmp(table, "APIC", 4) == 0) {
        *made = madt_make(table, length);
        return true;
    }
    if (memcmp(table, "MCFG", 4) == 0) {
        *made = mcfg_make(table, length);
        return true;
    }
    if (memcmp(table, "HPET", 4) == 0) {
        *made = hpet_make(table, length);
        return true;
    }
    return false;
}

static bool table_kept(const uint8_t *table) {
    for (size_t i = 0; i < ARRAY_COUNT(kept); i++) {
        if (memcmp(table, kept[i], 4) == 0) {
            return true;
        }
    }
    return false;
}

bool guest_acpi_build(uint8_t rsdp[GUEST_ACPI_RSDP_SIZE]) {
    size_t dsdt_length;
    uint8_t *dsdt = vacpi_dsdt(&dsdt_length);
    if (dsdt == NULL) {
        return false;
    }
    const uint8_t *host = host_rsdp();
    size_t entry_size = 4;
    const uint8_t *root = host != NULL ? host_root(host, &entry_size) : NULL;
    size_t count = root != NULL ? root_count(root, entry_size) : 0;

    // The FADT, the other tables, then the DSDT, which only the FADT lists.
    struct piece *pieces = kmalloc((count + 2) * sizeof(struct piece));
    uint32_t tables = 0;
    pieces[tables++] = (struct piece){ NULL, VACPI_FADT_SIZE, 0, NULL };
    for (size_t i = 0; i < count; i++) {
        const uint8_t *table = host_table(root_entry(root, entry_size, i));
        if (table == NULL) {
            continue;
        }
        size_t length = read_le32(table + ACPI_SDT_LENGTH);
        uint8_t *made = NULL;
        if (table_made(table, &made, &length)) {
            if (made != NULL) {
                pieces[tables++] = (struct piece){ made, length, 0, made };
            }
        } else if (table_kept(table)) {
            pieces[tables++] = (struct piece){ table, length, 0, NULL };
        }
    }
    uint32_t dsdt_piece = tables;
    pieces[dsdt_piece] = (struct piece){ dsdt, dsdt_length, 0, NULL };

    size_t offset = 0;
    for (size_t i = 0; i <= dsdt_piece; i++) {
        pieces[i].offset = offset;
        offset = align_up(offset + pieces[i].length, TABLE_ALIGN);
    }
    size_t rsdt_offset = offset;
    size_t rsdt_length = ACPI_SDT_HEADER_SIZE + tables * 4;
    size_t total = align_up(offset + rsdt_length, TABLE_ALIGN);

    uintptr_t base = layout_take(total, TABLE_ALIGN, E820_ACPI);
    // The OS keeps what it shares with itself across reclaiming the tables.
    uint32_t facs = layout_take(VACPI_FACS_SIZE, FACS_ALIGN, E820_NVS);
    uint8_t *copy = NULL;
    if (base != 0 && facs != 0) {
        copy = kmalloc(total);
        for (size_t i = 1; i <= dsdt_piece; i++) {
            memcpy(copy + pieces[i].offset, pieces[i].src, pieces[i].length);
        }
    }
    for (size_t i = 1; i < tables; i++) {
        kfree(pieces[i].made);
    }
    if (copy == NULL) {
        printf("No room in the guest's memory for its ACPI tables\n");
        kfree(pieces);
        kfree(dsdt);
        return false;
    }
    vacpi_fadt(copy, facs, base + pieces[dsdt_piece].offset);

    const uint8_t *rsdt_header = host != NULL
                               ? host_table(read_le32(host + ACPI_RSDP_RSDT))
                               : NULL;
    uint8_t *rsdt = copy + rsdt_offset;
    if (rsdt_header != NULL || root != NULL) {
        memcpy(rsdt, rsdt_header != NULL ? rsdt_header : root, ACPI_SDT_HEADER_SIZE);
    } else {
        memcpy(rsdt + 10, "REBIOSREBIOS  ", 14);
    }
    memcpy(rsdt, "RSDT", 4);
    write_le32(rsdt + ACPI_SDT_LENGTH, rsdt_length);
    rsdt[ACPI_SDT_REVISION] = 1;
    for (size_t i = 0; i < tables; i++) {
        write_le32(rsdt + ACPI_SDT_HEADER_SIZE + i * 4, base + pieces[i].offset);
    }
    checksum_set(rsdt, rsdt_length, ACPI_SDT_CHECKSUM);

    uint8_t facs_table[VACPI_FACS_SIZE];
    vacpi_facs(facs_table);
    guest_mem_phys_write(facs, facs_table, sizeof(facs_table));
    guest_mem_phys_write(base, copy, total);
    kfree(copy);
    kfree(pieces);
    kfree(dsdt);

    memset(rsdp, 0, GUEST_ACPI_RSDP_SIZE);
    memcpy(rsdp, "RSD PTR ", 8);
    const uint8_t *oem_id = host != NULL ? host + ACPI_RSDP_OEM_ID : (const uint8_t *)"REBIOS";
    memcpy(rsdp + ACPI_RSDP_OEM_ID, oem_id, 6);
    write_le32(rsdp + ACPI_RSDP_RSDT, base + rsdt_offset);
    checksum_set(rsdp, ACPI_RSDP_V1_SIZE, ACPI_RSDP_CHECKSUM);
    printf(
        "Guest ACPI tables at %#llx, %zu bytes, %u listed, FACS at %#x\n",
        (unsigned long long)base, total, tables, facs
    );
    return true;
}
