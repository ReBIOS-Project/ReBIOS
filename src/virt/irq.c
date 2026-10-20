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
#include <iommu/iommu_tables.h>
#include <iommu/remap.h>
#include <lib/heap.h>
#include <lib/log.h>
#include <lib/spinlock.h>
#include <sys/cpu.h>
#include <sys/smp.h>
#include <sys/x86.h>
#include <virt/io.h>
#include <virt/irq.h>
#include <virt/pic.h>
#include <virt/vcpu.h>
#include <virt/vioapic.h>
#include <virt/vlapic.h>

#define ISA_IRQS 16
#define CASCADE_IRQ 2
// The highest APIC ID an 8-bit destination names alone, FFh being the
// broadcast (SDM 325462-093 Vol. 3A, 13.6.2.1).
#define XAPIC_MAX_ID 0xfe

// The ISA IRQs of the devices the hypervisor provides: the PIT, the keyboard
// controller's keyboard and mouse ports, the cascade, the RTC, and FERR#.
#define HYPERVISOR_IRQS (1u << 0 | 1u << 1 | 1u << 2 | 1u << 8 | 1u << 12 | 1u << 13)

// The order free ISA IRQs go to PCI in, those period BIOSes gave it first.
static const uint8_t pci_irqs[] = {11, 10, 9, 5, 15, 14, 7, 6, 4, 3};
#define PERIOD_PCI_IRQS 4

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
    // The hypervisor's own, on VLAPIC_DEVICE_VECTOR.
    OWNER_HYPERVISOR,
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
    // A level-triggered interrupt of the guest's own entry, held likewise
    // until the guest's EOI, which its Remote IRR shows.
    bool held;
    uint64_t host_rte;
    // A host line the hypervisor took, the handler its interrupts call.
    void (*handler)(void *ctx);
    void *handler_ctx;
    // A line the hypervisor drives for the guest, its level, and, for the
    // guest's level-triggered I/O APIC entry, its Remote IRR.
    bool virtual_line;
    bool virtual_asserted;
    bool virtual_held;
    // What a guest's edge-triggered entry last saw.
    bool virtual_edge_seen;
    // The vector the host's entry has while it mirrors the guest's, and the
    // hypervisor's own while it has the line.
    struct irq_slot slot;
    struct irq_slot own;
};

// The vectors slots get, none of which the hypervisor uses itself.
#define SLOT_FIRST 0x20
#define SLOT_END 0xf0

static struct route *routes;
static uint32_t route_count;
// Per processor, the slot each vector belongs to, and where the next search
// for room starts, so that a vector freed is the last to be used again.
static struct irq_slot *(*slot_table)[256];
static uint8_t *slot_next;
static struct spinlock slot_lock;
static struct spinlock lock;
static struct cpu *bsp;
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

static void mirror_taken(void *ctx, unsigned index);

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
        routes[gsi].slot = (struct irq_slot){.taken = mirror_taken, .ctx = &routes[gsi]};
        uint32_t pin;
        struct ioapic *io = ioapic_for_gsi(gsi, &pin);
        if (io != NULL) {
            routes[gsi].io = io;
            routes[gsi].pin = pin;
            routes[gsi].slot.requester = remap_requester(IOMMU_SPECIAL_IOAPIC, io->id);
            routes[gsi].own.requester = routes[gsi].slot.requester;
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
static uint64_t desired(uint32_t gsi, struct route *r, enum owner *owner) {
    uint64_t rte;
    if (r->handler != NULL && irq_slot_fixed(&r->own, bsp, VLAPIC_DEVICE_VECTOR)) {
        *owner = OWNER_HYPERVISOR;
        return irq_slot_rte(&r->own, r->level, r->active_low);
    }
    // A line the hypervisor drives has none at the host.
    if (r->virtual_line) {
        *owner = OWNER_NONE;
        return IOAPIC_RTE_MASKED;
    }
    // The guest's entry, on a vector of the hypervisor's, to the processor of
    // the first vCPU it names: delivery follows the guest's entry.
    if (vioapic_rte(gsi, &rte) && !(rte & IOAPIC_RTE_MASKED)
        && (rte & IOAPIC_RTE_DELIVERY_MASK) != IOAPIC_RTE_DELIVERY_EXTINT
        && (r->slot.cpu != NULL
            || irq_slot_place(&r->slot,
                              vlapic_target((uint32_t)(rte >> IOAPIC_RTE_DEST_SHIFT),
                                            (rte & IOAPIC_RTE_LOGICAL) != 0),
                              1))) {
        *owner = OWNER_MIRROR;
        uint64_t host = irq_slot_rte(&r->slot, (rte & IOAPIC_RTE_LEVEL) != 0,
                                     (rte & IOAPIC_RTE_ACTIVE_LOW) != 0);
        if (r->held) {
            host |= IOAPIC_RTE_MASKED;
        }
        return host;
    }
    // While the guest has any 8259 input unmasked, its masked inputs latch
    // requests as well.
    if (r->irq >= 0 && irq_vectors[r->irq] != 0 && pic_in_use() && bsp->apic_id <= XAPIC_MAX_ID
        && irq_slot_fixed(&r->own, bsp, irq_vectors[r->irq])) {
        *owner = OWNER_PIC;
        // The guest's ELCR says how its 8259 senses the line, the MADT the
        // line's polarity.
        rte = irq_slot_rte(&r->own, pic_level_triggered((unsigned)r->irq), r->active_low);
        if (r->asserted) {
            rte |= IOAPIC_RTE_MASKED;
        }
        return rte;
    }
    *owner = OWNER_NONE;
    return IOAPIC_RTE_MASKED;
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
            if (routes[gsi].irq == (int)irq && routes[gsi].asserted && !routes[gsi].virtual_line) {
                routes[gsi].asserted = false;
                deasserted = true;
            }
        }
        if (deasserted) {
            pic_set_irq(irq, false);
        }
    }
}

