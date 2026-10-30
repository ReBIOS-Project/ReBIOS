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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Picks VMX or SVM and sets up what all vCPUs share. Returns false if neither
// is usable on this processor.
bool virt_init(void);

// Enables virtualisation on the calling CPU.
void virt_cpu_init(void);

// The intercept bitmaps every vCPU shares, in the vendor's format.
uintptr_t virt_io_bitmap(void);
uintptr_t virt_msr_bitmap(void);
void virt_intercept_io(uint16_t port, size_t count, bool intercept);
// Makes the guest's reads, writes, or both of an MSR exit, for a module to
// handle them, whatever virt_pass_msr() says; the others follow that.
void virt_intercept_msr(uint32_t msr, bool read, bool write);
// Lets the guest's accesses to an MSR no module has chosen for reach the
// processor without exits.
void virt_pass_msr(uint32_t msr);
// The MSR bitmap for vCPUs whose x2APIC the processor virtualises, made by the
// first of these calls, which pass reads or writes of an x2APIC MSR.
uintptr_t virt_msr_bitmap_x2apic(void);
void virt_x2apic_msr_passed(uint32_t msr, bool read, bool write);
// The same, for vCPUs whose IPIs it virtualises too, made once that one is
// done by passing the ICR's MSR as well.
uintptr_t virt_msr_bitmap_x2apic_icr(void);
void virt_x2apic_icr_passed(uint32_t msr);

// The reset a triple fault makes on a PC, held a while first if the log is on
// the screen (log_take_screen()), so that what led there can be read.
__attribute__((noreturn)) void virt_reset(bool shown);

#endif
