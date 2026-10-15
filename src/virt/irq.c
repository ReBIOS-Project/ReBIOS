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
#include <stddef.h>
#include <stdbool.h>
#include <uacpi/acpi.h>
#include <uacpi/tables.h>
#include <uacpi/utilities.h>
#include <memory.h>
#include <hw/acpi.h>
#include <hw/ioapic.h>
#include <hw/lapic.h>
#include <lib/heap.h>
#include <lib/log.h>
#include <lib/spinlock.h>
#include <sys/cpu.h>
#include <sys/smp.h>
#include <sys/x86.h>
#include <virt/event.h>
#include <virt/io.h>
#include <virt/irq.h>
#include <virt/pic.h>
#include <virt/vcpu.h>
#include <virt/vioapic.h>

#define ISA_IRQS 16
#define CASCADE_IRQ 2
#define XAPIC_MAX_ID 0xff

// The vectors the host's interrupts for the 8259s arrive on: those guests
// never use, of exceptions, but #MC's and #SX's, which the host takes as
// exceptions. The cascade has no source, nor IRQ 13 (FERR#).
static const uint8_t irq_vectors[ISA_IRQS] = {
    0x10, 0x11, 0, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0, 0x1d, 0x1f,
};

enum owner {
    OWNER_NONE,
    // The guest's I/O APIC entry, as it is.
    OWNER_MIRROR,
    OWNER_PIC,
};

struct route {
    struct ioapic *io;
    uint32_t pin;
    // The 8259 input it feeds, or -1.
    int irq;
    bool level;
    bool active_low;
    enum owner owner;
    // A level-triggered line the 8259 path took, held masked at the host
    // until the guest is done with it.
    bool asserted;
    uint64_t host_rte;
};

static struct route *routes;
static uint32_t route_count;
static struct spinlock lock;
static struct cpu *bsp;
// Whether the boot processor's physical interrupts exit, and whether it has
// routing to do that only it can.
static bool bsp_exiting;
static volatile bool bsp_dirty;
static volatile bool extint_asserted;
// The processor an unmasked I/O APIC ExtINT entry sends the 8259's
// interrupts to, if any.
static struct cpu *extint_target;

static void override(const struct acpi_madt_interrupt_source_override *iso) {
    if (iso->bus != 0 || iso->source >= ISA_IRQS || iso->gsi >= route_count
        || routes[iso->gsi].io == NULL) {
        printf("Ignoring the override of bus %u source %u to GSI %u\n", iso->bus, iso->source,
               iso->gsi);
        return;
    }
    // The source leaves the GSI of its own number.
    for (uint32_t gsi = 0; gsi < route_count; gsi++) {
        if (routes[gsi].irq == iso->source) {
            routes[gsi].irq = -1;
        }
    }
    struct route *r = &routes[iso->gsi];
    r->irq = iso->source;
    // What conforms to ISA is active high and edge-triggered.
    r->active_low = (iso->flags & ACPI_MADT_POLARITY_MASK) == ACPI_MADT_POLARITY_ACTIVE_LOW;
    r->level = (iso->flags & ACPI_MADT_TRIGGERING_MASK) == ACPI_MADT_TRIGGERING_LEVEL;
    printf("ISA IRQ %u is GSI %u, active %s, %s-triggered\n", iso->source, iso->gsi,
           r->active_low ? "low" : "high", r->level ? "level" : "edge");
}

