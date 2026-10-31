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

#ifndef HW__USB_HANDOFF_H
#define HW__USB_HANDOFF_H

#include <stddef.h>
#include <stdint.h>
#include <hw/pci_cfg.h>

// Takes every USB controller from the firmware, whose System Management Mode
// code may still drive it to emulate a keyboard, and stops it, so the memory
// the firmware set aside for it need not stay where it is for the IOMMU: that
// memory is forgotten for each controller taken. Needs pci_enum_scan(), and
// comes before anything maps the IOMMU tables' reserved regions.
void usb_handoff(void);

// Where a controller's USB legacy support capability is, or 0: an xHCI
// controller's in its registers at base, an EHCI controller's in its
// configuration space.
uint32_t usb_xhci_legacy_cap(uintptr_t base, size_t size);
uint32_t usb_ehci_legacy_cap(struct pci_addr a, uintptr_t base);

#endif
