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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <virt/vioapic.h>
#include <hw/ioapic.h>
#include <lib/heap.h>
#include <lib/log.h>
#include <lib/spinlock.h>
#include <sys/mm.h>
#include <virt/gpt.h>
#include <virt/irq.h>
#include <virt/mmio.h>
#include <virt/vcpu.h>

#define REG_SELECT 0x00
#define REG_WINDOW 0x10
#define REG_EOI 0x40

// The bits of an entry the guest writes; the others the I/O APIC reports.
#define RTE_WRITABLE \
    (~(IOAPIC_RTE_PENDING | IOAPIC_RTE_REMOTE_IRR) & ~((uint64_t)0x7fff << 17))
#define ID_WRITABLE ((uint32_t)0xff000000)

struct vioapic {
    struct ioapic *host;
    struct spinlock lock;
    uint32_t select;
    uint32_t id_reg;
    uint64_t *rtes;
};

static struct vioapic *vioapics;
static size_t count;

static const struct mmio_ops mmio_ops;

void vioapic_init(void) {
    count = ioapic_count();
    vioapics = kmalloc(count * sizeof(struct vioapic));
    for (size_t i = 0; i < count; i++) {
        struct vioapic *vio = &vioapics[i];
        struct ioapic *io = ioapic_get(i);
        *vio = (struct vioapic){ .host = io, .id_reg = io->id_reg };
        vio->rtes = kmalloc(io->pins * sizeof(uint64_t));
        for (size_t pin = 0; pin < io->pins; pin++) {
            vio->rtes[pin] = IOAPIC_RTE_MASKED;
        }
        // Every access to the page exits.
        mmio_register(io->base, PAGE_SIZE, &mmio_ops, vio);
        gpt_map(io->base, 0, PAGE_SIZE, 0, 0);
    }
    gpt_flush();
}

bool vioapic_rte(uint32_t gsi, uint64_t *rte) {
    for (size_t i = 0; i < count; i++) {
        struct ioapic *io = vioapics[i].host;
        if (gsi >= io->gsi_base && gsi - io->gsi_base < io->pins) {
            *rte = __atomic_load_n(
                &vioapics[i].rtes[gsi - io->gsi_base], __ATOMIC_ACQUIRE
            );
            return true;
        }
    }
    return false;
}

static uint32_t index_read(struct vioapic *vio, uint32_t index) {
    struct ioapic *io = vio->host;
    switch (index) {
        case IOAPIC_REG_ID: {
            return vio->id_reg;
        }
        case IOAPIC_REG_VERSION: {
            return io->version;
        }
        case IOAPIC_REG_ARBITRATION: {
            return io->arbitration;
        }
        default: {
            break;
        }
    }
    if (index < IOAPIC_REG_REDIR(0) || index >= IOAPIC_REG_REDIR(io->pins)) {
        return 0;
    }
    uint32_t pin = (index - IOAPIC_REG_REDIR(0)) / 2;
    uint64_t rte = vio->rtes[pin];
    if ((index & 1) != 0) {
        return (uint32_t)(rte >> 32);
    }
    // A level-triggered interrupt waits for the guest's EOI masked at the
    // host, which the guest's Remote IRR shows instead.
    uint64_t status = 0;
    if ((rte & IOAPIC_RTE_MASKED) == 0) {
        status = ioapic_rte_read(io, pin) & IOAPIC_RTE_PENDING;
    }
    if (irq_remote_irr(io->gsi_base + pin)) {
        status |= IOAPIC_RTE_REMOTE_IRR;
    }
    return (uint32_t)(rte | status);
}

static void index_write(struct vioapic *vio, uint32_t index, uint32_t value) {
    struct ioapic *io = vio->host;
    if (index == IOAPIC_REG_ID) {
        vio->id_reg = value & ID_WRITABLE;
        return;
    }
    if (index < IOAPIC_REG_REDIR(0) || index >= IOAPIC_REG_REDIR(io->pins)) {
        return;
    }
    uint32_t pin = (index - IOAPIC_REG_REDIR(0)) / 2;
    uint64_t rte = vio->rtes[pin];
    if ((index & 1) != 0) {
        rte = (rte & (uint64_t)0xffffffff) | (uint64_t)value << 32;
    } else {
        rte = (rte & ~(uint64_t)0xffffffff) | value;
    }
    rte &= RTE_WRITABLE;
    __atomic_store_n(&vio->rtes[pin], rte, __ATOMIC_RELEASE);
    irq_gsi_changed(io->gsi_base + pin);
}

static uint32_t reg_read(struct vioapic *vio, uint32_t offset) {
    switch (offset) {
        case REG_SELECT: {
            return vio->select;
        }
        case REG_WINDOW: {
            return index_read(vio, vio->select);
        }
        default: {
            return 0;
        }
    }
}

static void reg_write(struct vioapic *vio, uint32_t offset, uint32_t value) {
    switch (offset) {
        case REG_SELECT: {
            vio->select = value & 0xff;
            break;
        }
        case REG_WINDOW: {
            index_write(vio, vio->select, value);
            break;
        }
        case REG_EOI: {
            irq_mirror_eoi((uint8_t)value);
            break;
        }
        default: {
            break;
        }
    }
}

// Registers are 32 bits wide; narrower accesses take part of one.
static uint64_t mmio_read(
    struct vcpu *v, void *ctx, uint64_t offset, size_t size
) {
    (void)v;
    struct vioapic *vio = ctx;
    spinlock_acquire(&vio->lock);
    uint64_t value;
    if (size == 8) {
        value = reg_read(vio, (uint32_t)offset)
            | (uint64_t)reg_read(vio, (uint32_t)offset + 4) << 32;
    } else {
        uint64_t mask = ((uint64_t)1 << (size * 8)) - 1;
        value = (reg_read(vio, (uint32_t)offset & ~(uint32_t)3)
            >> ((offset & 3) * 8)) & mask;
    }
    spinlock_release(&vio->lock);
    return value;
}

static void mmio_write(
    struct vcpu *v, void *ctx, uint64_t offset, size_t size, uint64_t value
) {
    (void)v;
    struct vioapic *vio = ctx;
    spinlock_acquire(&vio->lock);
    if (size == 8) {
        reg_write(vio, (uint32_t)offset, (uint32_t)value);
        reg_write(vio, (uint32_t)offset + 4, (uint32_t)(value >> 32));
    } else {
        unsigned int shift = (offset & 3) * 8;
        uint32_t mask = (uint32_t)(size == 4 ? (uint32_t)0xffffffff
            : ((uint32_t)1 << (size * 8)) - 1) << shift;
        uint32_t old = size == 4 ? 0
            : reg_read(vio, (uint32_t)offset & ~(uint32_t)3);
        reg_write(
            vio, (uint32_t)offset & ~(uint32_t)3,
            (old & ~mask) | (((uint32_t)value << shift) & mask)
        );
    }
    spinlock_release(&vio->lock);
}

static const struct mmio_ops mmio_ops = { mmio_read, mmio_write, false };