static void routes_init(void) {
    for (uint32_t i = 0; i < ioapic_count(); i++) {
        struct ioapic *io = ioapic_get(i);
        if (io->gsi_base + io->pins > route_count) {
            route_count = io->gsi_base + io->pins;
        }
    }
    routes = kmalloc(route_count * sizeof(*routes));
    for (uint32_t gsi = 0; gsi < route_count; gsi++) {
        routes[gsi] = (struct route){.irq = -1, .host_rte = IOAPIC_RTE_MASKED};
        uint32_t pin;
        struct ioapic *io = ioapic_for_gsi(gsi, &pin);
        if (io != NULL) {
            routes[gsi].io = io;
            routes[gsi].pin = pin;
        }
    }
    for (int irq = 0; irq < ISA_IRQS && (uint32_t)irq < route_count; irq++) {
        if (irq != CASCADE_IRQ && routes[irq].io != NULL) {
            routes[irq].irq = irq;
        }
    }

    struct uacpi_table table;
    if (!acpi_tables_ready()
        || uacpi_table_find_by_signature(ACPI_MADT_SIGNATURE, &table) != UACPI_STATUS_OK) {
        return;
    }
    const uint8_t *madt = table.ptr;
    uint32_t length = table.hdr->length;
    for (uint32_t off = sizeof(struct acpi_madt); off + sizeof(struct acpi_entry_hdr) <= length;) {
        const struct acpi_entry_hdr *hdr = (const void *)(madt + off);
        if (hdr->length < sizeof(*hdr) || hdr->length > length - off) {
            break;
        }
        if (hdr->type == ACPI_MADT_ENTRY_TYPE_INTERRUPT_SOURCE_OVERRIDE
            && hdr->length >= sizeof(struct acpi_madt_interrupt_source_override)) {
            struct acpi_madt_interrupt_source_override iso;
            memcpy(&iso, hdr, sizeof(iso));
            override(&iso);
        }
        off += hdr->length;
    }
    uacpi_table_unref(&table);
}

bool irq_host_vector(uint8_t vector) {
    for (unsigned irq = 0; irq < ISA_IRQS; irq++) {
        if (irq_vectors[irq] != 0 && irq_vectors[irq] == vector) {
            return true;
        }
    }
    return false;
}

// What the host's entry for a GSI is to hold, and who owns it.
static uint64_t desired(uint32_t gsi, const struct route *r, enum owner *owner) {
    uint64_t rte;
    if (vioapic_rte(gsi, &rte) && !(rte & IOAPIC_RTE_MASKED)
        && (rte & IOAPIC_RTE_DELIVERY_MASK) != IOAPIC_RTE_DELIVERY_EXTINT) {
        *owner = OWNER_MIRROR;
        return rte;
    }
    // While the guest has any 8259 input unmasked, its masked inputs latch
    // requests as well.
    if (r->irq >= 0 && irq_vectors[r->irq] != 0 && pic_in_use() && bsp->apic_id <= XAPIC_MAX_ID) {
        *owner = OWNER_PIC;
        rte = irq_vectors[r->irq] | (uint64_t)bsp->apic_id << IOAPIC_RTE_DEST_SHIFT;
        // The guest's ELCR says how its 8259 senses the line, the MADT the
        // line's polarity.
        if (pic_level_triggered((unsigned)r->irq)) {
            rte |= IOAPIC_RTE_LEVEL;
        }
        if (r->active_low) {
            rte |= IOAPIC_RTE_ACTIVE_LOW;
        }
        if (r->asserted) {
            rte |= IOAPIC_RTE_MASKED;
        }
        return rte;
    }
    *owner = OWNER_NONE;
    return IOAPIC_RTE_MASKED;
}

static bool pic_path_wanted(void) {
    for (uint32_t gsi = 0; gsi < route_count; gsi++) {
        enum owner owner;
        if (routes[gsi].io != NULL && !(desired(gsi, &routes[gsi], &owner) & IOAPIC_RTE_MASKED)
            && owner == OWNER_PIC) {
            return true;
        }
    }
    return false;
}

// Level-triggered lines the 8259 path took get a fresh look once the guest
// is done with them: acknowledged, then unmasked and out of service. If still
// asserted, they interrupt again when unmasked at the host.
static void resample(void) {
    for (unsigned irq = 0; irq < ISA_IRQS; irq++) {
        if (!pic_acknowledged(irq) || pic_masked(irq) || pic_in_service(irq)) {
            continue;
        }
        bool deasserted = false;
        for (uint32_t gsi = 0; gsi < route_count; gsi++) {
            if (routes[gsi].irq == (int)irq && routes[gsi].asserted) {
                routes[gsi].asserted = false;
                deasserted = true;
            }
        }
        if (deasserted) {
            pic_set_irq(irq, false);
        }
    }
}

