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

#ifndef VIRT__LPSS_IDS_H
#define VIRT__LPSS_IDS_H

#include <stddef.h>
#include <stdint.h>

// The device IDs of Intel's LPSS functions, its I2C, UART, and SPI
// controllers, in ascending order.
extern const uint16_t lpss_ids[];
extern const size_t lpss_ids_count;

#endif
