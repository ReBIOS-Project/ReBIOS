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

#ifndef VIRT__RUN_H
#define VIRT__RUN_H

#include <stdint.h>
#include <virt/vcpu.h>

// Right after the hypervisor carried out an access to a region that begins
// runs, carries out the instructions that follow while they keep reaching such
// regions or the emulated VGA's ports, the rest of their operands in the
// guest's RAM, for a short while at most: only those it carries out as the
// processor would, faulting nowhere. The run ends before any other, which the
// guest then runs itself.
void run_begin(struct vcpu *v);
// For profile.c: what runs did over the last ms milliseconds.
void run_report(uint64_t ms);

#endif
