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

// The controller as the IBM Personal System/2 Hardware Interface Technical
// Reference, Common Interfaces (84F9735) of September 1991 has its Type 1
// controller: its ports, the status register's bits, the controller command
// byte's, its commands and what they answer, the output port's bits, and the
// bytes of the devices behind it that it passes on.
#define I8042_PORT_DATA 0x60
#define I8042_PORT_COMMAND 0x64

#define I8042_STATUS_OUTPUT_FULL 0x01
#define I8042_STATUS_INPUT_FULL 0x02
#define I8042_STATUS_SYSTEM 0x04
#define I8042_STATUS_COMMAND 0x08
#define I8042_STATUS_NOT_INHIBITED 0x10
#define I8042_STATUS_AUX_DATA 0x20

#define I8042_CCB_KBD_INTERRUPT 0x01
#define I8042_CCB_AUX_INTERRUPT 0x02
#define I8042_CCB_SYSTEM 0x04
#define I8042_CCB_KBD_DISABLED 0x10
#define I8042_CCB_AUX_DISABLED 0x20
#define I8042_CCB_TRANSLATE 0x40

#define I8042_COMMAND_READ_CCB 0x20
#define I8042_COMMAND_READ_RAM_FIRST 0x20
#define I8042_COMMAND_READ_RAM_LAST 0x3f
#define I8042_COMMAND_WRITE_CCB 0x60
#define I8042_COMMAND_WRITE_RAM_FIRST 0x60
#define I8042_COMMAND_WRITE_RAM_LAST 0x7f
#define I8042_COMMAND_RAM_MASK 0x1f
#define I8042_COMMAND_VERSION 0xa1
#define I8042_COMMAND_PASSWORD_TEST 0xa4
#define I8042_COMMAND_PASSWORD_LOAD 0xa5
#define I8042_COMMAND_PASSWORD_ENABLE 0xa6
#define I8042_COMMAND_AUX_DISABLE 0xa7
#define I8042_COMMAND_AUX_ENABLE 0xa8
#define I8042_COMMAND_AUX_TEST 0xa9
#define I8042_COMMAND_SELF_TEST 0xaa
#define I8042_COMMAND_KBD_TEST 0xab
#define I8042_COMMAND_KBD_DISABLE 0xad
#define I8042_COMMAND_KBD_ENABLE 0xae
#define I8042_COMMAND_READ_INPUT 0xc0
#define I8042_COMMAND_READ_OUTPUT 0xd0
#define I8042_COMMAND_WRITE_OUTPUT 0xd1
#define I8042_COMMAND_WRITE_KBD_BUFFER 0xd2
#define I8042_COMMAND_WRITE_AUX_BUFFER 0xd3
#define I8042_COMMAND_WRITE_AUX 0xd4
#define I8042_COMMAND_A20_DISABLE 0xdd
#define I8042_COMMAND_A20_ENABLE 0xdf
#define I8042_COMMAND_READ_TEST 0xe0
#define I8042_COMMAND_PULSE_FIRST 0xf0

#define I8042_PASSWORD_NOT_INSTALLED 0xf1
#define I8042_SELF_TEST_PASSED 0x55
#define I8042_INTERFACE_TEST_PASSED 0x00

#define I8042_OUTPUT_RESET_HIGH 0x01
#define I8042_OUTPUT_A20 0x02
#define I8042_OUTPUT_KBD_FULL 0x10
#define I8042_OUTPUT_AUX_FULL 0x20

#define I8042_DEVICE_SELF_TEST_PASSED 0xaa
#define I8042_DEVICE_ACK 0xfa
#define I8042_DEVICE_RESEND 0xfe

// The IDs of the devices behind the controller, NULL-ended, as Linux 7.3's
// i8042 driver takes them.
extern const char *const i8042_keyboard_ids[];
extern const char *const i8042_mouse_ids[];

// The chipset's keyboard controller, as a source of bytes from whatever is
// plugged into it: untranslated, with its interrupts on. Finds out what there
// is without resetting it, which firmware on some machines does not take well.
void i8042_init(void);
bool i8042_present(void);
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
