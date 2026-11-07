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

#ifndef VIRT__ISA_PORTS_H
#define VIRT__ISA_PORTS_H

#include <stdbool.h>

// Gives the guest the machine's serial and parallel ports that the firmware's
// ACPI describes as enabled, with their interrupts, but for the UART of
// ReBIOS's log, which the guest never reaches, and the IDE channels in
// compatibility mode. After irq_init() and acpi_namespace_start().
void isa_ports_init(void);

// Whether the guest got the serial or parallel port of that device.
bool isa_ports_given(const void *node);

#endif
