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

#include <stdint.h>
#include <stdbool.h>
#include <hw/ioapic.h>
#include <lib/heap.h>
#include <lib/log.h>
#include <lib/spinlock.h>
#include <sys/mm.h>
#include <virt/gpt.h>
#include <virt/insn.h>
#include <virt/irq.h>
#include <virt/vcpu.h>
#include <virt/vioapic.h>

#define REG_SELECT 0x00
#define REG_WINDOW 0x10
#define REG_EOI 0x40

// The bits of an entry the guest writes; the others the I/O APIC reports.
#define RTE_WRITABLE (~(IOAPIC_RTE_PENDING | IOAPIC_RTE_REMOTE_IRR) & ~(0x7fffull << 17))
#define ID_WRITABLE 0xff000000u

struct vioapic {
    struct ioapic *host;
    struct spinlock lock;
    uint32_t select;
    uint32_t id_reg;
    uint64_t *rtes;
};

static struct vioapic *vioapics;
static uint32_t count;

void vioapic_init(void) {
    count = ioapic_count();
    vioapics = kmalloc(count * sizeof(*vioapics));
    for (uint32_t i = 0; i < count; i++) {
        struct vioapic *vio = &vioapics[i];
        struct ioapic *io = ioapic_get(i);
        *vio = (struct vioapic){.host = io, .id_reg = io->id_reg};
        vio->rtes = kmalloc(io->pins * sizeof(*vio->rtes));
        for (uint32_t pin = 0; pin < io->pins; pin++) {
            vio->rtes[pin] = IOAPIC_RTE_MASKED;
        }
        // Every access to the page exits.
        gpt_map(io->base, 0, PAGE_SIZE, 0, 0);
    }
    gpt_flush();
}

bool vioapic_rte(uint32_t gsi, uint64_t *rte) {
    for (uint32_t i = 0; i < count; i++) {
        struct ioapic *io = vioapics[i].host;
        if (gsi >= io->gsi_base && gsi - io->gsi_base < io->pins) {
            *rte = __atomic_load_n(&vioapics[i].rtes[gsi - io->gsi_base], __ATOMIC_ACQUIRE);
            return true;
        }
    }
    return false;
}

static struct vioapic *find(uint64_t gpa) {
    for (uint32_t i = 0; i < count; i++) {
        if ((gpa & ~(uint64_t)(PAGE_SIZE - 1)) == vioapics[i].host->base) {
            return &vioapics[i];
        }
    }
    return NULL;
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
    if (index & 1) {
        return (uint32_t)(rte >> 32);
    }
    // What the host's entry reports of its interrupt, where it is the
    // guest's.
    uint64_t status = 0;
    if (!(rte & IOAPIC_RTE_MASKED)) {
        status = ioapic_rte_read(io, pin) & (IOAPIC_RTE_PENDING | IOAPIC_RTE_REMOTE_IRR);
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
    if (index & 1) {
        rte = (rte & 0xffffffffull) | (uint64_t)value << 32;
    } else {
        rte = (rte & ~0xffffffffull) | value;
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
            ioapic_eoi(vio->host, (uint8_t)value);
            break;
        }
        default: {
            break;
        }
    }
}

bool vioapic_access(struct vcpu *v, uint64_t gpa) {
    struct vioapic *vio = find(gpa);
    if (vio == NULL) {
        return false;
    }
    struct insn insn;
    if (!insn_decode(v, &insn)) {
        panic("Cannot decode an access to I/O APIC %u at %#llx\n", vio->host->id,
              (unsigned long long)vcpu_rip(v));
    }

    // Registers are 32 bits wide; narrower accesses take part of one. Read
    // and modify instructions may fault as reads, so the instruction tells.
    uint32_t offset = gpa & (PAGE_SIZE - 4);
    unsigned shift = (gpa & 3) * 8;
    struct insn_store store;
    struct insn_load load;
    spinlock_acquire(&vio->lock);
    if (insn_store(v, &insn, &store) && (gpa & 3) + store.size <= 4) {
        uint32_t old = store.op == INSN_STORE_MOV && store.size == 4 ? 0 : reg_read(vio, offset);
        uint32_t mask = (uint32_t)(store.size == 4 ? 0xffffffffu : (1u << (store.size * 8)) - 1)
                        << shift;
        uint64_t value = insn_store_apply(v, &store, (old & mask) >> shift);
        reg_write(vio, offset, (old & ~mask) | (((uint32_t)value << shift) & mask));
    } else if (insn_load(&insn, &load) && (gpa & 3) + load.size <= 4) {
        insn_load_complete(v, &load, reg_read(vio, offset) >> shift);
    } else {
        printf("An access to I/O APIC %u register %#x that is not emulated, skipped\n",
               vio->host->id, offset);
    }
    spinlock_release(&vio->lock);
    vcpu_advance(v, insn.length);
    return true;
}
