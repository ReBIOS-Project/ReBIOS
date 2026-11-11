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

#ifndef HW__SMBIOS_H
#define HW__SMBIOS_H

#include <stddef.h>
#include <stdint.h>
#include <lib/util.h>

// The SMBIOS 2.1 (32-bit) entry point (DSP0134 3.9.0, 5.2.1).
struct smbios_entry32 {
    char anchor[4];
    uint8_t checksum;
    uint8_t length;
    uint8_t major;
    uint8_t minor;
    uint16_t max_struct_size;
    uint8_t revision;
    uint8_t formatted[5];
    char intermediate_anchor[5];
    uint8_t intermediate_checksum;
    uint16_t table_length;
    uint32_t table_address;
    uint16_t struct_count;
    uint8_t bcd_revision;
} __attribute__((packed));

_Static_assert(
    sizeof(struct smbios_entry32) == 0x1f,
    "The SMBIOS 2.1 entry point's layout is not the one firmware and OSes use."
);

// SMBIOS 2.1 gave the entry point's length as 1Eh, short of its BCD revision,
// so 2.1 entry points may say 1Eh or 1Fh (DSP0134 3.9.0, 5.2.1), and Linux
// 7.3's dmi_present() takes 1Eh to 20h. The intermediate checksum covers 0Fh
// bytes from 10h, the BCD revision among them, whatever the length says.
#define SMBIOS_ENTRY32_MIN_LENGTH 0x1e
#define SMBIOS_ENTRY32_MAX_LENGTH 0x20
#define SMBIOS_ENTRY32_INTERMEDIATE_SIZE 0x0f

// Sets the entry point's checksums to match the rest of it.
static inline void smbios_entry32_checksum(struct smbios_entry32 *eps) {
    size_t intermediate = offsetof(struct smbios_entry32, intermediate_anchor);
    eps->intermediate_checksum = 0;
    eps->intermediate_checksum = (uint8_t)-checksum8(
        (uint8_t *)eps + intermediate, SMBIOS_ENTRY32_INTERMEDIATE_SIZE
    );
    eps->checksum = 0;
    eps->checksum = (uint8_t)-checksum8(eps, eps->length);
}

#endif
