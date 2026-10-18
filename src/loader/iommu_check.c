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
#include <lib/log.h>
#include <loader/efi_glue.h>
#include <loader/iommu_check.h>

#define RSDP_REVISION 15
#define RSDP_RSDT 16
#define RSDP_XSDT 24

#define SDT_LENGTH 4
#define SDT_HEADER_SIZE 36
// DMAR and IVRS have 12 more header bytes before their structures.
#define IOMMU_TABLE_HEADER_SIZE 48

#define DMAR_DRHD 0
#define DRHD_BASE 8
#define IVHD_10H 0x10
#define IVHD_11H 0x11
#define IVHD_40H 0x40
#define IVHD_BASE 8

#define VTD_CAP 0x08
#define VTD_ECAP 0x10
#define VTD_CAP_SAGAW_SHIFT 8
// 39-bit or 48-bit guest address widths, which take 3-level and 4-level tables.
#define VTD_SAGAW_3_OR_4_LEVELS 0x6u
#define VTD_ECAP_QI (1ull << 1)

#define FOUND_NONE 0
#define FOUND_UNUSABLE 1
#define FOUND_USABLE 2

#define NOTICE_SECONDS 10

static const EFI_GUID acpi_20_guid = EFI_ACPI_20_TABLE_GUID;
static const EFI_GUID acpi_10_guid = EFI_ACPI_10_TABLE_GUID;

static const uint8_t *config_table(const EFI_GUID *guid) {
    for (EFI_UINTN i = 0; i < efi_st->NumberOfTableEntries; i++) {
        const EFI_CONFIGURATION_TABLE *entry = &efi_st->ConfigurationTable[i];
        if (memcmp(&entry->VendorGuid, guid, sizeof(*guid)) == 0) {
            return entry->VendorTable;
        }
    }
    return NULL;
}

static uint32_t get32(const uint8_t *p) {
    uint32_t value;
    memcpy(&value, p, sizeof(value));
    return value;
}

static uint64_t get64(const uint8_t *p) {
    uint64_t value;
    memcpy(&value, p, sizeof(value));
    return value;
}

static uint16_t get16(const uint8_t *p) {
    uint16_t value;
    memcpy(&value, p, sizeof(value));
    return value;
}

static bool checksum_ok(const uint8_t *table, uint32_t length) {
    uint8_t sum = 0;
    for (uint32_t i = 0; i < length; i++) {
        sum += table[i];
    }
    return sum == 0;
}

// What the core needs of a VT-d unit, as boot services are the last chance to
// go back: queued invalidation, and second-stage tables of 3 or 4 levels.
static bool vtd_unit_usable(uint64_t base) {
    if (base == 0) {
        printf("A VT-d unit has no register base\n");
        return false;
    }
    const volatile uint64_t *regs = (const volatile uint64_t *)(uintptr_t)base;
    uint64_t cap = regs[VTD_CAP / 8];
    uint64_t ecap = regs[VTD_ECAP / 8];
    if (cap == UINT64_MAX && ecap == UINT64_MAX) {
        printf("Nothing answers at the VT-d unit's registers, %#llx\n", (unsigned long long)base);
        return false;
    }
    if (((cap >> VTD_CAP_SAGAW_SHIFT) & VTD_SAGAW_3_OR_4_LEVELS) != 0 && (ecap & VTD_ECAP_QI)) {
        return true;
    }
    printf("The VT-d unit at %#llx lacks queued invalidation or 3 or 4-level tables, "
           "capabilities %#llx, extended %#llx\n", (unsigned long long)base,
           (unsigned long long)cap, (unsigned long long)ecap);
    return false;
}

