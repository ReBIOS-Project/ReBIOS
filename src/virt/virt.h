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

#ifndef VIRT__VIRT_H
#define VIRT__VIRT_H

#include <stdint.h>
#include <stdbool.h>

// Picks VMX or SVM and sets up what all vCPUs share. Returns false if neither
// is usable on this processor.
bool virt_init(void);

// Enables virtualisation on the calling CPU.
void virt_cpu_init(void);

// The intercept bitmaps every vCPU shares, in the vendor's format.
uint64_t virt_io_bitmap(void);
uint64_t virt_msr_bitmap(void);
void virt_intercept_io(uint16_t port, unsigned count, bool intercept);
void virt_intercept_msr(uint32_t msr, bool read, bool write);
// VMX's MSR bitmap for vCPUs whose x2APIC the processor virtualises, made by
// the first of these calls, which pass reads or writes of an x2APIC MSR.
uint64_t virt_msr_bitmap_x2apic(void);
void virt_x2apic_msr_passed(uint32_t msr, bool read, bool write);

#endif
