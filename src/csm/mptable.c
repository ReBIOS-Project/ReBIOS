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
#include <uacpi/status.h>
#include <uacpi/tables.h>
#include <memory.h>
#include <csm/mptable.h>
#include <hw/acpi.h>
#include <hw/acpi_irq.h>
#include <hw/ioapic.h>
#include <hw/lapic.h>
#include <hw/pci_enum.h>
#include <lib/log.h>
#include <sys/cpu.h>
#include <sys/smp.h>
#include <sys/x86.h>

// The structures and values of the Intel MultiProcessor Specification 1.4,
// chapter 4.

#define MP_SPEC_REV 4

#define MP_PROCESSOR 0
#define MP_BUS 1
#define MP_IOAPIC 2
#define MP_IO_INTERRUPT 3
#define MP_LOCAL_INTERRUPT 4
#define MP_ENTRY_TYPES 5

#define MP_PROCESSOR_ENABLED (1u << 0)
#define MP_PROCESSOR_BSP (1u << 1)
#define MP_IOAPIC_ENABLED (1u << 0)

#define MP_INT 0
#define MP_NMI 1
#define MP_EXTINT 3

// The PO and EL fields of the interrupt entries' flags, which the MADT's MPS
// INTI flags share, ACPI 6.6, Table 5.26.
#define MP_FLAGS_MASK 0xfu
#define MP_ACTIVE_HIGH 0x1u
#define MP_ACTIVE_LOW 0x3u
#define MP_EDGE (0x1u << 2)
#define MP_LEVEL (0x3u << 2)

// The destination of every local APIC, which no processor can have as its ID.
#define MP_ALL_APICS 0xff
#define MP_MAX_APIC_ID 0xfe

#define MADT_ALL_UIDS 0xffu
#define MADT_ALL_X2APIC_UIDS 0xffffffffu

#define PCI_HEADER_BRIDGE 1
#define PCI_HEADER_CARDBUS 2
#define PCI_BUSES 256
#define ISA_IRQS 16
#define CASCADE_IRQ 2

struct mp_floating_pointer {
    char signature[4];
    uint32_t config_table;
    uint8_t length;
    uint8_t spec_rev;
    uint8_t checksum;
    uint8_t features[5];
} __attribute__((packed));

struct mp_config_header {
    char signature[4];
    uint16_t base_table_length;
    uint8_t spec_rev;
    uint8_t checksum;
    char oem_id[8];
    char product_id[12];
    uint32_t oem_table;
    uint16_t oem_table_size;
    uint16_t entry_count;
    uint32_t local_apic;
    uint16_t extended_table_length;
    uint8_t extended_table_checksum;
    uint8_t reserved;
} __attribute__((packed));

struct mp_processor {
    uint8_t type;
    uint8_t apic_id;
    uint8_t apic_version;
    uint8_t flags;
    uint32_t signature;
    uint32_t features;
    uint32_t reserved[2];
} __attribute__((packed));

struct mp_bus {
    uint8_t type;
    uint8_t id;
    char type_string[6];
} __attribute__((packed));

struct mp_ioapic {
    uint8_t type;
    uint8_t id;
    uint8_t version;
    uint8_t flags;
    uint32_t address;
} __attribute__((packed));

// The I/O and the local interrupt assignment entries alike.
struct mp_interrupt {
    uint8_t type;
    uint8_t interrupt_type;
    uint16_t flags;
    uint8_t source_bus;
    uint8_t source_irq;
    uint8_t destination;
    uint8_t destination_pin;
} __attribute__((packed));

_Static_assert(sizeof(struct mp_floating_pointer) == 16, "MP floating pointer layout");
_Static_assert(offsetof(struct mp_config_header, oem_id) == 8, "MP table header layout");
_Static_assert(offsetof(struct mp_config_header, oem_table) == 28, "MP table header layout");
_Static_assert(offsetof(struct mp_config_header, local_apic) == 36, "MP table header layout");
_Static_assert(sizeof(struct mp_config_header) == 44, "MP table header layout");
_Static_assert(sizeof(struct mp_processor) == 20, "MP processor entry layout");
_Static_assert(sizeof(struct mp_bus) == 8, "MP bus entry layout");
_Static_assert(sizeof(struct mp_ioapic) == 8, "MP I/O APIC entry layout");
_Static_assert(sizeof(struct mp_interrupt) == 8, "MP interrupt entry layout");

#define MP_ENTRIES_OFFSET \
    (sizeof(struct mp_floating_pointer) + sizeof(struct mp_config_header))

