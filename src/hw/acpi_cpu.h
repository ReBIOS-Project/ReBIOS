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

#ifndef HW__ACPI_CPU_H
#define HW__ACPI_CPU_H

// The firmware's processors for the guest, which manages their power itself:
// tells the firmware so, as an OS does through the processors' _OSC, else
// _PDC, shows the P-states it gives, and hands those and the C-states over
// through SMI_CMD where the FADT asks for that. Right after the namespace's
// _INI methods, as Linux 7.3 does (acpi_early_processor_control_setup()), with
// the ACPI lock held.
void acpi_cpu_init(void);

#endif
