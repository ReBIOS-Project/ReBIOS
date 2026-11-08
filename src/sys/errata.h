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

#ifndef SYS__ERRATA_H
#define SYS__ERRATA_H

// The Zen generation of an AMD processor, from 1, by its family and model as
// Linux 7.3 tells them (arch/x86/kernel/cpu/amd.c, bsp_init_amd()), or 0.
unsigned int errata_amd_zen(void);

// Works around the errata of each processor as it starts, with the chicken
// bits Linux 7.3 sets on bare metal (arch/x86/kernel/cpu/amd.c, init_amd()).
// Needs cpu_tables_load().
void errata_cpu_init(void);

#endif
