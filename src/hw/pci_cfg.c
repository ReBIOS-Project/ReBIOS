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

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <efi.h>
#include <uacpi/acpi.h>
#include <uacpi/status.h>
#include <uacpi/tables.h>
#include <hw/pci_cfg.h>
#include <core.h>
#include <handoff.h>
#include <hw/acpi.h>
#include <hw/pci_regs.h>
#include <lib/heap.h>
#include <lib/log.h>
#include <lib/spinlock.h>
#include <memory.h>
#include <sys/x86.h>

// No smaller reserved part of a window is kept, as in Linux 7.3's
// is_mmconf_reserved().
#define ECAM_MIN_RESERVED ((uint64_t)16 << 20)
// Firmware from that year on has its MCFG taken as it is by Linux 7.3's early
// pci_mmcfg_reserved().
#define ECAM_TRUSTED_YEAR 2016

struct ecam_window {
    uintptr_t base;
    uint16_t segment;
    uint8_t start_bus;
    uint8_t end_bus;
};

static struct ecam_window *windows;
static _Atomic(size_t) window_count;
// The MCFG's windows, waiting for pci_cfg_verify() where the memory map does
// not reserve them.
static struct ecam_window *pending;
static size_t pending_count;
static bool mcfg_done;
static struct spinlock legacy_lock;
static bool ports_answer;

static bool access_valid(struct pci_cfg_addr addr, uint32_t offset, size_t size) {
    if (size != 1 && size != 2 && size != 4) {
        return false;
    }
    return offset < PCI_CFG_SPACE_EXP_SIZE && offset % size == 0 && addr.device < 32
        && addr.function < 8;
}

static uint32_t all_ones(size_t size) {
    switch (size) {
        case 1: {
            return 0xff;
        }
        case 2: {
            return 0xffff;
        }
        default: {
            return 0xffffffff;
        }
    }
}

uintptr_t pci_cfg_ecam_base(uint16_t segment, uint8_t bus) {
    size_t count = atomic_load_explicit(&window_count, memory_order_acquire);
    for (size_t i = 0; i < count; i++) {
        struct ecam_window *w = &windows[i];
        if (w->segment == segment && bus >= w->start_bus && bus <= w->end_bus) {
            return w->base;
        }
    }
    return 0;
}

static uintptr_t ecam_address(uintptr_t base, struct pci_cfg_addr addr, uint32_t offset) {
    return (uintptr_t)(base
                     + (((uint64_t)addr.bus << 20) | ((uint64_t)addr.device << 15)
                      | ((uint64_t)addr.function << 12) | offset));
}

static uint32_t legacy_address(struct pci_cfg_addr addr, uint32_t offset) {
    return PCI_CFG_ENABLE | ((uint32_t)addr.bus << 16) | ((uint32_t)addr.device << 11)
         | ((uint32_t)addr.function << 8) | (offset & 0xfc);
}

static bool legacy_reachable(struct pci_cfg_addr addr, uint32_t offset) {
    return addr.segment == 0 && offset < 256;
}

uint32_t pci_cfg_read(struct pci_cfg_addr addr, uint32_t offset, size_t size) {
    if (!access_valid(addr, offset, size)) {
        return all_ones(size);
    }

    uintptr_t base = pci_cfg_ecam_base(addr.segment, addr.bus);
    if (base != 0) {
        uintptr_t p = ecam_address(base, addr, offset);
        switch (size) {
            case 1: {
                return *(volatile uint8_t *)p;
            }
            case 2: {
                return *(volatile uint16_t *)p;
            }
            default: {
                return *(volatile uint32_t *)p;
            }
        }
    }

    if (!legacy_reachable(addr, offset)) {
        return all_ones(size);
    }

    uint16_t port = PCI_CFG_DATA + (offset & 3);
    uint32_t value;
    spinlock_acquire(&legacy_lock);
    // The guest may be between its own write to CONFIG_ADDRESS and the data
    // access that goes with it.
    uint32_t saved = inl(PCI_CFG_ADDRESS);
    outl(PCI_CFG_ADDRESS, legacy_address(addr, offset));
    switch (size) {
        case 1: {
            value = inb(port);
            break;
        }
        case 2: {
            value = inw(port);
            break;
        }
        default: {
            value = inl(port);
            break;
        }
    }
    outl(PCI_CFG_ADDRESS, saved);
    spinlock_release(&legacy_lock);
    return value;
}

