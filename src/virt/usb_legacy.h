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

#ifndef VIRT__USB_LEGACY_H
#define VIRT__USB_LEGACY_H

#include <stdbool.h>

struct usb_controller;

// Whether the guest has to be kept from the controller, as the stack has it or
// is about to.
bool usb_legacy_own(struct usb_controller *c);
// The controller is the guest's from now on.
void usb_legacy_disown(struct usb_controller *c);

#endif
