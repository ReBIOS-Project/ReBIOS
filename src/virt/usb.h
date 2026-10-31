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

#ifndef VIRT__USB_H
#define VIRT__USB_H

#include <stdbool.h>

// Keeps memory for the USB host stack, and shows the guest the machine's USB
// controllers as a BIOS with legacy USB support has them. After
// pci_virt_init(), before the guest's memory map is made.
void usb_init(void);
// Starts driving them, on the way to the guest: device DMA is remapped by
// then.
void usb_start(void);

// A slice of the boot processor's time for the stack, whose work goes on from
// where the last one left it, one in four at most while the guest wants the
// processor, as it does unless guest_waiting.
void usb_slice(bool guest_waiting);
// Whether the stack has work under way, not waiting for anything.
bool usb_working(void);
// Whether the devices plugged in so far are all up.
bool usb_ready(void);

#endif
