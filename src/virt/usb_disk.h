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

#ifndef VIRT__USB_DISK_H
#define VIRT__USB_DISK_H

#include <stdint.h>

// Tendril's mass storage driver reports to this. Before the first controller
// is added.
void usb_disk_register(void);
// What the guest asked for, from the stack's task.
void usb_disk_work(void);

// HYPERCALL_USB_DISK, from the legacy BIOS's driver.
void usb_disk_call(uint32_t gpa);

#endif
