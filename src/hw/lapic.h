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

#ifndef HW__LAPIC_H
#define HW__LAPIC_H

#include <stdint.h>
#include <stdbool.h>

// Interrupt command register fields.
#define ICR_VECTOR_MASK 0xffu
#define ICR_DELIVERY_MASK (7u << 8)
#define ICR_DELIVERY_FIXED (0u << 8)
#define ICR_DELIVERY_NMI (4u << 8)
#define ICR_DELIVERY_INIT (5u << 8)
#define ICR_DELIVERY_STARTUP (6u << 8)
#define ICR_LOGICAL (1u << 11)
#define ICR_PENDING (1u << 12)
#define ICR_ASSERT (1u << 14)
#define ICR_LEVEL (1u << 15)
#define ICR_SHORTHAND_MASK (3u << 18)
#define ICR_SHORTHAND_NONE (0u << 18)
#define ICR_SHORTHAND_SELF (1u << 18)
#define ICR_SHORTHAND_ALL (2u << 18)
#define ICR_SHORTHAND_OTHERS (3u << 18)

// The calling processor's local APIC: whether it is in x2APIC mode, and its
// APIC ID as CPUID reports it.
bool lapic_x2apic(void);
uint32_t lapic_cpu_apic_id(void);

// Sends an IPI from the calling processor, to the APIC ID dest unless the
// command names a shorthand, and waits for the APIC to take it.
void lapic_send_ipi(uint32_t dest, uint32_t command);

// The highest vector in service at the calling processor's local APIC, or 0,
// and whether a vector is.
uint8_t lapic_highest_isr(void);
bool lapic_in_service(uint8_t vector);

// Puts the calling processor's local APIC in xAPIC mode, as a legacy BIOS
// hands it over, unless the processor keeps it in x2APIC mode.
void lapic_xapic_mode(void);

// Resets the calling processor's local APIC registers as an INIT does, which
// INITs taken as VM exits do not.
void lapic_init_reset(void);

#endif
