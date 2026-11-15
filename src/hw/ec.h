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

#ifndef HW__EC_H
#define HW__EC_H

// Handles the embedded controller's address space from before the namespace is
// loaded, for the AML that its initialisation runs, but runs no _REG yet, as
// Linux 7.3's acpi_ec_ecdt_probe() does for the ECDT's controller. Accesses
// fail until the ECDT, or ec_find(), gives the controller's ports. Between
// uacpi_initialize() and uacpi_namespace_load().
void ec_early_init(void);
// Finds the first embedded controller of the namespace, or else the ECDT's,
// before the namespace is initialised. With the namespace loaded.
void ec_find(void);
// Drives the first embedded controller of the ACPI namespace for the AML the
// hypervisor runs: its address space, and its queries, which run the _Qxx
// methods. Once the namespace's _INI methods have run, as firmware's _REG
// methods may take what _INI sets up (the OS that \_SB.PCI0._INI reads from
// _OSI on Lenovo's IdeaPad D330), and as Linux 7.3 runs the embedded
// controller's _REG only after acpi_initialize_objects() (drivers/acpi/bus.c,
// acpi_bus_init()). With the caller holding acpi_lock().
void ec_init(void);

#endif