// What the table is made of, gathered once for its two passes.
struct mp_input {
    struct uacpi_table madt_table;
    const uint8_t *madt;
    uint32_t madt_length;
    char oem_id[6];
    char product_id[8];
    uint32_t local_apic;
    uint32_t cpu_signature;
    uint32_t cpu_features;
    uint8_t apic_version;
    // Bit n % 64 of word n / 64 for each PCI bus n of segment 0.
    uint64_t pci_buses[PCI_BUSES / 64];
    bool isa;
    uint8_t isa_bus;
    uint32_t isa_gsi[ISA_IRQS];
    uint16_t isa_flags[ISA_IRQS];
    const struct acpi_irq_pci_route *routes;
    uint32_t route_count;
};

struct mp_writer {
    // NULL while the table is only measured.
    uint8_t *buf;
    uint32_t length;
    uint16_t counts[MP_ENTRY_TYPES];
};

static uint8_t checksum(const uint8_t *bytes, uint32_t length) {
    uint8_t sum = 0;
    for (uint32_t i = 0; i < length; i++) {
        sum = (uint8_t)(sum + bytes[i]);
    }
    return (uint8_t)-sum;
}

// MP table strings are ASCII, padded with spaces.
static void mp_string(char *dest, size_t size, const char *src, size_t length) {
    for (size_t i = 0; i < size; i++) {
        char c = i < length ? src[i] : ' ';
        dest[i] = c >= 0x20 && c <= 0x7e ? c : ' ';
    }
}

// The MADT's entry at *offset, which moves past it, or NULL after the last.
static const uint8_t *madt_next(const struct mp_input *in, uint32_t *offset) {
    if (*offset > in->madt_length || in->madt_length - *offset < sizeof(struct acpi_entry_hdr)) {
        return NULL;
    }
    const uint8_t *entry = in->madt + *offset;
    uint8_t length = entry[1];
    if (length < sizeof(struct acpi_entry_hdr) || length > in->madt_length - *offset) {
        return NULL;
    }
    *offset += length;
    return entry;
}

static void pci_bus_add(struct mp_input *in, uint8_t bus) {
    in->pci_buses[bus / 64] |= 1ull << (bus % 64);
}

static bool pci_bus_has(const struct mp_input *in, unsigned bus) {
    return (in->pci_buses[bus / 64] >> (bus % 64)) & 1;
}

static void pci_buses_find(struct mp_input *in) {
    const struct pci_topology *topology = pci_enum_scan();
    for (uint32_t i = 0; i < topology->root_bus_count; i++) {
        if (topology->root_buses[i].segment == 0) {
            pci_bus_add(in, topology->root_buses[i].bus);
        }
    }
    for (uint32_t i = 0; i < topology->function_count; i++) {
        const struct pci_function *f = &topology->functions[i];
        bool bridge = f->header_type == PCI_HEADER_BRIDGE || f->header_type == PCI_HEADER_CARDBUS;
        // A bridge whose secondary bus is not above its own has not been given
        // one.
        if (f->addr.segment == 0 && bridge && f->secondary_bus > f->addr.bus) {
            pci_bus_add(in, f->secondary_bus);
        }
    }

    unsigned next = 0;
    for (unsigned bus = 0; bus < PCI_BUSES; bus++) {
        if (pci_bus_has(in, bus)) {
            next = bus + 1;
        }
    }
    in->isa = next < PCI_BUSES;
    in->isa_bus = (uint8_t)next;
}

static void isa_overrides_find(struct mp_input *in) {
    for (unsigned irq = 0; irq < ISA_IRQS; irq++) {
        in->isa_gsi[irq] = irq;
        in->isa_flags[irq] = 0;
    }
    uint32_t offset = sizeof(struct acpi_madt);
    const uint8_t *entry;
    while ((entry = madt_next(in, &offset)) != NULL) {
        if (entry[0] != ACPI_MADT_ENTRY_TYPE_INTERRUPT_SOURCE_OVERRIDE
            || entry[1] < sizeof(struct acpi_madt_interrupt_source_override)) {
            continue;
        }
        struct acpi_madt_interrupt_source_override iso;
        memcpy(&iso, entry, sizeof(iso));
        if (iso.bus == 0 && iso.source < ISA_IRQS) {
            in->isa_gsi[iso.source] = iso.gsi;
            in->isa_flags[iso.source] = iso.flags & MP_FLAGS_MASK;
        }
    }
}

