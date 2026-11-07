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

#ifndef HW__CMOS_H
#define HW__CMOS_H

#include <stdbool.h>
#include <stdint.h>

// The machine's RTC and its RAM, behind ports 70h-77h, which the firmware's
// AML and the guest's RTC share: an access of the hypervisor's own leaves the
// index registers as the AML last set them.
void cmos_host_io(uint16_t port, bool in, uint8_t *value);
// A register of the standard bank, or of the RTC's own.
uint8_t cmos_read(uint8_t reg);
void cmos_write(uint8_t reg, uint8_t value);

#endif
