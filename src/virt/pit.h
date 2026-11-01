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

#ifndef VIRT__PIT_H
#define VIRT__PIT_H

#include <stdbool.h>

struct cpu;

// Gives the guest an 8254 of its own, and the timer bits of port 61h.
void pit_init(void);

// The guest's 8259 took counter 0's interrupt, and has ended it.
void pit_tick_begun(void);
void pit_tick_ended(void);
// Whether that interrupt's handler has yet to return on that CPU, and that it
// has, which the guest taking interrupts again tells.
bool pit_tick_returning(const struct cpu *cpu);
void pit_tick_returned(void);
// An edge of IRQ 0 came from the host's line, as the HPET's legacy replacement
// route sends them, with IRQ 0 in service or not: true if it waits as the
// 8254's would, and this module delivers it.
bool pit_tick_held(bool in_service);

#endif
