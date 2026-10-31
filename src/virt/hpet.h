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


#ifndef VIRT__HPET_H
#define VIRT__HPET_H

#include <stdbool.h>
#include <stdint.h>

// Hides from the guest every HPET with a timer that can deliver through the
// FSB, whose messages would go wherever the guest pointed them: its ACPI table
// and device go, and its registers read as absent. Other HPETs are the guest's
// as they are. After layout_init().
void hpet_init(void);

// Whether the HPET with its registers at base is hidden, and whether any is.
bool hpet_hidden(uintptr_t base);
bool hpet_any_hidden(void);
// Whether the guest's HPET has taken IRQ 0, so that the 8254 interrupts nothing
// (IA-PC HPET Specification 1.0a, 2.4.2.1).
bool hpet_replaces_pit(void);

#endif
