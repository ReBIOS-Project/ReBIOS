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
#include <memory.h>
#include <core.h>
#include <hw/acpi.h>
#include <hw/pci_cfg.h>
#include <lib/heap.h>
#include <lib/log.h>
#include <lib/spinlock.h>
#include <sys/x86.h>

#define CONFIG_ADDRESS 0xcf8
#define CONFIG_DATA 0xcfc
#define CONFIG_ENABLE (1u << 31)

struct ecam_window {
    uint64_t base;
    uint16_t segment;
    uint8_t start_bus;
    uint8_t end_bus;
};

static struct ecam_window *windows;
static unsigned window_count;
static bool mcfg_done;
static struct spinlock legacy_lock;

static bool access_valid(struct pci_addr addr, uint32_t offset, unsigned size) {
    if (size != 1 && size != 2 && size != 4) {
        return false;
    }
    return offset < 4096 && offset % size == 0 && addr.device < 32 && addr.function < 8;
}

static uint32_t all_ones(unsigned size) {
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

uint64_t pci_cfg_ecam_base(uint16_t segment, uint8_t bus) {
    unsigned count = __atomic_load_n(&window_count, __ATOMIC_ACQUIRE);
    for (unsigned i = 0; i < count; i++) {
        struct ecam_window *w = &windows[i];
        if (w->segment == segment && bus >= w->start_bus && bus <= w->end_bus) {
            return w->base;
        }
    }
    return 0;
}

static uintptr_t ecam_address(uint64_t base, struct pci_addr addr, uint32_t offset) {
    return (uintptr_t)(base + (((uint64_t)addr.bus << 20) | ((uint64_t)addr.device << 15)
                               | ((uint64_t)addr.function << 12) | offset));
}

static uint32_t legacy_address(struct pci_addr addr, uint32_t offset) {
    return CONFIG_ENABLE | ((uint32_t)addr.bus << 16) | ((uint32_t)addr.device << 11)
           | ((uint32_t)addr.function << 8) | (offset & 0xfc);
}

static bool legacy_reachable(struct pci_addr addr, uint32_t offset) {
    return addr.segment == 0 && offset < 256;
}

uint32_t pci_cfg_read(struct pci_addr addr, uint32_t offset, unsigned size) {
    if (!access_valid(addr, offset, size)) {
        return all_ones(size);
    }

    uint64_t base = pci_cfg_ecam_base(addr.segment, addr.bus);
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

    uint16_t port = CONFIG_DATA + (offset & 3);
    uint32_t value;
    spinlock_acquire(&legacy_lock);
    // The guest may be between its own write to CONFIG_ADDRESS and the data
    // access that goes with it.
    uint32_t saved = inl(CONFIG_ADDRESS);
    outl(CONFIG_ADDRESS, legacy_address(addr, offset));
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
    outl(CONFIG_ADDRESS, saved);
    spinlock_release(&legacy_lock);
    return value;
}

void pci_cfg_write(struct pci_addr addr, uint32_t offset, unsigned size, uint32_t value) {
    if (!access_valid(addr, offset, size)) {
        return;
    }

    uint64_t base = pci_cfg_ecam_base(addr.segment, addr.bus);
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

    uint16_t port = CONFIG_DATA + (offset & 3);
    spinlock_acquire(&legacy_lock);
    uint32_t saved = inl(CONFIG_ADDRESS);
    outl(CONFIG_ADDRESS, legacy_address(addr, offset));
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
    outl(CONFIG_ADDRESS, saved);
    spinlock_release(&legacy_lock);
}

void pci_cfg_init(void) {
    if (mcfg_done) {
        return;
    }
    if (!acpi_tables_ready()) {
        printf("PCI configuration space through ports only, without ACPI tables\n");
        return;
    }
    mcfg_done = true;

    struct uacpi_table table;
    uacpi_status status = uacpi_table_find_by_signature(ACPI_MCFG_SIGNATURE, &table);
    if (status != UACPI_STATUS_OK) {
        printf("PCI configuration space through ports only, no MCFG table: %s\n",
               uacpi_status_to_string(status));
        return;
    }

    uint32_t length = table.hdr->length;
    size_t count = 0;
    if (length > sizeof(struct acpi_mcfg)) {
        count = (length - sizeof(struct acpi_mcfg)) / sizeof(struct acpi_mcfg_allocation);
    }

    struct ecam_window *list = kmalloc(count * sizeof(*list));
    unsigned used = 0;
    uint64_t top = core_handoff->map_top;
    for (size_t i = 0; i < count; i++) {
        struct acpi_mcfg_allocation entry;
        memcpy(&entry, (const uint8_t *)table.ptr + sizeof(struct acpi_mcfg) + i * sizeof(entry),
               sizeof(entry));

        // The base is that of bus 0, whatever the first bus.
        uint64_t size = ((uint64_t)entry.end_bus + 1) << 20;
        bool usable = entry.address != 0 && entry.start_bus <= entry.end_bus && size <= top
                      && entry.address <= top - size;
        printf("PCI segment %u buses %02x-%02x: configuration space at %#llx%s\n",
               entry.segment, entry.start_bus, entry.end_bus, (unsigned long long)entry.address,
               usable ? "" : ", not usable");
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
    windows = list;
    __atomic_store_n(&window_count, used, __ATOMIC_RELEASE);
}
