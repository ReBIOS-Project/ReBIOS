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
#include <loader/iommu_check.h>
#include <lib/log.h>
#include <loader/efi_glue.h>
#include <loader/pci_io.h>
#include <memory.h>

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
// 39-bit or 48-bit guest address widths, which take 3-level and 4-level
// tables.
#define VTD_SAGAW_3_OR_4_LEVELS ((uint32_t)0x6)
#define VTD_ECAP_QI ((uint64_t)1 << 1)

#define FOUND_NONE 0
#define FOUND_UNUSABLE 1
#define FOUND_USABLE 2

#define NOTICE_SECONDS 10

#define PCI_VENDOR_INTEL 0x8086
#define IGFX_DEVFN (2 << 3)
#define HOST_BRIDGE_GGC 0x52
// The memory size encodings the firmware picks when it allocates a shadow GTT
// for remapping.
#define GGC_MEMORY_VT_ENABLED 0x800

// Integrated graphics that malfunctions under DMA remapping, which Linux 7.3
// therefore leaves untranslated (drivers/iommu/intel/iommu.c,
// quirk_iommu_igfx() and quirk_calpella_no_shadow_gtt()). ReBIOS cannot: the
// guest's memory is not where the host's is, so the guest's driver would point
// it at the wrong memory.
static const uint16_t broadwell_igfx[] = {
    0x1602, 0x1606, 0x160a, 0x160b, 0x160d, 0x160e, 0x1612, 0x1616, 0x161a,
    0x161b, 0x161d, 0x161e, 0x1622, 0x1626, 0x162a, 0x162b, 0x162d, 0x162e,
    0x1632, 0x1636, 0x163a, 0x163b, 0x163d, 0x163e,
};
// Linux 7.3 names the QM57 and QS57 chipsets it was seen with.
#define ARRANDALE_HOST_BRIDGE 0x0044
static const uint16_t calpella_host_bridges[] = { 0x0040, 0x0062, 0x006a };

static const EFI_GUID acpi_20_guid = EFI_ACPI_20_TABLE_GUID;
static const EFI_GUID acpi_10_guid = EFI_ACPI_10_TABLE_GUID;

static const uint8_t *config_table(const EFI_GUID *guid) {
    for (size_t i = 0; i < efi_st->NumberOfTableEntries; i++) {
        const EFI_CONFIGURATION_TABLE *entry = &efi_st->ConfigurationTable[i];
        if (memcmp(&entry->VendorGuid, guid, sizeof(EFI_GUID)) == 0) {
            return entry->VendorTable;
        }
    }
    return NULL;
}

static uint32_t get32(const uint8_t *p) {
    uint32_t value;
    memcpy(&value, p, sizeof(uint32_t));
    return value;
}

static uint64_t get64(const uint8_t *p) {
    uint64_t value;
    memcpy(&value, p, sizeof(uint64_t));
    return value;
}

static uint16_t get16(const uint8_t *p) {
    uint16_t value;
    memcpy(&value, p, sizeof(uint16_t));
    return value;
}

static bool checksum_ok(const uint8_t *table, size_t length) {
    uint8_t sum = 0;
    for (size_t i = 0; i < length; i++) {
        sum += table[i];
    }
    return sum == 0;
}

// What the core needs of a VT-d unit, as boot services are the last chance to
// go back: queued invalidation, and second-stage tables of 3 or 4 levels.
static bool vtd_unit_usable(uintptr_t base) {
    if (base == 0) {
        printf("A VT-d unit has no register base\n");
        return false;
    }
    const volatile uint64_t *regs = (const volatile uint64_t *)(uintptr_t)base;
    uint64_t cap = regs[VTD_CAP / 8];
    uint64_t ecap = regs[VTD_ECAP / 8];
    if (cap == UINT64_MAX && ecap == UINT64_MAX) {
        printf(
            "Nothing answers at the VT-d unit's registers, %#llx\n",
            (unsigned long long)base
        );
        return false;
    }
    if (((cap >> VTD_CAP_SAGAW_SHIFT) & VTD_SAGAW_3_OR_4_LEVELS) != 0
     && (ecap & VTD_ECAP_QI) != 0) {
        return true;
    }
    printf(
        "The VT-d unit at %#llx lacks queued invalidation or 3 or 4-level "
        "tables, capabilities %#llx, extended %#llx\n",
        (unsigned long long)base, (unsigned long long)cap,
        (unsigned long long)ecap
    );
    return false;
}