// Brings the host's entries up to date. Only the boot processor unmasks the
// 8259 path's, once its physical interrupts exit; others leave that to it.
static void update(void) {
    bool on_bsp = this_cpu() == bsp;
    resample();
    if (on_bsp && !bsp_exiting && pic_path_wanted()) {
        event_intr_exiting(bsp->vcpu, true);
        bsp_exiting = true;
    }

    extint_target = NULL;
    for (uint32_t gsi = 0; gsi < route_count; gsi++) {
        struct route *r = &routes[gsi];
        uint64_t vrte;
        if (vioapic_rte(gsi, &vrte) && !(vrte & IOAPIC_RTE_MASKED)
            && (vrte & IOAPIC_RTE_DELIVERY_MASK) == IOAPIC_RTE_DELIVERY_EXTINT) {
            extint_target = smp_cpu_by_apic_id((uint32_t)(vrte >> IOAPIC_RTE_DEST_SHIFT));
        }
        if (r->io == NULL) {
            continue;
        }
        enum owner owner;
        uint64_t rte = desired(gsi, r, &owner);
        if (owner == OWNER_PIC && !(rte & IOAPIC_RTE_MASKED) && !(on_bsp && bsp_exiting)) {
            bsp_dirty = true;
            rte |= IOAPIC_RTE_MASKED;
        }
        if (owner != OWNER_PIC && r->asserted) {
            r->asserted = false;
        }
        r->owner = owner;
        if (rte != r->host_rte) {
            ioapic_rte_write(r->io, r->pin, rte);
            r->host_rte = rte;
        }
    }

    __atomic_store_n(&extint_asserted, pic_output(), __ATOMIC_RELEASE);
}

// After the 8259s changed, on any processor.
static void kick_after_update(void) {
    struct cpu *self = this_cpu();
    if (bsp_dirty && self != bsp) {
        vcpu_kick(bsp);
    }
    struct cpu *target = extint_target != NULL ? extint_target : bsp;
    if (__atomic_load_n(&extint_asserted, __ATOMIC_ACQUIRE) && target != self) {
        vcpu_kick(target);
    }
}

void irq_host_interrupt(uint8_t vector) {
    int irq = -1;
    for (int i = 0; i < ISA_IRQS; i++) {
        if (irq_vectors[i] == vector) {
            irq = i;
        }
    }

    spinlock_acquire(&lock);
    bool edge = false;
    bool level = false;
    for (uint32_t gsi = 0; gsi < route_count; gsi++) {
        struct route *r = &routes[gsi];
        if (r->irq != irq || r->owner != OWNER_PIC) {
            continue;
        }
        if (!(r->host_rte & IOAPIC_RTE_LEVEL)) {
            edge = true;
            continue;
        }
        // Masked before the EOI, as the line stays asserted until the guest
        // deals with its device.
        if (!r->asserted) {
            r->asserted = true;
            r->host_rte |= IOAPIC_RTE_MASKED;
            ioapic_rte_write(r->io, r->pin, r->host_rte);
            ioapic_eoi(r->io, vector);
        }
        level = true;
    }
    if (edge) {
        pic_pulse_irq((unsigned)irq);
    }
    if (level) {
        pic_set_irq((unsigned)irq, true);
    }
    lapic_eoi();
    __atomic_store_n(&extint_asserted, pic_output(), __ATOMIC_RELEASE);
    spinlock_release(&lock);
    kick_after_update();
}

void irq_gsi_changed(uint32_t gsi) {
    (void)gsi;
    spinlock_acquire(&lock);
    update();
    spinlock_release(&lock);
    kick_after_update();
}

bool irq_extint_pending(struct vcpu *v) {
    if (!__atomic_load_n(&extint_asserted, __ATOMIC_ACQUIRE)) {
        return false;
    }
    return v->cpu == extint_target || lapic_extint_open();
}

