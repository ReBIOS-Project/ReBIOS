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
// You should have received a copy of the GNU General Public License along with
// ReBIOS. If not, see <https://www.gnu.org/licenses/>.

#ifndef LOADER__LAST_LOG_H
#define LOADER__LAST_LOG_H

// Writes the log that ReBIOS kept in memory at its last start with save_log
// (handoff.h) to rebios-last.log, beside the loader's image, once. Says on the
// screen where the log went, or why it did not.
void last_log_write(void);

// Keeps this start's log in memory, from now on, for the next start to write
// out.
void last_log_keep(void);

// Deletes the EFI variables that ReBIOS 0.7.0 to 0.11.0 kept their log in, if
// any are left, without reading them.
void last_log_delete_variables(void);

#endif
