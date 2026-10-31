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

#ifndef HW__I8042_H
#define HW__I8042_H

#include <stdbool.h>
#include <stdint.h>

// The chipset's keyboard controller, as a source of bytes from whatever is
// plugged into it: untranslated, with its interrupts on. Finds out what there
// is without resetting it, which firmware on some machines does not take well.
void i8042_init(void);
bool i8042_keyboard(void);
bool i8042_aux(void);
// Queues a byte for a device, which goes when the controller takes it: nobody
// waits for that here.
void i8042_send(bool aux, uint8_t byte);
// Sends what the controller takes now of what is queued, and returns whether
// any is left for later.
bool i8042_send_more(void);
// Takes a byte from a device, if one waits.
bool i8042_receive(bool *aux, uint8_t *byte);

#endif
