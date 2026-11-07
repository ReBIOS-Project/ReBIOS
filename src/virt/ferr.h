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

#ifndef VIRT__FERR_H
#define VIRT__FERR_H

#include <stdbool.h>

struct vcpu;

// The PC/AT's report of x87 errors to a guest that clears CR0.NE: an unmasked
// error asserts FERR#, which the chipset latches into IRQ13, and a write to
// port F0h then lowers IRQ13 and asserts IGNNE#, which has the processor
// ignore the error, until the error flags' clearing takes FERR# away (SDM
// 325462-093 Vol. 1, 8.7.2; ICH10 datasheet 319973-003, 5.12.1.3, 13.7.4). The
// processor under the guest keeps CR0.NE set and raises #MF instead, which the
// hypervisor catches while the guest's CR0.NE is clear. After io_init().
void ferr_init(void);

// Whether the guest's #MF is the hypervisor's to catch: its CR0.NE is clear.
bool ferr_catches(struct vcpu *v);
// An #MF caught: FERR# goes up and the processor waits before the instruction
// until an interrupt or NMI comes, or with IGNNE# up, the instruction runs as
// the error were not there.
void ferr_fault(struct vcpu *v);
// Whether the vCPU steps an instruction that ignores the error, and the end of
// that step.
bool ferr_stepping(struct vcpu *v);
void ferr_step_done(struct vcpu *v);
// Before each entry: FERR# goes with the error flags the guest cleared.
void ferr_follow(struct vcpu *v);

#endif
