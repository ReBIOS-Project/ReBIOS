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
#include <memory.h>
#include <hw/pci_cfg.h>
#include <hw/pci_enum.h>
#include <lib/log.h>
#include <lib/spinlock.h>
#include <sys/x86.h>
#include <virt/gpt.h>
#include <virt/io.h>
#include <virt/mmio.h>
#include <virt/pci_virt.h>

#define CONFIG_ADDRESS 0xcf8
#define CONFIG_DATA 0xcfc
#define CONFIG_PORTS 8
#define CONFIG_ENABLE 0x80000000u

#define FUNCTION_SIZE 0x1000
#define DEVICE_SIZE (8 * FUNCTION_SIZE)

static struct {
    bool on;
    uint8_t device;
    const struct pci_virt_ops *ops;
    void *ctx;
    // The guest's configuration address, which never reaches the hardware:
    // the hypervisor makes every data access itself.
    uint32_t config_address;
    struct spinlock lock;
} virt;

static uint32_t all_ones(unsigned size) {
    return size == 4 ? 0xffffffffu : (1u << (8 * size)) - 1;
}

static uint32_t function_read(uint8_t bus, uint8_t device, uint8_t function, uint32_t offset,
                              unsigned size) {
    if (bus == 0 && device == virt.device) {
        return function == 0 ? virt.ops->read(virt.ctx, offset, size) : all_ones(size);
    }
    return pci_cfg_read((struct pci_addr){0, bus, device, function}, offset, size);
}

static void function_write(uint8_t bus, uint8_t device, uint8_t function, uint32_t offset,
                           unsigned size, uint32_t value) {
    if (bus == 0 && device == virt.device) {
        if (function == 0) {
            virt.ops->write(virt.ctx, offset, size, value);
        }
        return;
    }
    pci_cfg_write((struct pci_addr){0, bus, device, function}, offset, size, value);
}

static void port_native(uint16_t port, unsigned size, bool in, uint32_t *value) {
    switch (size) {
        case 1: {
            if (in) {
                *value = inb(port);
            } else {
                outb(port, (uint8_t)*value);
            }
            break;
        }
        case 2: {
            if (in) {
                *value = inw(port);
            } else {
                outw(port, (uint16_t)*value);
            }
            break;
        }
        default: {
            if (in) {
                *value = inl(port);
            } else {
                outl(port, *value);
            }
            break;
        }
    }
}

// Only dword accesses to CF8h are the configuration address: the others, such
// as the reset control register at CF9h on many chipsets, and data accesses
// with the address disabled, go on to the hardware.
static void config_ports(struct vcpu *v, void *ctx, uint16_t offset, unsigned size, bool in,
                         uint32_t *value) {
    (void)v;
    (void)ctx;
    uint16_t port = (uint16_t)(CONFIG_ADDRESS + offset);
    if (port == CONFIG_ADDRESS && size == 4) {
        spinlock_acquire(&virt.lock);
        if (in) {
            *value = virt.config_address;
        } else {
            virt.config_address = *value & ~3u;
        }
        spinlock_release(&virt.lock);
        return;
    }
    uint32_t address = __atomic_load_n(&virt.config_address, __ATOMIC_RELAXED);
    unsigned byte = (unsigned)(port - CONFIG_DATA);
    if (port < CONFIG_DATA || !(address & CONFIG_ENABLE) || byte + size > 4) {
        port_native(port, size, in, value);
        return;
    }
    uint8_t bus = (uint8_t)(address >> 16);
    uint8_t device = (address >> 11) & 0x1f;
    uint8_t function = (address >> 8) & 7;
    // Bits 27:24 extend the register number where the chipset allows it.
    uint32_t reg = (address & 0xfc) | ((address >> 16) & 0xf00);
    if (byte % size == 0) {
        if (in) {
            *value = function_read(bus, device, function, reg + byte, size);
        } else {
            function_write(bus, device, function, reg + byte, size, *value);
        }
        return;
    }
    uint32_t result = 0;
    for (unsigned i = 0; i < size; i++) {
        if (in) {
            result |= function_read(bus, device, function, reg + byte + i, 1) << (8 * i);
        } else {
            function_write(bus, device, function, reg + byte + i, 1, (*value >> (8 * i)) & 0xff);
        }
    }
    if (in) {
        *value = result;
    }
}

static uint64_t ecam_read(struct vcpu *v, void *ctx, uint64_t offset, unsigned size) {
    (void)v;
    (void)ctx;
    uint8_t function = (uint8_t)(offset / FUNCTION_SIZE);
    uint32_t reg = (uint32_t)(offset % FUNCTION_SIZE);
    if (size == 8) {
        return function_read(0, virt.device, function, reg, 4)
               | (uint64_t)function_read(0, virt.device, function, reg + 4, 4) << 32;
    }
    return function_read(0, virt.device, function, reg, size);
}

static void ecam_write(struct vcpu *v, void *ctx, uint64_t offset, unsigned size, uint64_t value) {
    (void)v;
    (void)ctx;
    uint8_t function = (uint8_t)(offset / FUNCTION_SIZE);
    uint32_t reg = (uint32_t)(offset % FUNCTION_SIZE);
    if (size == 8) {
        function_write(0, virt.device, function, reg, 4, (uint32_t)value);
        function_write(0, virt.device, function, reg + 4, 4, (uint32_t)(value >> 32));
        return;
    }
    function_write(0, virt.device, function, reg, size, (uint32_t)value);
}

static const struct mmio_ops ecam_ops = {ecam_read, ecam_write, false};

static bool device_free(const struct pci_topology *t, uint8_t device) {
    for (uint32_t i = 0; i < t->function_count; i++) {
        const struct pci_function *f = &t->functions[i];
        if (f->addr.segment == 0 && f->addr.bus == 0 && f->addr.device == device) {
            return false;
        }
    }
    return pci_cfg_read((struct pci_addr){0, 0, device, 0}, 0, 2) == 0xffff;
}

int pci_virt_add(const struct pci_virt_ops *ops, void *ctx) {
    const struct pci_topology *t = pci_enum_topology();
    if (virt.on || t == NULL) {
        return -1;
    }
    int device = -1;
    for (uint8_t d = 1; d < 32 && device < 0; d++) {
        if (device_free(t, d)) {
            device = d;
        }
    }
    if (device < 0) {
        printf("No free device number on PCI bus 0 for a virtual device\n");
        return -1;
    }
    virt.device = (uint8_t)device;
    virt.ops = ops;
    virt.ctx = ctx;
    virt.on = true;

    struct io_handler ports = {CONFIG_ADDRESS, CONFIG_PORTS, config_ports, NULL};
    io_register(&ports);
    uint64_t ecam = pci_cfg_ecam_base(0, 0);
    if (ecam != 0) {
        uint64_t base = ecam + (uint64_t)device * DEVICE_SIZE;
        mmio_register(base, DEVICE_SIZE, &ecam_ops, NULL);
        gpt_map(base, 0, DEVICE_SIZE, 0, 0);
        gpt_flush();
    }
    return device;
}
