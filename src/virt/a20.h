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

#ifndef VIRT__A20_H
#define VIRT__A20_H

#include <stdbool.h>

// The guest's A20 gate: the system control port and the keyboard controller's
// output port are the guest's own, the host's gate stays open, and while both
// of the guest's A20 bits are clear, the processor sees every odd MiB below 4
// GiB as the even one before it. The system control port's INIT_NOW sends INIT
// to every vCPU. Needs virt_init().
void a20_init(void);
// The keyboard controller's A20 line, from its output port.
void a20_kbc_set(bool open);

#endif
