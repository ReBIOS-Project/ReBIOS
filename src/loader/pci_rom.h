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

#ifndef LOADER__PCI_ROM_H
#define LOADER__PCI_ROM_H

#include <stdint.h>
#include <stdbool.h>

// Finds the x86 PC-AT image meant for the device in a PCI expansion ROM,
// returning where it starts in rom, and its size. Returns false if there is
// none, or the ROM is malformed.
bool pci_rom_find_legacy_image(const void *rom, uint64_t rom_size, uint16_t vendor_id,
                               uint16_t device_id, const void **image, uint32_t *size);

#endif