uint8_t irq_extint_acknowledge(void) {
    spinlock_acquire(&lock);
    uint8_t vector = pic_acknowledge();
    __atomic_store_n(&extint_asserted, pic_output(), __ATOMIC_RELEASE);
    spinlock_release(&lock);
    return vector;
}

// Whether an interrupt the 8259 path's entries sent may still reach the
// boot processor's local APIC.
static bool host_vector_in_flight(void) {
    for (uint32_t gsi = 0; gsi < route_count; gsi++) {
        struct route *r = &routes[gsi];
        if (r->irq >= 0 && irq_vectors[r->irq] != 0
            && (ioapic_rte_read(r->io, r->pin) & IOAPIC_RTE_PENDING)) {
            return true;
        }
    }
    for (unsigned irq = 0; irq < ISA_IRQS; irq++) {
        if (irq_vectors[irq] != 0 && lapic_requested(irq_vectors[irq])) {
            return true;
        }
    }
    return false;
}

void irq_before_entry(struct vcpu *v) {
    if (v->cpu != bsp) {
        return;
    }
    spinlock_acquire(&lock);
    if (__atomic_exchange_n(&bsp_dirty, false, __ATOMIC_ACQ_REL)) {
        update();
    }
    bool unmasked = false;
    for (uint32_t gsi = 0; gsi < route_count; gsi++) {
        if (routes[gsi].owner == OWNER_PIC && !(routes[gsi].host_rte & IOAPIC_RTE_MASKED)) {
            unmasked = true;
        }
    }
    // Physical interrupts reach the guest directly again once none of the
    // 8259 path's can arrive.
    if (!unmasked && bsp_exiting && !host_vector_in_flight()) {
        event_intr_exiting(v, false);
        bsp_exiting = false;
    }
    spinlock_release(&lock);
}

static void pic_access(struct vcpu *v, void *ctx, uint16_t offset, unsigned size, bool in,
                       uint32_t *value) {
    (void)v;
    uint16_t base = (uint16_t)(uintptr_t)ctx;
    spinlock_acquire(&lock);
    for (unsigned i = 0; i < size; i++) {
        uint16_t port = (uint16_t)(base + offset + i);
        if (in) {
            *value = (*value & ~(0xffu << (8 * i))) | (uint32_t)pic_read(port) << (8 * i);
        } else {
            pic_write(port, (uint8_t)(*value >> (8 * i)));
        }
    }
    update();
    spinlock_release(&lock);
    kick_after_update();
}

void irq_init(void) {
    bsp = this_cpu();
    // The guest's 8259s stand in for the host's from now on.
    outb(PIC_MASTER_DATA, 0xff);
    outb(PIC_SLAVE_DATA, 0xff);
    uacpi_status status = uacpi_set_interrupt_model(UACPI_INTERRUPT_MODEL_IOAPIC);
    if (status != UACPI_STATUS_OK) {
        printf("Cannot put the firmware in APIC mode: %s\n", uacpi_status_to_string(status));
    }

    routes_init();
    pic_reset();
    // As a BIOS sets the ELCR for level-triggered sources, such as the SCI.
    uint16_t elcr = 0;
    for (uint32_t gsi = 0; gsi < route_count; gsi++) {
        if (routes[gsi].irq >= 0 && routes[gsi].level) {
            elcr |= 1u << routes[gsi].irq;
        }
    }
    pic_write(PIC_ELCR_MASTER, (uint8_t)elcr);
    pic_write(PIC_ELCR_SLAVE, (uint8_t)(elcr >> 8));

    static const uint16_t ports[] = {PIC_MASTER_CMD, PIC_SLAVE_CMD, PIC_ELCR_MASTER};
    for (unsigned i = 0; i < sizeof(ports) / sizeof(ports[0]); i++) {
        struct io_handler handler = {
            .base = ports[i],
            .count = 2,
            .access = pic_access,
            .ctx = (void *)(uintptr_t)ports[i],
        };
        io_register(&handler);
    }
}
