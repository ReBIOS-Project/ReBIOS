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

#ifndef VIRT__RTC_H
#define VIRT__RTC_H

#include <stdint.h>

// Reads the machine's RTC once, unless the FADT says it has none, for
// rtc_init() to see that its seconds count. Right after acpi_init().
void rtc_sample(void);
// The guest's RTC and CMOS RAM. After acpi_namespace_start() and
// layout_init().
void rtc_init(void);
// The RTC's bytes the guest's FADT gives the indexes of (ACPI 6.6, Table 5.9):
// its century, day-of-month alarm, and month alarm, 0 for none.
void rtc_fadt_indexes(uint8_t *century_index, uint8_t *day_alarm, uint8_t *month_alarm);

#endif
