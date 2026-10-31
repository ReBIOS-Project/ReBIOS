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

#ifndef VIRT__KBC_H
#define VIRT__KBC_H

#include <stdbool.h>
#include <stdint.h>

// Gives the guest an 8042 of its own on ports 60h and 64h, with a PS/2
// keyboard and mouse behind it whatever the machine has. The chipset's, where
// it has devices, feeds them. Needs a20_init() and irq_init().
void kbc_init(void);

// Input for the emulated keyboard and mouse. A key is a HID keyboard usage;
// modifiers are as in a HID boot keyboard report. dy grows downwards, as in
// HID.
void kbc_key(uint16_t usage, bool pressed, uint8_t modifiers);
void kbc_pointer(int32_t dx, int32_t dy, int32_t wheel, uint32_t buttons);

// What the guest last asked of the keyboard's LEDs, in the bits of the HID
// boot keyboard's output report.
uint8_t kbc_leds(void);

#endif
