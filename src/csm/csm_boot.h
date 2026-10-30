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

#ifndef CSM__CSM_BOOT_H
#define CSM__CSM_BOOT_H

struct vcpu;

// Lays out the guest's legacy BIOS, SeaBIOS built as a CSM16, and sets the
// vCPU up to call into it, as the Intel CSM Specification 0.98's firmware side
// would: initialisation, the option ROMs, then the boot.
void csm_boot_start(struct vcpu *v);

// Takes the guest's return from a CSM16 call, and sets up the next one.
void csm_boot_returned(struct vcpu *v);

#endif