void pci_cfg_write(struct pci_cfg_addr addr, uint32_t offset, size_t size, uint32_t value) {
    if (!access_valid(addr, offset, size)) {
        return;
    }

    uintptr_t base = pci_cfg_ecam_base(addr.segment, addr.bus);
    if (base != 0) {
        uintptr_t p = ecam_address(base, addr, offset);
        switch (size) {
            case 1: {
                *(volatile uint8_t *)p = (uint8_t)value;
                break;
            }
            case 2: {
                *(volatile uint16_t *)p = (uint16_t)value;
                break;
            }
            default: {
                *(volatile uint32_t *)p = value;
                break;
            }
        }
        return;
    }

    if (!legacy_reachable(addr, offset)) {
        return;
    }

    uint16_t port = PCI_CFG_DATA + (offset & 3);
    spinlock_acquire(&legacy_lock);
    uint32_t saved = inl(PCI_CFG_ADDRESS);
    outl(PCI_CFG_ADDRESS, legacy_address(addr, offset));
    switch (size) {
        case 1: {
            outb(port, (uint8_t)value);
            break;
        }
        case 2: {
            outw(port, (uint16_t)value);
            break;
        }
        default: {
            outl(port, value);
            break;
        }
    }
    outl(PCI_CFG_ADDRESS, saved);
    spinlock_release(&legacy_lock);
}

// The types Linux 7.3's do_add_efi_memmap() makes reserved E820 entries.
static bool type_reserved(uint32_t type) {
    switch (type) {
        case EFI_LOADER_CODE:
        case EFI_LOADER_DATA:
        case EFI_BOOT_SERVICES_CODE:
        case EFI_BOOT_SERVICES_DATA:
        case EFI_CONVENTIONAL_MEMORY:
        case EFI_UNUSABLE_MEMORY:
        case EFI_ACPI_RECLAIM_MEMORY:
        case EFI_ACPI_MEMORY_NVS:
        case EFI_PERSISTENT_MEMORY: {
            return false;
        }
        default: {
            return true;
        }
    }
}

// Whether reserved descriptors cover all of [base, end), as Linux 7.3's
// e820__mapped_all() asks of its reserved entries.
static bool memmap_reserves(uint64_t base, uint64_t end) {
    uint64_t at = base;
    bool advanced = true;
    while (at < end && advanced) {
        advanced = false;
        const struct efi_memory_descriptor *d;
        for (size_t i = 0; (d = handoff_efi_desc(core_handoff, i)) != NULL; i++) {
            uint64_t stop;
            if (handoff_efi_desc_end(d, &stop) && type_reserved(d->type) && d->physical_start <= at
             && at < stop) {
                at = stop;
                advanced = true;
            }
        }
    }
    return at >= end;
}

// Whether one EfiMemoryMappedIO descriptor covers all of [base, end), which
// Linux 7.3's is_efi_mmio() takes for an ECAM reservation where the system
// board devices make none.
static bool memmap_mmio(uint64_t base, uint64_t end) {
    const struct efi_memory_descriptor *d;
    for (size_t i = 0; (d = handoff_efi_desc(core_handoff, i)) != NULL; i++) {
        uint64_t stop;
        if (handoff_efi_desc_end(d, &stop) && d->type == EFI_MEMORY_MAPPED_IO
         && d->physical_start <= base && end <= stop) {
            return true;
        }
    }
    return false;
}

static uint64_t window_start(const struct ecam_window *w) {
    return w->base + ((uint64_t)w->start_bus << 20);
}

static uint64_t window_end(const struct ecam_window *w) {
    return w->base + (((uint64_t)w->end_bus + 1) << 20);
}

// Whether something reserves the window's buses, or the first of them in a
// part no smaller than 16 MiB, to which the window is cut, as Linux 7.3's
// is_mmconf_reserved() does.
static bool window_reserved(struct ecam_window *w, bool (*reserves)(uint64_t, uint64_t)) {
    uint64_t start = window_start(w);
    uint64_t full = window_end(w) - start;
    uint64_t size = full;
    while (!reserves(start, start + size)) {
        size >>= 1;
        if (size < ECAM_MIN_RESERVED) {
            break;
        }
    }
    if (size < ECAM_MIN_RESERVED && size != full) {
        return false;
    }
    w->end_bus = (uint8_t)(w->start_bus + (size >> 20) - 1);
    return true;
}

static void log_cut(const struct ecam_window *w, uint8_t end_bus) {
    if (w->end_bus != end_bus) {
        printf(
            "PCI segment %u: configuration space cut to buses %02x-%02x, as only those are "
            "reserved\n", w->segment, w->start_bus, w->end_bus
        );
    }
}

static void publish(struct ecam_window *list, size_t count) {
    windows = list;
    atomic_store_explicit(&window_count, count, memory_order_release);
}

