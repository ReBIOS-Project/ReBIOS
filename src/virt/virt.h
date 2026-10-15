// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of ReBIOS.
// Copyright (C) 2026 Mintsuki and contributors
//
// ReBIOS is free software: you can redistribute it and/or modify it under the
// terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version.
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

// Whether processors wait for start-up IPIs in the guest themselves, in
// VMX's wait-for-SIPI state.
bool virt_sipi_in_guest(void);

// The intercept bitmaps every vCPU shares, in the vendor's format.
uint64_t virt_io_bitmap(void);
uint64_t virt_msr_bitmap(void);
void virt_intercept_io(uint16_t port, unsigned count, bool intercept);
void virt_intercept_msr(uint32_t msr, bool read, bool write);

#endif
