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

#ifndef VIRT__MWAIT_H
#define VIRT__MWAIT_H

struct vcpu;

// The guest's MONITOR and MWAIT, and AMD's MONITORX and MWAITX, which exit to
// the hypervisor: MONITOR notes the line it arms, and MWAIT waits in the
// hypervisor, the threads running meanwhile, then in the processor's own MWAIT
// with the guest's hint, on the line armed again, as VM entries and exits
// clear the guest's own monitoring (SDM 325462-093 Vol. 3C, 29.3.3 and
// 30.5.6). After cpuid_init().
void mwait_init(void);
void mwait_monitor(struct vcpu *v);
void mwait_wait(struct vcpu *v);

#endif
