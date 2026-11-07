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

#ifndef VIRT__OSPM_H
#define VIRT__OSPM_H

// While the guest has no ACPI OS of its own, does what one does for the
// firmware's thermal zones (ACPI 6.6, 11.1), as the firmware does outside ACPI
// mode. Needs vacpi_init(), before the guest starts.
void ospm_init(void);

#endif
