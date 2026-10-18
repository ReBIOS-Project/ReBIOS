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

#ifndef IOMMU__IOMMU_TABLES_H
#define IOMMU__IOMMU_TABLES_H

#include <stdint.h>
#include <stdbool.h>

#define IOMMU_KIND_NONE 0
#define IOMMU_KIND_VTD 1
#define IOMMU_KIND_AMD 2

// Bits of iommu_device_range.amd_settings, from an IVHD device entry's DTE
// setting byte (AMD IOMMU 48882 rev. 3.11, Table 106).
#define IOMMU_AMD_INIT_PASS (1u << 0)
#define IOMMU_AMD_EINT_PASS (1u << 1)
#define IOMMU_AMD_NMI_PASS (1u << 2)
#define IOMMU_AMD_ATC_PERMIT (1u << 3)
#define IOMMU_AMD_SYS_MGT_SHIFT 4
#define IOMMU_AMD_SYS_MGT_MASK (3u << 4)
#define IOMMU_AMD_LINT0_PASS (1u << 6)
#define IOMMU_AMD_LINT1_PASS (1u << 7)

struct iommu_unit {
    // The register block's physical address.
    uint64_t base;
    // VT-d: the register block's size in bytes, from the DRHD's Size field.
    uint64_t vtd_size;
    uint16_t segment;
    // VT-d: the DRHD's INCLUDE_PCI_ALL flag.
    bool include_all;
    // AMD: the type of the IVHD the unit comes from, which tells what
    // amd_features holds.
    uint8_t amd_ivhd_type;
    // AMD: the IOMMU's own requester ID, the offset of its capability block in
    // its configuration space, and the IVHD's flags, IOMMU info, and IOMMU
    // feature reporting (type 10h) or IOMMU attributes (types 11h and 40h).
    uint16_t amd_rid;
    uint16_t amd_cap_offset;
    uint32_t amd_ivhd_flags;
    uint32_t amd_iommu_info;
    uint32_t amd_features;
    // AMD, from an IVHD of type 11h or 40h, 0 otherwise.
    uint64_t amd_efr;
    uint64_t amd_efr2;
};

// Requester IDs first to last of a segment, which the unit translates, in
// order of precedence: the first range listing a requester ID is the one that
// applies to it.
struct iommu_device_range {
    uint16_t segment;
    uint16_t first;
    uint16_t last;
    uint16_t unit;
    // AMD: the requester ID the IOMMU sees for these devices, when an alias
    // entry gives one.
    bool has_alias;
    uint16_t alias;
    uint8_t amd_settings;
};

// A device the firmware names by another identity than its requester ID: an
// I/O APIC (by its APIC ID), an HPET (by its number), or, for VT-d, an ACPI
// namespace device (by its ANDD number).
#define IOMMU_SPECIAL_IOAPIC 1
#define IOMMU_SPECIAL_HPET 2
#define IOMMU_SPECIAL_ACPI 3

struct iommu_special_device {
    uint8_t type;
    uint8_t id;
    uint16_t segment;
    uint16_t rid;
    uint16_t unit;
    uint8_t amd_settings;
};

// Memory that the devices given need handled specially, of one of three kinds:
// - with exclusion, memory they reach untranslated and unchecked by the IOMMU
//   (AMD IVMD exclusion ranges), readable and writable being set as well;
// - otherwise, with readable or writable, memory to map one to one for them,
//   with that access (VT-d RMRRs, AMD IVMDs);
// - otherwise, memory that must never be mapped for them (AMD IVMDs).
struct iommu_reserved_region {
    uint64_t base;
    // Inclusive.
    uint64_t last;
    bool exclusion;
    bool readable;
    bool writable;
    // 0, and meaningless, with all_devices.
    uint16_t segment;
    // Every device of every segment, or requester IDs first to last of the
    // segment.
    bool all_devices;
    uint16_t first;
    uint16_t last_rid;
};

struct iommu_tables {
    uint32_t kind;
    // VT-d: host address width in bits, and the DMAR flags. AMD: the IVinfo
    // field.
    uint32_t vtd_haw;
    uint32_t vtd_flags;
    uint32_t amd_ivinfo;
    // A malformed structure ended the parsing early, so units, devices, and
    // regions that the rest of the table describes are missing.
    bool truncated;
    uint32_t unit_count;
    struct iommu_unit *units;
    uint32_t range_count;
    struct iommu_device_range *ranges;
    uint32_t special_count;
    struct iommu_special_device *specials;
    uint32_t reserved_count;
    struct iommu_reserved_region *reserved;
};

// Parses DMAR or IVRS (kind NONE if neither is there or usable). Needs
// acpi_init() and pci_cfg_init(). Runs once; later calls return the same.
const struct iommu_tables *iommu_tables_parse(void);

// The range that applies to the requester ID, of those iommu_tables_parse()
// found, or NULL if no unit translates it.
const struct iommu_device_range *iommu_range_for(uint16_t segment, uint16_t rid);

#endif
