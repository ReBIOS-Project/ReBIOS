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

#ifndef LIB__LOG_H
#define LIB__LOG_H

#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>

// Until this is called, the log is kept, to be written out here. A serial
// port of 0 means none.
void log_init(uint16_t serial_port, uint32_t serial_baud, bool console);
void vprintf(const char *fmt, va_list args);
__attribute__((format(printf, 1, 2))) void printf(const char *fmt, ...);
__attribute__((noreturn, format(printf, 1, 2))) void panic(const char *fmt, ...);

#endif