// Whether the table is a DMAR or IVRS describing remapping hardware, within
// its length, and whether the hypervisor can use all of it.
static int iommu_table_check(const uint8_t *table) {
    bool dmar = memcmp(table, "DMAR", 4) == 0;
    if (!dmar && memcmp(table, "IVRS", 4) != 0) {
        return FOUND_NONE;
    }
    size_t length = get32(table + SDT_LENGTH);
    if (length < IOMMU_TABLE_HEADER_SIZE || !checksum_ok(table, length)) {
        printf("The %.4s table is malformed\n", (const char *)table);
        return FOUND_NONE;
    }

    uint32_t units = 0;
    bool usable = true;
    for (size_t offset = IOMMU_TABLE_HEADER_SIZE; length - offset >= 4; ) {
        size_t entry_length = get16(table + offset + 2);
        if (entry_length < 4 || entry_length > length - offset) {
            break;
        }
        uint8_t type = table[offset];
        if (dmar && get16(table + offset) == DMAR_DRHD
         && entry_length >= DRHD_BASE + 8) {
            units++;
            usable &= vtd_unit_usable(get64(table + offset + DRHD_BASE));
        } else if (!dmar
                && (type == IVHD_10H || type == IVHD_11H || type == IVHD_40H)
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
        printf(
            "The %.4s table describes no remapping hardware\n",
            (const char *)table
        );
        return FOUND_NONE;
    }
    return usable ? FOUND_USABLE : FOUND_UNUSABLE;
}

static int sdt_find_iommu(const uint8_t *sdt, size_t entry_size) {
    size_t length = get32(sdt + SDT_LENGTH);
    if (length < SDT_HEADER_SIZE) {
        return FOUND_NONE;
    }
    for (size_t offset = SDT_HEADER_SIZE; length - offset >= entry_size;
         offset += entry_size) {
        uintptr_t address = get32(sdt + offset);
        if (entry_size == 8) {
            address |= (uint64_t)get32(sdt + offset + 4) << 32;
        }
        const uint8_t *table = (const uint8_t *)(uintptr_t)address;
        int found = table != NULL ? iommu_table_check(table) : FOUND_NONE;
        if (found != FOUND_NONE) {
            printf(
                "IOMMU described by the %.4s table at %#llx\n",
                (const char *)table, (unsigned long long)address
            );
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
        uintptr_t xsdt = get32(rsdp + RSDP_XSDT)
            | (uint64_t)get32(rsdp + RSDP_XSDT + 4) << 32;
        if (xsdt != 0) {
            return sdt_find_iommu((const uint8_t *)(uintptr_t)xsdt, 8);
        }
    }
    uint32_t rsdt = get32(rsdp + RSDP_RSDT);
    return rsdt != 0 ? sdt_find_iommu((const uint8_t *)(uintptr_t)rsdt, 4)
        : FOUND_NONE;
}

static bool listed(uint16_t id, const uint16_t *ids, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (ids[i] == id) {
            return true;
        }
    }
    return false;
}

// Whether the integrated graphics, if any, is one that remapping breaks.
static bool igfx_broken(uint16_t *igfx_id) {
    size_t count;
    struct pci_io_device *devices = pci_io_devices(&count);
    const struct pci_io_device *igfx = NULL;
    const struct pci_io_device *host_bridge = NULL;
    for (size_t i = 0; i < count; i++) {
        const struct pci_io_device *d = &devices[i];
        if (d->vendor_id != PCI_VENDOR_INTEL || d->address.segment != 0
         || d->address.bus != 0) {
            continue;
        }
        if (d->address.devfn == IGFX_DEVFN
         && d->base_class == PCI_CLASS_DISPLAY) {
            igfx = d;
        } else if (d->address.devfn == 0) {
            host_bridge = d;
        }
    }

    bool broken = false;
    if (igfx != NULL) {
        *igfx_id = igfx->device_id;
        broken = listed(
            igfx->device_id, broadwell_igfx,
            sizeof(broadwell_igfx) / sizeof(uint16_t)
        );
        if (host_bridge != NULL
         && host_bridge->device_id == ARRANDALE_HOST_BRIDGE) {
            broken = true;
        }
        uint16_t ggc;
        bool no_shadow_gtt = host_bridge != NULL
            && listed(
                host_bridge->device_id, calpella_host_bridges,
                sizeof(calpella_host_bridges) / sizeof(uint16_t)
            )
            && host_bridge->io->Pci.Read(
                host_bridge->io, EfiPciIoWidthUint16, HOST_BRIDGE_GGC, 1, &ggc
            ) == EFI_SUCCESS
            && (ggc & GGC_MEMORY_VT_ENABLED) == 0;
        if (no_shadow_gtt) {
            broken = true;
        }
    }
    if (devices != NULL) {
        efi_bs->FreePool(devices);
    }
    return broken;
}

bool iommu_check(void) {
    int found = acpi_find_iommu();
    uint16_t igfx_id;
    if (found == FOUND_USABLE && !igfx_broken(&igfx_id)) {
        return true;
    }

    if (found == FOUND_USABLE) {
        log_notice(
            "ReBIOS cannot run with this machine's integrated graphics, "
            "%04x:%04x, which\n"
            "malfunctions when the IOMMU remaps its memory accesses. If "
            "another\n"
            "graphics card is installed, disable the integrated graphics in "
            "the\n"
            "firmware setup.\n",
            PCI_VENDOR_INTEL, igfx_id
        );
    } else if (found == FOUND_UNUSABLE) {
        log_notice("ReBIOS cannot use the IOMMU the firmware reports.\n");
    } else {
        log_notice(
            "ReBIOS needs an IOMMU (Intel VT-d or AMD-Vi), but the firmware "
            "reports none.\n"
            "If the machine has one, enable it in the firmware setup.\n"
        );
    }
    efi_bs->Stall(NOTICE_SECONDS * 1000000);
    return false;
}
