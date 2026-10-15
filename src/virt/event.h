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

#ifndef VIRT__EVENT_H
#define VIRT__EVENT_H

#include <stdbool.h>
#include <virt/vcpu.h>

// Notes the event whose delivery the exit cut short, before the exit is
// handled.
void event_exit(struct vcpu *v);

// Sets up what the guest gets on the coming entry, after the exit is handled.
void event_entry(struct vcpu *v);

// Whether the exit cut an event's delivery short, so that what it reports
// happened delivering the event rather than running an instruction.
bool event_cut_short(struct vcpu *v);

#endif
