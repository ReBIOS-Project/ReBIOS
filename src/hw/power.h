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

#ifndef HW__POWER_H
#define HW__POWER_H

#include <stdbool.h>

// Through the firmware's reset service when the core can call it, else ACPI,
// else the legacy reset mechanisms.
__attribute__((noreturn)) void power_reset(bool warm);

// Through the firmware's reset service when the core can call it, else ACPI's
// S5 sleep state.
__attribute__((noreturn)) void power_off(void);

#endif