// Where a line the hypervisor drives reaches the guest: its I/O APIC entry,
// while unmasked, else its 8259.
static void virtual_deliver(uint32_t gsi, struct route *r) {
    uint64_t vrte;
    bool apic = vioapic_rte(gsi, &vrte) && !(vrte & IOAPIC_RTE_MASKED)
                && (vrte & IOAPIC_RTE_DELIVERY_MASK) != IOAPIC_RTE_DELIVERY_EXTINT;
    if (r->irq >= 0) {
        pic_set_irq((unsigned)r->irq, !apic && r->virtual_asserted);
    }
    if (!apic) {
        r->virtual_held = false;
        r->virtual_edge_seen = r->virtual_asserted;
        return;
    }
    uint32_t mode = (uint32_t)(vrte & IOAPIC_RTE_DELIVERY_MASK);
    bool level = (vrte & IOAPIC_RTE_LEVEL) != 0;
    bool send = level ? r->virtual_asserted && !r->virtual_held
                      : r->virtual_asserted && !r->virtual_edge_seen;
    r->virtual_edge_seen = r->virtual_asserted;
    if (!send || (mode != IOAPIC_RTE_DELIVERY_FIXED && mode != IOAPIC_RTE_DELIVERY_LOWEST)) {
        return;
    }
    r->virtual_held = level;
    vlapic_deliver_external((uint32_t)(vrte >> IOAPIC_RTE_DEST_SHIFT),
                            (vrte & IOAPIC_RTE_LOGICAL) != 0, (uint8_t)vrte, level,
                            mode == IOAPIC_RTE_DELIVERY_LOWEST);
}

static void update(void) {
    resample();

    struct cpu *was = extint_target;
    extint_target = NULL;
    for (uint32_t gsi = 0; gsi < route_count; gsi++) {
        struct route *r = &routes[gsi];
        uint64_t vrte;
        if (vioapic_rte(gsi, &vrte) && !(vrte & IOAPIC_RTE_MASKED)
            && (vrte & IOAPIC_RTE_DELIVERY_MASK) == IOAPIC_RTE_DELIVERY_EXTINT) {
            extint_target = smp_cpu_by_apic_id((uint32_t)(vrte >> IOAPIC_RTE_DEST_SHIFT));
        }
        if (r->virtual_line) {
            virtual_deliver(gsi, r);
        }
        if (r->io == NULL) {
            continue;
        }
        enum owner owner;
        uint64_t rte = desired(gsi, r, &owner);
        if (owner != OWNER_PIC) {
            r->asserted = false;
        }
        if (owner != OWNER_MIRROR) {
            r->held = false;
            irq_slot_free(&r->slot);
        }
        r->owner = owner;
        if (rte != r->host_rte) {
            ioapic_rte_write(r->io, r->pin, rte);
            r->host_rte = rte;
        }
    }

    __atomic_store_n(&extint_asserted, pic_output(), __ATOMIC_RELEASE);
    if (extint_target != was) {
        vlapic_extint_moved();
    }
}

