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

#ifndef VIRT__EVENT_H
#define VIRT__EVENT_H

#include <stdbool.h>
#include <virt/vcpu.h>

// Notes the event whose delivery the exit cut short, before the exit is
// handled.
void event_exit(struct vcpu *v);

// Sets up what the guest gets on the coming entry, after the exit is handled,
// or an entry that delivers nothing and asks for no window to deliver in.
void event_entry(struct vcpu *v);
void event_hold(struct vcpu *v);
// As event_entry(), delivering NMIs and interrupts only where told to, for an
// entry that runs the guest's guest: what it does not take as exits, the
// processor would deliver to it.
void event_entry_for(struct vcpu *v, bool nmis, bool interrupts);

// An NMI that made the guest exit.
void event_nmi(struct vcpu *v);

// Whether something ends a HLT: an NMI, an INIT, or an interrupt the guest
// takes.
bool event_halt_ends(struct vcpu *v);

// Takes back what event_entry() set up, for an entry that did not happen.
void event_cancel(struct vcpu *v);

// An interrupt the processor took, which all make the guest exit.
void event_interrupt(uint8_t vector);

// Whether the exit cut an event's delivery short, so that what it reports
// happened delivering the event rather than running an instruction.
bool event_cut_short(struct vcpu *v);
// Delivers such an event, an interrupt or an exception, as a real-mode
// processor does, through what memory is to the hypervisor's emulation, as
// where it reached emulated memory: false where it cannot.
bool event_deliver_real_mode(struct vcpu *v);

#endif
