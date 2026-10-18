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

// The guest's A20 gate: the A20 bits of the system control port and of the
// keyboard controller's output port are the guest's own, the host's gate stays
// open, and while both of the guest's are clear, the processor sees every odd
// MiB below 4 GiB as the even one before it. Needs virt_init().
void a20_init(void);

#endif
