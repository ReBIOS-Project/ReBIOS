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

#ifndef VIRT__PCI_QUIRKS_H
#define VIRT__PCI_QUIRKS_H

// Takes over the other ways some devices give their configuration space, so
// that the guest finds there what the configuration ports show it: NVIDIA's and
// AMD's GPUs' copies of it, which hold the host's BAR addresses. Needs
// pci_virt_init() and vga_init().
void pci_quirks_init(void);

#endif
