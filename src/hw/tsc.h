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

#ifndef HW__TSC_H
#define HW__TSC_H

// Measures the TSC's rate against the PM timer, where the loader only timed it
// with the firmware's Stall(), and takes its nominal rate instead where the
// processor gives one that agrees. Needs pm_timer_init(), and comes before
// anything times with the TSC.
void tsc_calibrate(void);

#endif