// As Linux 7.3's pci_check_type1() has it: CONFIG_ADDRESS holds what is
// written to it, and a device answers on bus 0.
static bool check_ports(void) {
    spinlock_acquire(&legacy_lock);
    uint32_t saved = inl(PCI_CFG_ADDRESS);
    outl(PCI_CFG_ADDRESS, PCI_CFG_ENABLE);
    bool holds = inl(PCI_CFG_ADDRESS) == PCI_CFG_ENABLE;
    bool device = holds && inw(PCI_CFG_DATA) != 0xffff;
    outl(PCI_CFG_ADDRESS, saved);
    spinlock_release(&legacy_lock);
    return device;
}

bool pci_cfg_ports_answer(void) {
    return ports_answer;
}

void pci_cfg_init(void) {
    if (mcfg_done) {
        return;
    }
    ports_answer = check_ports();
    if (!acpi_tables_ready()) {
        printf("PCI configuration space through ports only, without ACPI tables\n");
        return;
    }
    mcfg_done = true;

    struct uacpi_table table;
    uacpi_status status = uacpi_table_find_by_signature(ACPI_MCFG_SIGNATURE, &table);
    if (status != UACPI_STATUS_OK) {
        printf(
            "PCI configuration space through ports only, no MCFG table: %s\n",
            uacpi_status_to_string(status)
        );
        return;
    }

    size_t length = table.hdr->length;
    size_t count = 0;
    if (length > sizeof(struct acpi_mcfg)) {
        count = (length - sizeof(struct acpi_mcfg)) / sizeof(struct acpi_mcfg_allocation);
    }

    struct ecam_window *list = kmalloc(count * sizeof(struct ecam_window));
    size_t used = 0;
    uintptr_t top = core_handoff->map_top;
    for (size_t i = 0; i < count; i++) {
        struct acpi_mcfg_allocation entry;
        memcpy(
            &entry,
            (const uint8_t *)table.ptr + sizeof(struct acpi_mcfg)
          + i * sizeof(struct acpi_mcfg_allocation), sizeof(struct acpi_mcfg_allocation)
        );

        // The base is that of bus 0, whatever the first bus.
        size_t size = ((uint64_t)entry.end_bus + 1) << 20;
        bool usable = entry.address != 0 && entry.start_bus <= entry.end_bus && size <= top
                   && entry.address <= top - size;
        printf(
            "PCI segment %u buses %02x-%02x: configuration space at %#llx%s\n", entry.segment,
            entry.start_bus, entry.end_bus, (unsigned long long)entry.address,
            usable ? "" : ", not usable"
        );
        if (usable) {
            list[used].base = entry.address;
            list[used].segment = entry.segment;
            list[used].start_bus = entry.start_bus;
            list[used].end_bus = entry.end_bus;
            used++;
        }
    }
    uacpi_table_unref(&table);

    if (used == 0) {
        kfree(list);
        return;
    }
    // Before the namespace can show what reserves ECAM space, Linux 7.3's
    // pci_mmcfg_reserved() asks the memory map to only for firmware dated
    // before 2016, an undated one among it, and where the map does not, waits
    // for the system board devices.
    if (core_handoff->bios_year < ECAM_TRUSTED_YEAR && ports_answer) {
        struct ecam_window *cut = kmalloc(used * sizeof(struct ecam_window));
        memcpy(cut, list, used * sizeof(struct ecam_window));
        for (size_t i = 0; i < used; i++) {
            if (!window_reserved(&cut[i], memmap_reserves)) {
                printf(
                    "PCI configuration space through ports until the ACPI namespace runs, as "
                    "the memory map does not reserve segment %u's\n", list[i].segment
                );
                kfree(cut);
                pending = list;
                pending_count = used;
                return;
            }
        }
        for (size_t i = 0; i < used; i++) {
            log_cut(&cut[i], list[i].end_bus);
        }
        kfree(list);
        list = cut;
    }
    publish(list, used);
}

void pci_cfg_verify(void) {
    struct ecam_window *list = pending;
    if (list == NULL) {
        return;
    }
    pending = NULL;
    for (size_t i = 0; i < pending_count; i++) {
        struct ecam_window *w = &list[i];
        uint8_t end_bus = w->end_bus;
        if (window_reserved(w, acpi_motherboard_reserves)) {
            log_cut(w, end_bus);
        } else if (!memmap_mmio(window_start(w), window_end(w))) {
            printf(
                "PCI configuration space through ports only, as nothing reserves segment %u's "
                "at %#llx\n", w->segment, (unsigned long long)window_start(w)
            );
            kfree(list);
            return;
        }
    }
    printf("PCI configuration space reserved by the ACPI namespace or the memory map\n");
    publish(list, pending_count);
}
