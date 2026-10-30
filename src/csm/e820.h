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

#ifndef CSM__E820_H
#define CSM__E820_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define E820_RAM 1
#define E820_RESERVED 2
#define E820_ACPI 3
#define E820_NVS 4
#define E820_UNUSABLE 5
#define E820_PMEM 7

#define E820_MAX_ENTRIES 128

// The 20-byte entry of the BIOS E820 interface, which the Intel CSM
// Specification 0.98's E820 table also uses.
struct e820_entry {
    uint64_t base;
    uint64_t size;
    uint32_t type;
} __attribute__((packed));

struct e820_map {
    size_t count;
    struct e820_entry entries[E820_MAX_ENTRIES];
};

// The memory sizes PC/AT CMOS reports, as stored in its registers.
struct e820_cmos {
    // Registers 15h-16h: base memory, in KiB.
    uint16_t base_kib;
    // Registers 17h-18h and 30h-31h: memory from 1 MiB, in KiB, at most 65535.
    uint16_t extended_kib;
    // Registers 34h-35h: memory from 16 MiB, in 64 KiB units, at most 65535.
    uint16_t above_16m;
    // Registers 5Bh-5Dh: memory from 4 GiB, in 64 KiB units, at most 0xffffff.
    uint32_t above_4g;
};

void e820_init(struct e820_map *map);

// Makes [base, base + size) of the type, whatever was there. Type 0 makes it a
// hole. Returns false, leaving the map unchanged, if the result would need
// more than E820_MAX_ENTRIES entries.
bool e820_set(
    struct e820_map *map, uintptr_t base, size_t size, uint32_t type
);

// Sets every range of a UEFI memory map, as it stands after
// ExitBootServices(), to the corresponding E820 type.
bool e820_set_efi_map(
    struct e820_map *map, const void *efi_mmap, size_t mmap_size,
    size_t desc_size
);

// Replaces the first MiB with base memory, a 1 KiB extended BIOS data area,
// and a 64 KiB system BIOS, as in the example address map of ACPI 6.6, 15.5.
bool e820_set_low_memory(struct e820_map *map);

void e820_cmos(const struct e820_map *map, struct e820_cmos *cmos);

void e820_log(const struct e820_map *map);

#endif
