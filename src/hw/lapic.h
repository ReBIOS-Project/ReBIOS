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

#ifndef HW__LAPIC_H
#define HW__LAPIC_H

#include <stdbool.h>
#include <stdint.h>

// Registers by their xAPIC offsets; their x2APIC MSRs are 800h + offset / 16.
#define APIC_ID 0x20
#define APIC_VERSION 0x30
#define APIC_TPR 0x80
#define APIC_EOI 0xb0
#define APIC_LDR 0xd0
#define APIC_DFR 0xe0
#define APIC_SVR 0xf0
#define APIC_ISR 0x100
#define APIC_TMR 0x180
#define APIC_IRR 0x200
#define APIC_ESR 0x280
#define APIC_LVT_CMCI 0x2f0
#define APIC_ICR_LOW 0x300
#define APIC_ICR_HIGH 0x310
#define APIC_LVT_TIMER 0x320
#define APIC_LVT_THERMAL 0x330
#define APIC_LVT_PERF 0x340
#define APIC_LVT_LINT0 0x350
#define APIC_LVT_LINT1 0x360
#define APIC_LVT_ERROR 0x370
#define APIC_TIMER_INITIAL 0x380
#define APIC_TIMER_CURRENT 0x390
#define APIC_TIMER_DIVIDE 0x3e0

#define APIC_SVR_ENABLED ((uint32_t)1 << 8)
#define APIC_LVT_MASKED ((uint32_t)1 << 16)

// Interrupt command register fields.
#define ICR_VECTOR_MASK ((uint32_t)0xff)
#define ICR_DELIVERY_MASK ((uint32_t)7 << 8)
#define ICR_DELIVERY_FIXED ((uint32_t)0 << 8)
#define ICR_DELIVERY_LOWEST ((uint32_t)1 << 8)
#define ICR_DELIVERY_NMI ((uint32_t)4 << 8)
#define ICR_DELIVERY_INIT ((uint32_t)5 << 8)
#define ICR_DELIVERY_STARTUP ((uint32_t)6 << 8)
#define ICR_LOGICAL ((uint32_t)1 << 11)
#define ICR_PENDING ((uint32_t)1 << 12)
#define ICR_ASSERT ((uint32_t)1 << 14)
#define ICR_LEVEL ((uint32_t)1 << 15)
#define ICR_SHORTHAND_MASK ((uint32_t)3 << 18)
#define ICR_SHORTHAND_NONE ((uint32_t)0 << 18)
#define ICR_SHORTHAND_SELF ((uint32_t)1 << 18)
#define ICR_SHORTHAND_ALL ((uint32_t)2 << 18)
#define ICR_SHORTHAND_OTHERS ((uint32_t)3 << 18)

// Message-signalled interrupts' address; their data has the interrupt command
// register's vector, delivery mode, level, and trigger mode fields (SDM
// 325462-093 Vol. 3A, 13.11).
#define MSI_ADDRESS_BASE ((uint32_t)0xfee00000)
#define MSI_ADDRESS_MASK ((uint32_t)0xfff00000)
#define MSI_ADDRESS_DEST_SHIFT 12
#define MSI_ADDRESS_REDIRECT ((uint32_t)1 << 3)
#define MSI_ADDRESS_LOGICAL ((uint32_t)1 << 2)

// The calling processor's local APIC: whether it is in x2APIC mode, and its
// APIC ID as CPUID reports it.
bool lapic_x2apic(void);
uint32_t lapic_cpu_apic_id(void);
// Whether the processor has x2APIC mode, and has its xAPIC mode turned off for
// good.
bool lapic_x2apic_supported(void);
bool lapic_xapic_locked(void);

uint32_t lapic_read(uint32_t reg);
void lapic_write(uint32_t reg, uint32_t value);

// Sends an IPI from the calling processor, to the APIC ID dest unless the
// command names a shorthand, and waits for the APIC to take it.
void lapic_send_ipi(uint32_t dest, uint32_t command);
void lapic_eoi(void);

// Takes the calling processor's local APIC for the hypervisor, in xAPIC mode
// where it can be: enabled with the spurious vector, the timer disarmed on its
// vector, LINT0 masked, nothing in service, LINT1 as it was.
void lapic_host_init(uint8_t spurious_vector, uint8_t timer_vector);
// Arms the calling processor's timer for when the TSC reaches tsc, or disarms
// it for 0. It may fire a little early where it has no TSC-deadline mode.
void lapic_timer_arm(uint64_t tsc);
// How many TSC ticks that many ticks of the timer's clock last, and back.
uint64_t lapic_timer_to_tsc(uint64_t ticks);
uint64_t lapic_timer_from_tsc(uint64_t tsc);
// Switches the calling processor's local APIC to x2APIC mode or, where it can
// be, xAPIC mode, keeping what the hypervisor set. Returns whether it is in
// x2APIC mode.
bool lapic_host_mode(bool x2apic);

#endif
