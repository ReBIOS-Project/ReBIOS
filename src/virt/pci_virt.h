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

#ifndef VIRT__PCI_VIRT_H
#define VIRT__PCI_VIRT_H

// Takes the guest's configuration space accesses over, through ports CF8h and
// CFCh and through the configuration space windows, segment 0's where the plan
// puts it and the others' where the host has them, and gives the guest the
// plan's BARs and bridge windows, mapped onto the host's BARs. Needs
// layout_init().
void pci_virt_init(void);

#endif