// After the 8259s changed, on any processor.
static void kick_after_update(void) {
    struct cpu *target = extint_target != NULL ? extint_target : bsp;
    if (__atomic_load_n(&extint_asserted, __ATOMIC_ACQUIRE) && target != this_cpu()) {
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
            ioapic_eoi(r->io, (uint8_t)r->host_rte);
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

// As a BIOS sets the ELCR for level-triggered sources, such as the SCI.
static void elcr_init(void) {
    uint16_t elcr = 0;
    for (uint32_t gsi = 0; gsi < route_count; gsi++) {
        if (routes[gsi].irq >= 0 && routes[gsi].level) {
            elcr |= 1u << routes[gsi].irq;
        }
    }
    pic_write(PIC_ELCR_MASTER, (uint8_t)elcr);
    pic_write(PIC_ELCR_SLAVE, (uint8_t)(elcr >> 8));
}

void irq_route_pci(const struct irq_pci_line *lines, uint32_t count, uint16_t isa_claimed) {
    uint16_t taken = HYPERVISOR_IRQS | isa_claimed;
    struct acpi_fadt *fadt;
    if (uacpi_table_fadt(&fadt) == UACPI_STATUS_OK && fadt->sci_int < ISA_IRQS) {
        taken |= 1u << fadt->sci_int;
    }
    uint8_t pool[sizeof(pci_irqs)];
    unsigned pool_size = 0;
    for (unsigned i = 0; i < sizeof(pci_irqs); i++) {
        if (!(taken & (1u << pci_irqs[i]))) {
            pool[pool_size++] = pci_irqs[i];
        }
    }
    // With none free, PCI shares those it had of old.
    if (pool_size == 0) {
        memcpy(pool, pci_irqs, PERIOD_PCI_IRQS);
        pool_size = PERIOD_PCI_IRQS;
    }

    spinlock_acquire(&lock);
    unsigned next = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t gsi = lines[i].gsi;
        if (gsi >= route_count || routes[gsi].io == NULL || routes[gsi].irq >= 0) {
            continue;
        }
        struct route *r = &routes[gsi];
        r->irq = pool[next++ % pool_size];
        r->level = lines[i].level;
        r->active_low = lines[i].active_low;
        printf("GSI %u is ISA IRQ %d for PCI\n", gsi, r->irq);
    }
    elcr_init();
    spinlock_release(&lock);
}

int irq_isa_irq(uint32_t gsi) {
    return gsi < route_count ? routes[gsi].irq : -1;
}

uint32_t irq_isa_gsi(unsigned irq) {
    for (uint32_t gsi = 0; gsi < route_count; gsi++) {
        if (routes[gsi].irq == (int)irq) {
            return gsi;
        }
    }
    return UINT32_MAX;
}

void irq_gsi_changed(uint32_t gsi) {
    (void)gsi;
    spinlock_acquire(&lock);
    update();
    spinlock_release(&lock);
    kick_after_update();
}

bool irq_extint_routed(void) {
    return __atomic_load_n(&extint_target, __ATOMIC_ACQUIRE) != NULL;
}

bool irq_extint_pending(struct vcpu *v) {
    if (!__atomic_load_n(&extint_asserted, __ATOMIC_ACQUIRE)) {
        return false;
    }
    return v->cpu == extint_target || vlapic_extint_open(v);
}

// An interrupt of a mirrored entry: level-triggered ones held masked at the
// host until the guest's EOI, before the processor's EOI lets the line in
// again. That EOI reaches the entry by its vector field, which AMD-Vi's
// remapping fills with an index instead: there the I/O APIC is told directly.
static void mirror_taken(void *ctx, unsigned index) {
    (void)index;
    struct route *r = ctx;
    uint32_t gsi = (uint32_t)(r - routes);
    uint64_t vrte = 0;
    spinlock_acquire(&lock);
    bool deliver = r->owner == OWNER_MIRROR && vioapic_rte(gsi, &vrte);
    bool level = deliver && (vrte & IOAPIC_RTE_LEVEL);
    if (level) {
        r->held = true;
        r->host_rte |= IOAPIC_RTE_MASKED;
        ioapic_rte_write(r->io, r->pin, r->host_rte);
        if ((uint8_t)r->host_rte != r->slot.vector) {
            ioapic_eoi(r->io, (uint8_t)r->host_rte);
        }
    }
    spinlock_release(&lock);
    if (deliver) {
        vlapic_deliver_message((uint32_t)(vrte >> IOAPIC_RTE_DEST_SHIFT),
                               (vrte & IOAPIC_RTE_LOGICAL) != 0,
                               (uint32_t)(vrte & IOAPIC_RTE_DELIVERY_MASK), (uint8_t)vrte, level);
    }
}

void irq_mirror_eoi(uint8_t vector) {
    spinlock_acquire(&lock);
    bool released = false;
    for (uint32_t gsi = 0; gsi < route_count; gsi++) {
        struct route *r = &routes[gsi];
        uint64_t vrte;
        if (!vioapic_rte(gsi, &vrte) || (uint8_t)vrte != vector) {
            continue;
        }
        if (r->held) {
            r->held = false;
            released = true;
        }
        if (r->virtual_held) {
            r->virtual_held = false;
            released = true;
        }
    }
    if (released) {
        update();
    }
    spinlock_release(&lock);
}

static void slot_release(struct irq_slot *s) {
    if (s->cpu == NULL) {
        return;
    }
    if (s->remapped) {
        remap_free(s->requester, s->remap, s->count);
        s->remapped = false;
    }
    for (unsigned i = 0; i < s->count; i++) {
        __atomic_store_n(&slot_table[s->cpu->index][s->vector + i], NULL, __ATOMIC_RELEASE);
    }
    s->cpu = NULL;
}

// An aligned block of count free vectors on the processor.
static bool slot_room(struct cpu *cpu, unsigned count, uint8_t *vector) {
    struct irq_slot **table = slot_table[cpu->index];
    unsigned blocks = (SLOT_END - SLOT_FIRST) / count;
    unsigned first = (slot_next[cpu->index] - SLOT_FIRST) / count;
    for (unsigned n = 0; n < blocks; n++) {
        unsigned base = SLOT_FIRST + (first + n) % blocks * count;
        bool free = true;
        for (unsigned i = 0; i < count && free; i++) {
            free = table[base + i] == NULL;
        }
        if (free) {
            *vector = (uint8_t)base;
            slot_next[cpu->index] = (uint8_t)(base + count >= SLOT_END ? SLOT_FIRST : base + count);
            return true;
        }
    }
    return false;
}

static bool slot_usable(struct cpu *cpu) {
    return cpu != NULL && cpu->vcpu != NULL && cpu->apic_id <= XAPIC_MAX_ID;
}

bool irq_slot_place(struct irq_slot *s, struct cpu *cpu, unsigned count) {
    spinlock_acquire(&slot_lock);
    slot_release(s);
    struct cpu *where = NULL;
    uint8_t vector = 0;
    if (slot_usable(cpu) && slot_room(cpu, count, &vector)) {
        where = cpu;
    }
    for (uint32_t i = 0; where == NULL && i < smp_cpu_count(); i++) {
        struct cpu *c = smp_cpu(i);
        if (slot_usable(c) && slot_room(c, count, &vector)) {
            where = c;
        }
    }
    int remap = -1;
    if (where != NULL && remap_on() && (remap = remap_alloc(s->requester, count)) < 0) {
        where = NULL;
    }
    if (where != NULL) {
        s->cpu = where;
        s->vector = vector;
        s->count = (uint8_t)count;
        s->remapped = remap >= 0;
        s->remap = (uint16_t)remap;
        for (unsigned i = 0; i < count; i++) {
            __atomic_store_n(&slot_table[where->index][vector + i], s, __ATOMIC_RELEASE);
        }
    }
    spinlock_release(&slot_lock);
    return where != NULL;
}

bool irq_slot_fixed(struct irq_slot *s, struct cpu *cpu, uint8_t vector) {
    if (s->cpu == cpu && s->vector == vector && s->count == 1) {
        return true;
    }
    spinlock_acquire(&slot_lock);
    if (s->remapped) {
        remap_free(s->requester, s->remap, s->count);
        s->remapped = false;
    }
    int remap = remap_on() ? remap_alloc(s->requester, 1) : -1;
    bool ok = !remap_on() || remap >= 0;
    s->cpu = ok ? cpu : NULL;
    s->vector = vector;
    s->count = 1;
    s->remapped = remap >= 0;
    s->remap = (uint16_t)remap;
    spinlock_release(&slot_lock);
    return ok;
}

void irq_slot_free(struct irq_slot *s) {
    spinlock_acquire(&slot_lock);
    slot_release(s);
    spinlock_release(&slot_lock);
}

// Compatibility format, fixed and edge-triggered to the processor (SDM
// 325462-093 Vol. 3A, 13.11).
// Where remapped, the entries say where the vectors are.
void irq_slot_message(struct irq_slot *s, uint32_t *address, uint32_t *data) {
    if (s->remapped) {
        for (unsigned i = 0; i < s->count; i++) {
            remap_set(s->requester, s->remap + (int)i, s->cpu->apic_id, (uint8_t)(s->vector + i),
                      false);
        }
        remap_message(s->requester, s->remap, address, data);
        return;
    }
    *address = MSI_ADDRESS_BASE | s->cpu->apic_id << MSI_ADDRESS_DEST_SHIFT;
    *data = s->vector;
}

uint64_t irq_slot_rte(struct irq_slot *s, bool level, bool active_low) {
    if (s->remapped) {
        remap_set(s->requester, s->remap, s->cpu->apic_id, s->vector, level);
        return remap_rte(s->requester, s->remap, s->vector, level, active_low);
    }
    return s->vector | (uint64_t)s->cpu->apic_id << IOAPIC_RTE_DEST_SHIFT
           | (level ? IOAPIC_RTE_LEVEL : 0) | (active_low ? IOAPIC_RTE_ACTIVE_LOW : 0);
}

bool irq_slot_interrupt(uint8_t vector) {
    struct irq_slot *s =
        __atomic_load_n(&slot_table[this_cpu()->index][vector], __ATOMIC_ACQUIRE);
    if (s == NULL) {
        return false;
    }
    s->taken(s->ctx, vector & (s->count - 1u));
    lapic_eoi();
    return true;
}

bool irq_remote_irr(uint32_t gsi) {
    return gsi < route_count && (__atomic_load_n(&routes[gsi].held, __ATOMIC_ACQUIRE)
                                 || __atomic_load_n(&routes[gsi].virtual_held, __ATOMIC_ACQUIRE));
}

void irq_take_host_line(uint32_t gsi, void (*handler)(void *ctx), void *ctx) {
    if (gsi >= route_count || routes[gsi].io == NULL) {
        printf("No host line for GSI %u\n", gsi);
        return;
    }
    spinlock_acquire(&lock);
    routes[gsi].handler = handler;
    routes[gsi].handler_ctx = ctx;
    update();
    spinlock_release(&lock);
}

void irq_device_interrupt(void) {
    spinlock_acquire(&lock);
    for (uint32_t gsi = 0; gsi < route_count; gsi++) {
        struct route *r = &routes[gsi];
        if (r->handler == NULL) {
            continue;
        }
        // Masked while its handler runs, as a level-triggered line stays
        // asserted until the handler has dealt with its device.
        ioapic_rte_write(r->io, r->pin, r->host_rte | IOAPIC_RTE_MASKED);
        if (r->host_rte & IOAPIC_RTE_LEVEL) {
            ioapic_eoi(r->io, (uint8_t)r->host_rte);
        }
    }
    lapic_eoi();
    spinlock_release(&lock);

    for (uint32_t gsi = 0; gsi < route_count; gsi++) {
        if (routes[gsi].handler != NULL) {
            routes[gsi].handler(routes[gsi].handler_ctx);
        }
    }

    spinlock_acquire(&lock);
    for (uint32_t gsi = 0; gsi < route_count; gsi++) {
        struct route *r = &routes[gsi];
        if (r->handler != NULL) {
            ioapic_rte_write(r->io, r->pin, r->host_rte);
        }
    }
    spinlock_release(&lock);
}

void irq_virtual_line(uint32_t gsi, bool asserted) {
    if (gsi >= route_count) {
        return;
    }
    spinlock_acquire(&lock);
    struct route *r = &routes[gsi];
    r->virtual_line = true;
    r->virtual_asserted = asserted;
    update();
    spinlock_release(&lock);
    kick_after_update();
}

uint8_t irq_extint_acknowledge(void) {
    spinlock_acquire(&lock);
    uint8_t vector = pic_acknowledge();
    __atomic_store_n(&extint_asserted, pic_output(), __ATOMIC_RELEASE);
    spinlock_release(&lock);
    return vector;
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
    elcr_init();
    slot_table = kmalloc(smp_cpu_count() * sizeof(*slot_table));
    slot_next = kmalloc(smp_cpu_count());
    memset(slot_next, SLOT_FIRST, smp_cpu_count());

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
