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
// You should have received a copy of the GNU General Public License along with
// ReBIOS. If not, see <https://www.gnu.org/licenses/>.

#ifndef VIRT__PMU_H
#define VIRT__PMU_H

#include <stdbool.h>
#include <stdint.h>
#include <virt/vcpu.h>

// Finds the processor's performance counters, and on Intel's has the guest's
// writes of what they count go through pmu_programmed().
void pmu_init(void);
// Whether an MSR is one whose writes pmu_programmed() follows.
bool pmu_programming(uint32_t msr);
// Works out which of the vCPU's counters count retired instructions and
// retired branch instructions, as the guest has programmed them.
void pmu_programmed(struct vcpu *v);
// Counts instructions the hypervisor carried out for the guest, of which
// branches were branch instructions, in the guest's counters of those events,
// as the processor would have counted them at the guest's CPL, with the
// overflows' status and interrupts.
void pmu_retired(struct vcpu *v, uint64_t instructions, uint64_t branches);
// Whether an AMD event select's host/guest-only field lets it count in the
// vCPU's mode now: with neither bit, always; with either and the vCPU's
// EFER.SVME clear, never; else with both, or the one of its mode (APM 24593
// rev. 3.46, Table 13-4), as Linux 7.3's KVM has it
// (amd_pmc_is_disabled_in_current_mode()).
bool pmu_amd_counts_now(struct vcpu *v, uint64_t evtsel);

#endif
