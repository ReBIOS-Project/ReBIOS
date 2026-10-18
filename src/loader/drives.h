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

#ifndef LOADER__DRIVES_H
#define LOADER__DRIVES_H

// Has the firmware's drivers take on the storage and USB controllers they have
// not, so that their drives show.
void drives_connect(void);

// Orders the drives the firmware knows, as the boot_drive setting asks, into
// loader_handoff->boot_order.
void drives_collect(void);

#endif
