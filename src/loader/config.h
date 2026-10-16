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

#ifndef LOADER__CONFIG_H
#define LOADER__CONFIG_H

#include <stdint.h>
#include <stdbool.h>
#include <efi.h>
#include <handoff.h>

#define CONFIG_VGABIOS_MAX 256

struct config {
    bool serial;
    uint16_t serial_port;
    uint32_t serial_baud;
    bool verbose;
    // NUL-terminated, empty when unset.
    EFI_CHAR16 vgabios[CONFIG_VGABIOS_MAX];
    bool vga_specified;
    struct handoff_pci_address vga_device;
    // One of the HANDOFF_VGA_EMULATION_* and HANDOFF_VGA_SCALING_* values.
    uint8_t vga_emulation;
    uint8_t vga_scaling;
    uint8_t vga_scale;
    // One of the HANDOFF_CPU_FILTER_* values.
    uint32_t cpu_filter;
    uint32_t cpu_range_count;
    struct {
        uint32_t first;
        uint32_t last;
    } cpu_ranges[HANDOFF_MAX_CPU_RANGES];
};

// The configuration in effect, filled in by config_load().
extern struct config config;

// Sets the defaults, then applies the first configuration source found.
void config_load(void);

// Copies what the core needs into the handoff.
void config_to_handoff(struct handoff_config *out);

#endif
