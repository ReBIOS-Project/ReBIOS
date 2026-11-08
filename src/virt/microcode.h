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

#ifndef VIRT__MICROCODE_H
#define VIRT__MICROCODE_H

#include <stdint.h>

struct vcpu;

// Loads the microcode update that the guest wrote the linear address of to
// Intel's IA32_BIOS_UPDT_TRIG or AMD's patch loader MSR on the processor the
// vCPU runs on, as its OS would on bare metal, which VMX would not let it do
// itself: from a copy of the guest's, as the processor reads it at a linear
// address of its own. An update the guest's memory does not hold loads
// nothing.
void microcode_load(struct vcpu *v, uint64_t address);

#endif
