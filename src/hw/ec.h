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

#ifndef HW__EC_H
#define HW__EC_H

// Drives the first embedded controller of the ACPI namespace for the AML the
// hypervisor runs: its address space, and its queries, which run the _Qxx
// methods. With the namespace loaded and not yet initialised, so that _REG and
// _INI find it, and the caller holding acpi_lock().
void ec_init(void);

#endif