static bool input_gather(struct mp_input *in) {
    memset(in, 0, sizeof(*in));
    if (!acpi_tables_ready()
        || uacpi_table_find_by_signature(ACPI_MADT_SIGNATURE, &in->madt_table)
           != UACPI_STATUS_OK) {
        printf("No MADT, so no MP table\n");
        return false;
    }
    in->madt = in->madt_table.ptr;
    in->madt_length = in->madt_table.hdr->length;
    if (in->madt_length < sizeof(struct acpi_madt)) {
        printf("The MADT is too short for an MP table\n");
        uacpi_table_unref(&in->madt_table);
        return false;
    }
    struct acpi_madt madt;
    memcpy(&madt, in->madt, sizeof(madt));
    memcpy(in->oem_id, madt.hdr.oemid, sizeof(in->oem_id));
    memcpy(in->product_id, madt.hdr.oem_table_id, sizeof(in->product_id));
    in->local_apic = madt.local_interrupt_controller_address;

    struct cpuid_regs id = cpuid(1, 0);
    in->cpu_signature = id.eax;
    in->cpu_features = id.edx;
    in->apic_version = (uint8_t)lapic_read(APIC_VERSION);

    pci_buses_find(in);
    isa_overrides_find(in);
    in->routes = acpi_irq_pci_routes(&in->route_count);
    return true;
}

static void emit(struct mp_writer *w, const void *entry, uint32_t length) {
    if (w->buf != NULL) {
        memcpy(w->buf + w->length, entry, length);
    }
    w->counts[*(const uint8_t *)entry]++;
    w->length += length;
}

// Whether the table can list a processor: one the guest gets, with an ID the
// table's entries can hold.
static bool cpu_listable(uint32_t apic_id) {
    return apic_id <= MP_MAX_APIC_ID && smp_guest_cpu(apic_id);
}

static bool cpu_listed(uint32_t apic_id) {
    if (!cpu_listable(apic_id)) {
        return false;
    }
    for (uint32_t i = 0; i < smp_cpu_count(); i++) {
        if (smp_cpu(i)->apic_id == apic_id) {
            return true;
        }
    }
    return false;
}

static void processors_emit(struct mp_writer *w, const struct mp_input *in) {
    for (uint32_t i = 0; i < smp_cpu_count(); i++) {
        const struct cpu *cpu = smp_cpu(i);
        if (!cpu_listable(cpu->apic_id)) {
            continue;
        }
        struct mp_processor entry = {
            .type = MP_PROCESSOR,
            .apic_id = (uint8_t)cpu->apic_id,
            .apic_version = in->apic_version,
            .flags = MP_PROCESSOR_ENABLED | (cpu->bsp ? MP_PROCESSOR_BSP : 0),
            .signature = in->cpu_signature,
            .features = in->cpu_features,
        };
        emit(w, &entry, sizeof(entry));
    }
}

static void buses_emit(struct mp_writer *w, const struct mp_input *in) {
    for (unsigned bus = 0; bus < PCI_BUSES; bus++) {
        if (!pci_bus_has(in, bus)) {
            continue;
        }
        struct mp_bus entry = {.type = MP_BUS, .id = (uint8_t)bus};
        memcpy(entry.type_string, "PCI   ", sizeof(entry.type_string));
        emit(w, &entry, sizeof(entry));
    }
    if (in->isa) {
        struct mp_bus entry = {.type = MP_BUS, .id = in->isa_bus};
        memcpy(entry.type_string, "ISA   ", sizeof(entry.type_string));
        emit(w, &entry, sizeof(entry));
    }
}

static void ioapics_emit(struct mp_writer *w) {
    for (uint32_t i = 0; i < ioapic_count(); i++) {
        const struct ioapic *io = ioapic_get(i);
        struct mp_ioapic entry = {
            .type = MP_IOAPIC,
            .id = io->id,
            .version = (uint8_t)io->version,
            .flags = MP_IOAPIC_ENABLED,
            .address = (uint32_t)io->base,
        };
        emit(w, &entry, sizeof(entry));
    }
}

// An I/O interrupt assignment to the I/O APIC input of the GSI, if an I/O APIC
// has it.
static bool io_interrupt_emit(struct mp_writer *w, uint8_t interrupt_type, uint16_t flags,
                              uint8_t bus, uint8_t irq, uint32_t gsi) {
    uint32_t pin;
    const struct ioapic *io = ioapic_for_gsi(gsi, &pin);
    if (io == NULL) {
        return false;
    }
    struct mp_interrupt entry = {
        .type = MP_IO_INTERRUPT,
        .interrupt_type = interrupt_type,
        .flags = flags,
        .source_bus = bus,
        .source_irq = irq,
        .destination = io->id,
        .destination_pin = (uint8_t)pin,
    };
    emit(w, &entry, sizeof(entry));
    return true;
}

