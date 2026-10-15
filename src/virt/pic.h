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

#ifndef VIRT__PIC_H
#define VIRT__PIC_H

#include <stdint.h>
#include <stdbool.h>

// A cascaded pair of 8259A interrupt controllers with the PC's edge/level
// control registers, as a model the caller serialises.

#define PIC_MASTER_CMD 0x20
#define PIC_MASTER_DATA 0x21
#define PIC_SLAVE_CMD 0xa0
#define PIC_SLAVE_DATA 0xa1
#define PIC_ELCR_MASTER 0x4d0
#define PIC_ELCR_SLAVE 0x4d1

void pic_reset(void);
void pic_write(uint16_t port, uint8_t value);
uint8_t pic_read(uint16_t port);

// The level of an IRQ line.
void pic_set_irq(unsigned irq, bool level);
// An edge on an IRQ line, which stays high until acknowledged, as the
// 8259A needs.
void pic_pulse_irq(unsigned irq);

// Whether the master's INT output is asserted, and the interrupt
// acknowledge that answers it: the vector, IRQ 7's when nothing requests.
bool pic_output(void);
uint8_t pic_acknowledge(void);

bool pic_masked(unsigned irq);
bool pic_in_service(unsigned irq);
// Whether the input was acknowledged since its line last rose.
bool pic_acknowledged(unsigned irq);
// Whether the ELCR makes an input level-triggered.
bool pic_level_triggered(unsigned irq);
// Whether software has unmasked any of the master's inputs since it last
// masked them all.
bool pic_in_use(void);

#endif
