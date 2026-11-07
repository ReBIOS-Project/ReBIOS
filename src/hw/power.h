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
#include <stdnoreturn.h>

// The reset control register at CF9h (ICH10 datasheet 319973-003, 13.7.5).
#define RST_CNT 0xcf9
#define RST_CNT_SYS_RST 0x02
#define RST_CNT_RST_CPU 0x04
#define RST_CNT_FULL_RST 0x08

// Through the firmware's reset service when the core can call it, else ACPI,
// else the legacy reset mechanisms.
noreturn void power_reset(bool warm);

// Through the firmware's reset service when the core can call it, else ACPI's
// S5 sleep state.
noreturn void power_off(void);

#endif
