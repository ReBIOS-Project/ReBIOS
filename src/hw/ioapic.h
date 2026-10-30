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

#ifndef HW__IOAPIC_H
#define HW__IOAPIC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <lib/spinlock.h>

#define IOAPIC_REG_ID 0x00
#define IOAPIC_REG_VERSION 0x01
#define IOAPIC_REG_ARBITRATION 0x02
#define IOAPIC_REG_REDIR(n) (0x10 + 2 * (n))

// Redirection table entry fields.
#define IOAPIC_RTE_VECTOR_MASK ((uint64_t)0xff)
#define IOAPIC_RTE_DELIVERY_MASK ((uint64_t)7 << 8)
#define IOAPIC_RTE_DELIVERY_FIXED ((uint64_t)0 << 8)
#define IOAPIC_RTE_DELIVERY_LOWEST ((uint64_t)1 << 8)
#define IOAPIC_RTE_DELIVERY_EXTINT ((uint64_t)7 << 8)
#define IOAPIC_RTE_LOGICAL ((uint64_t)1 << 11)
#define IOAPIC_RTE_PENDING ((uint64_t)1 << 12)
#define IOAPIC_RTE_ACTIVE_LOW ((uint64_t)1 << 13)
#define IOAPIC_RTE_REMOTE_IRR ((uint64_t)1 << 14)
#define IOAPIC_RTE_LEVEL ((uint64_t)1 << 15)
#define IOAPIC_RTE_MASKED ((uint64_t)1 << 16)
#define IOAPIC_RTE_DEST_SHIFT 56

// The I/O APIC versions from which the EOI register is there.
#define IOAPIC_VERSION_EOI 0x20

struct ioapic {
    uintptr_t base;
    uint32_t gsi_base;
    uint32_t pins;
    // The ID the MADT gives it.
    uint8_t id;
    // Its ID, version, and arbitration registers, as the firmware left them.
    uint32_t id_reg;
    uint32_t version;
    uint32_t arbitration;
    struct spinlock lock;
};

// Finds the I/O APICs the MADT lists, and masks every redirection entry, as a
// legacy BIOS hands them over, the 8259 delivering interrupts. Needs
// acpi_init().
void ioapic_init(void);

size_t ioapic_count(void);
struct ioapic *ioapic_get(size_t index);
// The I/O APIC a GSI goes to, and its pin there, or NULL.
struct ioapic *ioapic_for_gsi(uint32_t gsi, uint32_t *pin);

uint64_t ioapic_rte_read(struct ioapic *io, uint32_t pin);
void ioapic_rte_write(struct ioapic *io, uint32_t pin, uint64_t rte);
// An EOI for the level-triggered entries with the vector, where the I/O APIC
// has the register for it.
void ioapic_eoi(struct ioapic *io, uint8_t vector);

#endif