// Whether an earlier route has made the entry of route i: that of the same
// bus, device, and pin, on an I/O APIC.
static bool pci_entry_made(const struct mp_input *in, uint32_t i) {
    const struct acpi_irq_pci_route *route = &in->routes[i];
    for (uint32_t j = 0; j < i; j++) {
        const struct acpi_irq_pci_route *other = &in->routes[j];
        uint32_t pin;
        if (other->addr.segment == 0 && other->addr.bus == route->addr.bus
            && other->addr.device == route->addr.device && other->pin == route->pin
            && ioapic_for_gsi(other->gsi, &pin) != NULL) {
            return true;
        }
    }
    return false;
}

static void io_interrupts_emit(struct mp_writer *w, const struct mp_input *in) {
    bool gsi0_taken = false;
    if (in->isa) {
        for (unsigned irq = 0; irq < ISA_IRQS; irq++) {
            if (irq == CASCADE_IRQ) {
                continue;
            }
            uint32_t gsi = in->isa_gsi[irq];
            if (io_interrupt_emit(w, MP_INT, in->isa_flags[irq], in->isa_bus, (uint8_t)irq, gsi)
                && gsi == 0) {
                gsi0_taken = true;
            }
        }
    }
    // As on PCs, the 8259s' output reaches the I/O APIC input of GSI 0, unless
    // an ISA IRQ does.
    if (!gsi0_taken) {
        io_interrupt_emit(w, MP_EXTINT, 0, in->isa ? in->isa_bus : 0, 0, 0);
    }

    for (uint32_t i = 0; i < in->route_count; i++) {
        const struct acpi_irq_pci_route *route = &in->routes[i];
        if (route->addr.segment != 0 || pci_entry_made(in, i)) {
            continue;
        }
        uint16_t flags = (route->active_low ? MP_ACTIVE_LOW : MP_ACTIVE_HIGH)
                         | (route->level ? MP_LEVEL : MP_EDGE);
        // MultiProcessor Specification 1.4, Table D-1.
        uint8_t irq = (uint8_t)(route->addr.device << 2 | (route->pin - 1));
        io_interrupt_emit(w, MP_INT, flags, route->addr.bus, irq, route->gsi);
    }
}

static void local_interrupt_emit(struct mp_writer *w, uint8_t interrupt_type, uint16_t flags,
                                 uint8_t bus, uint8_t destination, uint8_t lint) {
    struct mp_interrupt entry = {
        .type = MP_LOCAL_INTERRUPT,
        .interrupt_type = interrupt_type,
        .flags = flags,
        .source_bus = bus,
        .source_irq = 0,
        .destination = destination,
        .destination_pin = lint,
    };
    emit(w, &entry, sizeof(entry));
}

// The APIC ID of the processor whose Processor Local APIC or x2APIC structure
// has the ACPI Processor UID.
static bool uid_apic_id(const struct mp_input *in, uint32_t uid, uint32_t *apic_id) {
    uint32_t offset = sizeof(struct acpi_madt);
    const uint8_t *entry;
    while ((entry = madt_next(in, &offset)) != NULL) {
        if (entry[0] == ACPI_MADT_ENTRY_TYPE_LAPIC && entry[1] >= sizeof(struct acpi_madt_lapic)) {
            struct acpi_madt_lapic lapic;
            memcpy(&lapic, entry, sizeof(lapic));
            if (lapic.uid == uid) {
                *apic_id = lapic.id;
                return true;
            }
        } else if (entry[0] == ACPI_MADT_ENTRY_TYPE_LOCAL_X2APIC
                   && entry[1] >= sizeof(struct acpi_madt_x2apic)) {
            struct acpi_madt_x2apic x2apic;
            memcpy(&x2apic, entry, sizeof(x2apic));
            if (x2apic.uid == uid) {
                *apic_id = x2apic.id;
                return true;
            }
        }
    }
    return false;
}

