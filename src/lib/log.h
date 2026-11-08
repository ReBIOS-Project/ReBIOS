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

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdnoreturn.h>

struct flanterm_context;

// Until this is called, the log is kept, to be written out here. A serial port
// of 0 means none.
void log_init(uint16_t serial_port, uint32_t serial_baud, uint32_t serial_clock, bool console);
// What the log shows goes to this console from now on, rather than the
// firmware's, as it lasts beyond ExitBootServices().
void log_set_screen(struct flanterm_context *screen);
// The guest draws on the screen from now on: only panics still reach it.
void log_hand_over_screen(void);
void vprintf(const char *fmt, va_list args);
__attribute__((format(printf, 1, 2))) void printf(const char *fmt, ...);
// Like printf(), but shown whatever the configuration.
__attribute__((format(printf, 1, 2))) void log_notice(const char *fmt, ...);
__attribute__((format(printf, 1, 2))) noreturn void panic(const char *fmt, ...);
bool log_panicked(void);
// The log as a stream of bytes, for keeping beyond a reset: its first
// LOG_SAVED_START bytes, and its latest ones after those.
#define LOG_SAVED_START 24576
// How many bytes the log has had.
size_t log_saved_count(void);
// Copies the log's first bytes, at most size, and returns how many.
size_t log_saved_start(char *out, size_t size, bool lock);
// Copies the log's latest bytes from position since on, at most size, and
// returns how many, with the position of the first in *first, which is after
// since where some between are lost. Without lock, for panics, the copy may
// catch a line half written.
size_t log_saved_latest(size_t since, char *out, size_t size, size_t *first, bool lock);
// What keeps the log beyond a reset: log_keep() calls it, and panic() does
// without wait.
typedef void (*log_keeper)(bool wait);
void log_set_keeper(log_keeper keeper);
// Before the machine resets or powers off.
void log_keep(void);
// The guest is done for: if the log is on the screen, it is shown there again,
// over the guest's picture, and log_panicked() stops the other processors.
// Returns whether it is shown.
bool log_take_screen(void);

#endif
