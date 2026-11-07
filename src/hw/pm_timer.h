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

#ifndef HW__PM_TIMER_H
#define HW__PM_TIMER_H

#include <stdbool.h>
#include <stdint.h>

// The ACPI PM timer's rate (ACPI 6.6, 4.8.3.3).
#define PM_TIMER_HZ 3579545

// Finds the PM timer that the FADT gives, and checks that it counts, as Linux
// 7.3's acpi_pm.c does (init_acpi_pm_clocksource()). Needs acpi_init().
void pm_timer_init(void);
// Its port, 0 where there is none.
uint16_t pm_timer_port(void);
// Where its count wraps: 24 bits wide, or 32 with the FADT's TMR_VAL_EXT.
uint32_t pm_timer_mask(void);
// Whether it passed that check, and was not rejected since.
bool pm_timer_sound(void);
// For a timer that runs at the wrong rate, which tsc_calibrate() checks.
void pm_timer_reject(void);
uint32_t pm_timer_read(void);

#endif
