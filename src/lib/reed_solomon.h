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
// You should have received a copy of the GNU General Public License along with
// ReBIOS. If not, see <https://www.gnu.org/licenses/>.

#ifndef LIB__REED_SOLOMON_H
#define LIB__REED_SOLOMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The Reed-Solomon code that Linux 7.3's ramoops keeps what it leaves in
// memory across a restart with (fs/pstore/ram_core.c): over GF(2^8), whose
// polynomial is x^8 + x^4 + x^3 + x^2 + 1, the generator's roots from alpha^0
// on, and REED_SOLOMON_PARITY bytes of parity, which correct up to half as
// many bytes in error, for each REED_SOLOMON_BLOCK bytes of data.
#define REED_SOLOMON_PARITY 16
#define REED_SOLOMON_BLOCK 128

// Builds the field's tables, before anything else here is used.
void reed_solomon_init(void);

// The parity of size bytes of data, at most 255 - REED_SOLOMON_PARITY.
void reed_solomon_encode(
    const uint8_t *data,
    size_t size,
    uint8_t parity[static REED_SOLOMON_PARITY]
);

// Corrects size bytes of data and their parity, and adds the number of bytes
// corrected to *corrected: false where they hold more errors than the code
// corrects, which leaves them as they are.
bool reed_solomon_decode(
    uint8_t *data,
    size_t size,
    uint8_t parity[static REED_SOLOMON_PARITY],
    size_t *corrected
);

// The parity of each block of data that the bytes from from up to end are in,
// REED_SOLOMON_PARITY bytes for each, in order from parity on.
void reed_solomon_encode_blocks(const uint8_t *data, uint8_t *parity, size_t from, size_t end);

// Corrects the first count blocks of data and their parity, and adds the
// number of bytes corrected to *corrected, and that of blocks with more errors
// than the code corrects to *failed.
void reed_solomon_decode_blocks(
    uint8_t *data,
    uint8_t *parity,
    size_t count,
    size_t *corrected,
    size_t *failed
);

#endif