static void local_interrupts_emit(struct mp_writer *w, const struct mp_input *in) {
    uint8_t bus = in->isa ? in->isa_bus : 0;
    local_interrupt_emit(w, MP_EXTINT, 0, bus, MP_ALL_APICS, 0);

    bool madt_nmis = false;
    uint32_t offset = sizeof(struct acpi_madt);
    const uint8_t *entry;
    while ((entry = madt_next(in, &offset)) != NULL) {
        uint32_t uid;
        uint16_t flags;
        uint8_t lint;
        bool all;
        if (entry[0] == ACPI_MADT_ENTRY_TYPE_LAPIC_NMI
            && entry[1] >= sizeof(struct acpi_madt_lapic_nmi)) {
            struct acpi_madt_lapic_nmi nmi;
            memcpy(&nmi, entry, sizeof(nmi));
            uid = nmi.uid;
            flags = nmi.flags;
            lint = nmi.lint;
            all = nmi.uid == MADT_ALL_UIDS;
        } else if (entry[0] == ACPI_MADT_ENTRY_TYPE_LOCAL_X2APIC_NMI
                   && entry[1] >= sizeof(struct acpi_madt_x2apic_nmi)) {
            struct acpi_madt_x2apic_nmi nmi;
            memcpy(&nmi, entry, sizeof(nmi));
            uid = nmi.uid;
            flags = nmi.flags;
            lint = nmi.lint;
            all = nmi.uid == MADT_ALL_X2APIC_UIDS;
        } else {
            continue;
        }
        madt_nmis = true;

        uint32_t apic_id = MP_ALL_APICS;
        if (!all && (!uid_apic_id(in, uid, &apic_id) || !cpu_listed(apic_id))) {
            continue;
        }
        local_interrupt_emit(w, MP_NMI, flags & MP_FLAGS_MASK, bus, (uint8_t)apic_id, lint);
    }
    if (!madt_nmis) {
        local_interrupt_emit(w, MP_NMI, 0, bus, MP_ALL_APICS, 1);
    }
}

static uint32_t table_write(struct mp_writer *w, const struct mp_input *in, uint8_t *buf,
                            uint32_t guest) {
    *w = (struct mp_writer){.buf = buf, .length = MP_ENTRIES_OFFSET};
    processors_emit(w, in);
    buses_emit(w, in);
    ioapics_emit(w);
    io_interrupts_emit(w, in);
    local_interrupts_emit(w, in);
    if (buf == NULL) {
        return w->length;
    }

    uint16_t entries = 0;
    for (unsigned type = 0; type < MP_ENTRY_TYPES; type++) {
        entries = (uint16_t)(entries + w->counts[type]);
    }
    struct mp_config_header header = {
        .base_table_length = (uint16_t)(w->length - sizeof(struct mp_floating_pointer)),
        .spec_rev = MP_SPEC_REV,
        .entry_count = entries,
        .local_apic = in->local_apic,
    };
    memcpy(header.signature, "PCMP", sizeof(header.signature));
    mp_string(header.oem_id, sizeof(header.oem_id), in->oem_id, sizeof(in->oem_id));
    mp_string(header.product_id, sizeof(header.product_id), in->product_id,
              sizeof(in->product_id));
    uint8_t *config = buf + sizeof(struct mp_floating_pointer);
    memcpy(config, &header, sizeof(header));
    config[offsetof(struct mp_config_header, checksum)] =
        checksum(config, header.base_table_length);

    // Feature byte 2's IMCRP is clear: there is no IMCR, the guest's 8259s
    // reaching its local APICs in virtual wire mode.
    struct mp_floating_pointer pointer = {
        .config_table = guest + (uint32_t)sizeof(struct mp_floating_pointer),
        .length = 1,
        .spec_rev = MP_SPEC_REV,
    };
    memcpy(pointer.signature, "_MP_", sizeof(pointer.signature));
    memcpy(buf, &pointer, sizeof(pointer));
    buf[offsetof(struct mp_floating_pointer, checksum)] = checksum(buf, sizeof(pointer));
    return w->length;
}

uint32_t mptable_build(void *buf, uint32_t size, uint32_t guest) {
    if (ioapic_count() == 0) {
        printf("No I/O APIC, so no MP table\n");
        return 0;
    }
    struct mp_input in;
    if (!input_gather(&in)) {
        return 0;
    }

    struct mp_writer w;
    uint32_t length = table_write(&w, &in, NULL, guest);
    uint32_t written = 0;
    if (length > size || length - sizeof(struct mp_floating_pointer) > UINT16_MAX) {
        printf("The MP table takes %u bytes, more than the %u it has\n", length, size);
    } else {
        written = table_write(&w, &in, buf, guest);
        printf("MP table at %#x: %u processors, %u buses, %u I/O APICs, %u I/O interrupts, "
               "%u local interrupts\n", guest, w.counts[MP_PROCESSOR], w.counts[MP_BUS],
               w.counts[MP_IOAPIC], w.counts[MP_IO_INTERRUPT], w.counts[MP_LOCAL_INTERRUPT]);
    }
    uacpi_table_unref(&in.madt_table);
    return written;
}
