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

#ifndef HW__ISA_H
#define HW__ISA_H

// The two 8259s' interrupt lines, and those that the IBM Personal System/2
// Hardware Interface Technical Reference, Common Interfaces (84F9735) of
// September 1991 assigns in Interrupt Controller (Type 1), Figure 1.
#define ISA_IRQS 16
#define ISA_IRQ_TIMER 0
#define ISA_IRQ_KEYBOARD 1
#define ISA_IRQ_CASCADE 2
#define ISA_IRQ_RTC 8
#define ISA_IRQ_AUX 12
#define ISA_IRQ_FPU 13

#endif