// Whether the table is a DMAR or IVRS describing remapping hardware, within
// its length, and whether the hypervisor can use all of it.
static int iommu_table_check(const uint8_t *table) {
    bool dmar = memcmp(table, "DMAR", 4) == 0;
    if (!dmar && memcmp(table, "IVRS", 4) != 0) {
        return FOUND_NONE;
    }
    uint32_t length = get32(table + SDT_LENGTH);
    if (length < IOMMU_TABLE_HEADER_SIZE || !checksum_ok(table, length)) {
        printf("The %.4s table is malformed\n", (const char *)table);
        return FOUND_NONE;
    }

    uint32_t units = 0;
    bool usable = true;
    for (uint32_t offset = IOMMU_TABLE_HEADER_SIZE; length - offset >= 4;) {
        uint16_t entry_length = get16(table + offset + 2);
        if (entry_length < 4 || entry_length > length - offset) {
            break;
        }
        uint8_t type = table[offset];
        if (dmar && get16(table + offset) == DMAR_DRHD && entry_length >= DRHD_BASE + 8) {
            units++;
            usable &= vtd_unit_usable(get64(table + offset + DRHD_BASE));
        } else if (!dmar && (type == IVHD_10H || type == IVHD_11H || type == IVHD_40H)
                   && entry_length >= IVHD_BASE + 8) {
            units++;
            if (get64(table + offset + IVHD_BASE) == 0) {
                printf("An AMD-Vi unit has no register base\n");
                usable = false;
            }
        }
        offset += entry_length;
    }
    if (units == 0) {
        printf("The %.4s table describes no remapping hardware\n", (const char *)table);
        return FOUND_NONE;
    }
    return usable ? FOUND_USABLE : FOUND_UNUSABLE;
}

static int sdt_find_iommu(const uint8_t *sdt, uint32_t entry_size) {
    uint32_t length = get32(sdt + SDT_LENGTH);
    if (length < SDT_HEADER_SIZE) {
        return FOUND_NONE;
    }
    for (uint32_t offset = SDT_HEADER_SIZE; length - offset >= entry_size; offset += entry_size) {
        uint64_t address = get32(sdt + offset);
        if (entry_size == 8) {
            address |= (uint64_t)get32(sdt + offset + 4) << 32;
        }
        const uint8_t *table = (const uint8_t *)(uintptr_t)address;
        int found = table != NULL ? iommu_table_check(table) : FOUND_NONE;
        if (found != FOUND_NONE) {
            printf("IOMMU described by the %.4s table at %#llx\n", (const char *)table,
                   (unsigned long long)address);
            return found;
        }
    }
    return FOUND_NONE;
}

static int acpi_find_iommu(void) {
    const uint8_t *rsdp = config_table(&acpi_20_guid);
    if (rsdp == NULL) {
        rsdp = config_table(&acpi_10_guid);
    }
    if (rsdp == NULL || memcmp(rsdp, "RSD PTR ", 8) != 0) {
        printf("No ACPI tables\n");
        return FOUND_NONE;
    }
    if (rsdp[RSDP_REVISION] >= 2) {
        uint64_t xsdt = get32(rsdp + RSDP_XSDT) | (uint64_t)get32(rsdp + RSDP_XSDT + 4) << 32;
        if (xsdt != 0) {
            return sdt_find_iommu((const uint8_t *)(uintptr_t)xsdt, 8);
        }
    }
    uint32_t rsdt = get32(rsdp + RSDP_RSDT);
    return rsdt != 0 ? sdt_find_iommu((const uint8_t *)(uintptr_t)rsdt, 4) : FOUND_NONE;
}

bool iommu_check(void) {
    int found = acpi_find_iommu();
    if (found == FOUND_USABLE) {
        return true;
    }

    const char *notice =
        "ReBIOS needs an IOMMU (Intel VT-d or AMD-Vi), but the firmware reports none.\n"
        "If the machine has one, enable it in the firmware setup.\n";
    if (found == FOUND_UNUSABLE) {
        notice = "ReBIOS cannot use the IOMMU the firmware reports.\n";
    }
    log_notice("%s", notice);
    efi_bs->Stall(NOTICE_SECONDS * 1000000);
    return false;
}
