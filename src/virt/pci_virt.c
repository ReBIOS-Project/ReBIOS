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
#include <stdbool.h>
#include <uacpi/acpi.h>
#include <uacpi/status.h>
#include <uacpi/tables.h>
#include <memory.h>
#include <hw/acpi.h>
#include <hw/pci_cfg.h>
#include <hw/pci_enum.h>
#include <lib/heap.h>
#include <lib/log.h>
#include <lib/spinlock.h>
#include <sys/mm.h>
#include <sys/x86.h>
#include <virt/gpt.h>
#include <virt/io.h>
#include <virt/layout.h>
#include <virt/mmio.h>
#include <virt/pci_plan.h>
#include <virt/pci_virt.h>

#define CONFIG_ADDRESS 0xcf8
#define CONFIG_DATA 0xcfc
#define CONFIG_PORTS 8
#define CONFIG_ENABLE 0x80000000u

#define FUNCTION_SIZE 0x1000u
#define BUS_SIZE 0x100000ull

// PCI Local Bus Specification 3.0, 6.1 and 6.2.5, and PCI-to-PCI Bridge
// Architecture Specification 1.1, 3.2.
#define REG_HEADER_END 0x40
#define REG_BAR0 0x10
#define REG_BUS_NUMBERS 0x18
#define REG_MEMORY 0x20
#define REG_PREF 0x24
#define REG_PREF_BASE_UPPER 0x28
#define REG_PREF_LIMIT_UPPER 0x2c
#define REG_ROM 0x30
#define REG_BRIDGE_ROM 0x38
#define HEADER_DEVICE 0
#define HEADER_BRIDGE 1
#define BAR_ADDRESS 0xfffffff0u
#define ROM_ADDRESS 0xfffff800u
#define ROM_ENABLE 0x1u
#define WINDOW_ADDRESS 0xfff0u
#define WINDOW_PREF_64 0x1u
#define WINDOW_GRANULE 0x100000ull
// The latency timer of a bridge's bus number register is the hardware's.
#define BUS_NUMBERS_VIRTUAL 0x00ffffffu

// PCI Express Base Specification 6.0, 7.8.6: the capability header, then a
// capability and a control register for each of up to six BARs.
#define REBAR_SIZE 0x34u

// An AMD IOMMU's function, whose capability holds its registers' address.
#define CLASS_SYSTEM 0x08
#define SUBCLASS_IOMMU 0x06

#define BAR_COUNT 6
#define ROM_INDEX 6

// A memory BAR or expansion ROM BAR as the guest has it.
struct vbar {
    uint8_t kind;
    // The read-only type bits of its (lower) register.
    uint8_t type_bits;
    uint64_t size;
    uint64_t host;
    uint64_t guest;
    bool enabled;
    // Where the guest's range maps to the host's, 0 if nowhere.
    uint64_t mapped;
};

struct vfunction {
    struct pci_function *f;
    bool hidden;
    // A 64-bit BAR at the index of its lower register; a ROM at ROM_INDEX.
    struct vbar bars[BAR_COUNT + 1];
    uint32_t bus_numbers;
    uint64_t mem_base;
    uint64_t mem_limit;
    uint64_t pref_base;
    uint64_t pref_limit;
    bool pref_64;
};

struct ecam_window {
    uint16_t segment;
    uint8_t start;
};

static struct {
    bool on;
    // The guest's configuration address, which never reaches the hardware:
    // the hypervisor makes every data access itself.
    uint32_t config_address;
    struct spinlock lock;
    struct vfunction *functions;
    uint32_t count;
    // The guest-physical addresses the processor implements.
    uint64_t address_limit;
} virt;

static uint32_t all_ones(unsigned size) {
    return size == 4 ? 0xffffffffu : (1u << (8 * size)) - 1;
}

// In the topology's order.
static uint32_t addr_key(struct pci_addr a) {
    return (uint32_t)a.segment << 16 | (uint32_t)a.bus << 8 | (uint32_t)a.device << 3 | a.function;
}

static struct vfunction *vfunction_find(struct pci_addr a) {
    uint32_t key = addr_key(a);
    uint32_t low = 0;
    uint32_t high = virt.count;
    while (low < high) {
        uint32_t mid = low + (high - low) / 2;
        uint32_t mkey = addr_key(virt.functions[mid].f->addr);
        if (mkey == key) {
            return &virt.functions[mid];
        }
        if (mkey < key) {
            low = mid + 1;
        } else {
            high = mid;
        }
    }
    return NULL;
}

static uint64_t page_down(uint64_t x) {
    return x & ~(uint64_t)(PAGE_SIZE - 1);
}

static uint64_t page_up(uint64_t x) {
    return (x + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
}

// The guest's pages for the BAR at guest, which must sit at the same offset in
// its page as the host's BAR does in the host's, as whole pages map.
static bool mappable(const struct vbar *b, uint64_t guest) {
    uint64_t base = page_down(guest);
    uint64_t end = page_up(guest + b->size);
    return ((guest ^ b->host) & (PAGE_SIZE - 1)) == 0 && end > base && end <= virt.address_limit
           && layout_mmio_free(base, end - base);
}

static void map(const struct vbar *b) {
    uint64_t base = page_down(b->mapped);
    gpt_map(base, page_down(b->host), page_up(b->mapped + b->size) - base, GPT_RWX, MT_UC);
}

static bool overlaps(const struct vbar *b, uint64_t base, uint64_t end) {
    return b->mapped != 0 && page_down(b->mapped) < end && page_up(b->mapped + b->size) > base;
}

// What else is mapped in [base, end), which an unmapping there took away.
static void map_again(uint64_t base, uint64_t end) {
    for (uint32_t i = 0; i < virt.count; i++) {
        for (unsigned k = 0; k <= BAR_COUNT; k++) {
            if (overlaps(&virt.functions[i].bars[k], base, end)) {
                map(&virt.functions[i].bars[k]);
            }
        }
    }
}

// Follows the guest's address and enables for the BAR. Where the guest puts it
// somewhere it cannot map, such as at the size mask while it sizes the BAR, it
// is in no place.
static void bar_update(struct vbar *b, bool is_rom) {
    uint64_t want = 0;
    if (b->guest != 0 && (!is_rom || b->enabled) && mappable(b, b->guest)) {
        want = b->guest;
    }
    if (want == b->mapped) {
        return;
    }
    if (b->mapped != 0) {
        uint64_t base = page_down(b->mapped);
        uint64_t end = page_up(b->mapped + b->size);
        b->mapped = 0;
        gpt_map(base, 0, end - base, 0, 0);
        map_again(base, end);
    }
    if (want != 0) {
        b->mapped = want;
        map(b);
    }
    gpt_flush();
}

// The register at reg, if the hypervisor keeps it: its BAR, and whether it is
// the upper half of a 64-bit one.
static struct vbar *bar_at(struct vfunction *vf, uint32_t reg, bool *upper) {
    unsigned bars = vf->f->header_type == HEADER_BRIDGE ? 2 : BAR_COUNT;
    uint32_t rom = vf->f->header_type == HEADER_BRIDGE ? REG_BRIDGE_ROM : REG_ROM;
    *upper = false;
    if (reg == rom) {
        return vf->bars[ROM_INDEX].kind == PCI_BAR_MEM32 ? &vf->bars[ROM_INDEX] : NULL;
    }
    if (reg < REG_BAR0 || reg >= REG_BAR0 + 4 * bars) {
        return NULL;
    }
    unsigned i = (reg - REG_BAR0) / 4;
    struct vbar *b = &vf->bars[i];
    if (b->kind == PCI_BAR_MEM32 || b->kind == PCI_BAR_MEM64) {
        return b;
    }
    if (i > 0 && vf->bars[i - 1].kind == PCI_BAR_MEM64) {
        *upper = true;
        return &vf->bars[i - 1];
    }
    return NULL;
}

static bool is_bridge(const struct vfunction *vf) {
    return vf->f->header_type == HEADER_BRIDGE;
}

static bool window_reg(const struct vfunction *vf, uint32_t reg) {
    return is_bridge(vf) && (reg == REG_MEMORY || reg == REG_PREF || reg == REG_PREF_BASE_UPPER
                             || reg == REG_PREF_LIMIT_UPPER);
}

static uint64_t bar_mask(const struct vbar *b) {
    return ~(b->size - 1);
}

static uint32_t window_word(uint64_t address) {
    return (uint32_t)(address >> 16) & WINDOW_ADDRESS;
}

// The dword at reg of a register the hypervisor keeps.
static uint32_t kept_read(struct vfunction *vf, uint32_t reg) {
    bool upper;
    struct vbar *b = bar_at(vf, reg, &upper);
    if (b != NULL) {
        if (b == &vf->bars[ROM_INDEX]) {
            return ((uint32_t)b->guest & ROM_ADDRESS & (uint32_t)bar_mask(b))
                   | (b->enabled ? ROM_ENABLE : 0);
        }
        if (upper) {
            return (uint32_t)((b->guest & bar_mask(b)) >> 32);
        }
        return ((uint32_t)b->guest & BAR_ADDRESS & (uint32_t)bar_mask(b)) | b->type_bits;
    }
    switch (reg) {
        case REG_BUS_NUMBERS: {
            return (vf->bus_numbers & BUS_NUMBERS_VIRTUAL)
                   | (pci_cfg_read(vf->f->addr, reg, 4) & ~BUS_NUMBERS_VIRTUAL);
        }
        case REG_MEMORY: {
            return window_word(vf->mem_base) | window_word(vf->mem_limit) << 16;
        }
        case REG_PREF: {
            uint32_t width = vf->pref_64 ? WINDOW_PREF_64 : 0;
            return (window_word(vf->pref_base) | width)
                   | (window_word(vf->pref_limit) | width) << 16;
        }
        case REG_PREF_BASE_UPPER: {
            return vf->pref_64 ? (uint32_t)(vf->pref_base >> 32) : 0;
        }
        default: {
            return vf->pref_64 ? (uint32_t)(vf->pref_limit >> 32) : 0;
        }
    }
}

static void kept_write(struct vfunction *vf, uint32_t reg, uint32_t value) {
    bool upper;
    struct vbar *b = bar_at(vf, reg, &upper);
    if (b != NULL) {
        if (b == &vf->bars[ROM_INDEX]) {
            b->guest = value & ROM_ADDRESS & (uint32_t)bar_mask(b);
            b->enabled = (value & ROM_ENABLE) != 0;
            // The function decodes its ROM where the host has it.
            pci_cfg_write(vf->f->addr, reg, 4,
                          ((uint32_t)b->host & ROM_ADDRESS) | (b->enabled ? ROM_ENABLE : 0));
            bar_update(b, true);
        } else if (upper) {
            b->guest = (b->guest & 0xffffffffull) | ((uint64_t)value << 32 & bar_mask(b));
            bar_update(b, false);
        } else {
            b->guest = (b->guest & ~0xffffffffull) | (value & BAR_ADDRESS & bar_mask(b));
            bar_update(b, false);
        }
        return;
    }
    switch (reg) {
        case REG_BUS_NUMBERS: {
            pci_cfg_write(vf->f->addr, reg + 3, 1, value >> 24);
            break;
        }
        case REG_MEMORY: {
            vf->mem_base = (uint64_t)(value & WINDOW_ADDRESS) << 16;
            vf->mem_limit = (uint64_t)(value >> 16 & WINDOW_ADDRESS) << 16 | (WINDOW_GRANULE - 1);
            break;
        }
        case REG_PREF: {
            vf->pref_base = (vf->pref_base & ~0xffffffffull)
                            | (uint64_t)(value & WINDOW_ADDRESS) << 16;
            vf->pref_limit = (vf->pref_limit & ~0xffffffffull)
                             | (uint64_t)(value >> 16 & WINDOW_ADDRESS) << 16
                             | (WINDOW_GRANULE - 1);
            break;
        }
        case REG_PREF_BASE_UPPER: {
            if (vf->pref_64) {
                vf->pref_base = (vf->pref_base & 0xffffffffull) | (uint64_t)value << 32;
            }
            break;
        }
        default: {
            if (vf->pref_64) {
                vf->pref_limit = (vf->pref_limit & 0xffffffffull) | (uint64_t)value << 32;
            }
            break;
        }
    }
}

static bool is_kept(struct vfunction *vf, uint32_t reg) {
    bool upper;
    return bar_at(vf, reg, &upper) != NULL || window_reg(vf, reg)
           || (is_bridge(vf) && reg == REG_BUS_NUMBERS);
}

// Resizing a BAR would move it on the host, behind the guest's back.
static bool in_rebar(const struct vfunction *vf, uint32_t offset) {
    uint32_t cap = vf->f->rebar_ext_cap;
    return cap != 0 && offset >= cap && offset < cap + REBAR_SIZE;
}

static uint32_t function_read(struct pci_addr a, uint32_t offset, unsigned size) {
    struct vfunction *vf = vfunction_find(a);
    if (vf != NULL && vf->hidden) {
        return all_ones(size);
    }
    uint32_t reg = offset & ~3u;
    if (vf == NULL || offset >= REG_HEADER_END || !is_kept(vf, reg)) {
        return pci_cfg_read(a, offset, size);
    }
    return (kept_read(vf, reg) >> (8 * (offset & 3))) & all_ones(size);
}

static void function_write(struct pci_addr a, uint32_t offset, unsigned size, uint32_t value) {
    struct vfunction *vf = vfunction_find(a);
    if (vf != NULL && (vf->hidden || in_rebar(vf, offset))) {
        return;
    }
    uint32_t reg = offset & ~3u;
    if (vf == NULL || offset >= REG_HEADER_END || !is_kept(vf, reg)) {
        pci_cfg_write(a, offset, size, value);
        return;
    }
    unsigned shift = 8 * (offset & 3);
    uint32_t mask = all_ones(size) << shift;
    kept_write(vf, reg, (kept_read(vf, reg) & ~mask) | ((value << shift) & mask));
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
    spinlock_acquire(&virt.lock);
    if (port == CONFIG_ADDRESS && size == 4) {
        if (in) {
            *value = virt.config_address;
        } else {
            virt.config_address = *value & ~3u;
        }
        spinlock_release(&virt.lock);
        return;
    }
    uint32_t address = virt.config_address;
    unsigned byte = (unsigned)(port - CONFIG_DATA);
    if (port < CONFIG_DATA || !(address & CONFIG_ENABLE) || byte + size > 4) {
        port_native(port, size, in, value);
        spinlock_release(&virt.lock);
        return;
    }
    struct pci_addr a = {0, (uint8_t)(address >> 16), (address >> 11) & 0x1f, (address >> 8) & 7};
    // Bits 27:24 extend the register number where the chipset allows it.
    uint32_t reg = (address & 0xfc) | ((address >> 16) & 0xf00);
    if (byte % size == 0) {
        if (in) {
            *value = function_read(a, reg + byte, size);
        } else {
            function_write(a, reg + byte, size, *value);
        }
    } else {
        uint32_t result = 0;
        for (unsigned i = 0; i < size; i++) {
            if (in) {
                result |= function_read(a, reg + byte + i, 1) << (8 * i);
            } else {
                function_write(a, reg + byte + i, 1, (*value >> (8 * i)) & 0xff);
            }
        }
        if (in) {
            *value = result;
        }
    }
    spinlock_release(&virt.lock);
}

static struct pci_addr ecam_addr(const struct ecam_window *w, uint64_t offset, uint32_t *reg) {
    *reg = (uint32_t)(offset % FUNCTION_SIZE);
    return (struct pci_addr){w->segment, (uint8_t)(w->start + offset / BUS_SIZE),
                             (uint8_t)((offset >> 15) & 0x1f), (uint8_t)((offset >> 12) & 7)};
}

static uint64_t ecam_read(struct vcpu *v, void *ctx, uint64_t offset, unsigned size) {
    (void)v;
    uint32_t reg;
    struct pci_addr a = ecam_addr(ctx, offset, &reg);
    spinlock_acquire(&virt.lock);
    uint64_t value;
    if (size == 8) {
        value = function_read(a, reg, 4) | (uint64_t)function_read(a, reg + 4, 4) << 32;
    } else {
        value = function_read(a, reg, size);
    }
    spinlock_release(&virt.lock);
    return value;
}

static void ecam_write(struct vcpu *v, void *ctx, uint64_t offset, unsigned size, uint64_t value) {
    (void)v;
    uint32_t reg;
    struct pci_addr a = ecam_addr(ctx, offset, &reg);
    spinlock_acquire(&virt.lock);
    if (size == 8) {
        function_write(a, reg, 4, (uint32_t)value);
        function_write(a, reg + 4, 4, (uint32_t)(value >> 32));
    } else {
        function_write(a, reg, size, (uint32_t)value);
    }
    spinlock_release(&virt.lock);
}

static const struct mmio_ops ecam_ops = {ecam_read, ecam_write, false};

static void ecam_trap(uint16_t segment, uint8_t start, uint8_t end, uint64_t base) {
    struct ecam_window *w = kmalloc(sizeof(*w));
    *w = (struct ecam_window){segment, start};
    uint64_t size = ((uint64_t)end - start + 1) * BUS_SIZE;
    mmio_register(base, size, &ecam_ops, w);
    gpt_map(base, 0, size, 0, 0);
}

// Segment 0's window is where the plan put it, the others' where the host has
// them, for 64-bit guests: at addresses the guest's RAM keeps clear of.
static void ecam_windows(const struct pci_plan *plan) {
    if (plan->ecam != 0) {
        ecam_trap(0, plan->ecam_start, plan->ecam_end, plan->ecam);
    }
    struct uacpi_table table;
    if (!acpi_tables_ready()
        || uacpi_table_find_by_signature(ACPI_MCFG_SIGNATURE, &table) != UACPI_STATUS_OK) {
        return;
    }
    uint32_t length = table.hdr->length;
    for (uint32_t off = sizeof(struct acpi_mcfg);
         off <= length && length - off >= sizeof(struct acpi_mcfg_allocation);
         off += sizeof(struct acpi_mcfg_allocation)) {
        struct acpi_mcfg_allocation entry;
        memcpy(&entry, (const uint8_t *)table.ptr + off, sizeof(entry));
        uint64_t base = entry.address + ((uint64_t)entry.start_bus << 20);
        uint64_t size = ((uint64_t)entry.end_bus - entry.start_bus + 1) * BUS_SIZE;
        if (entry.segment == 0 || entry.start_bus > entry.end_bus) {
            continue;
        }
        if (layout_mmio_free(base, size)) {
            ecam_trap(entry.segment, entry.start_bus, entry.end_bus, base);
        } else {
            printf("PCI segment %04x: its configuration space window at 0x%llx is in the "
                   "guest's memory, so the guest cannot reach it\n", entry.segment,
                   (unsigned long long)base);
        }
    }
    uacpi_table_unref(&table);
}

static void vfunction_init(struct vfunction *vf, struct pci_function *f,
                           const struct pci_plan_function *pf) {
    *vf = (struct vfunction){.f = f};
    if (f->base_class == CLASS_SYSTEM && f->subclass == SUBCLASS_IOMMU) {
        vf->hidden = true;
        return;
    }
    if (f->header_type != HEADER_DEVICE && f->header_type != HEADER_BRIDGE) {
        return;
    }
    for (unsigned i = 0; i < f->bar_count && i < BAR_COUNT; i++) {
        const struct pci_bar *bar = &f->bars[i];
        if ((bar->kind != PCI_BAR_MEM32 && bar->kind != PCI_BAR_MEM64) || bar->size == 0) {
            continue;
        }
        vf->bars[i] = (struct vbar){
            .kind = bar->kind,
            .type_bits = (uint8_t)(pci_cfg_read(f->addr, bar->reg, 4) & ~BAR_ADDRESS),
            .size = bar->size,
            .host = bar->base,
            .guest = pf->bars[i],
        };
    }
    if (f->rom.kind == PCI_BAR_MEM32 && f->rom.size != 0) {
        uint32_t reg = f->header_type == HEADER_BRIDGE ? REG_BRIDGE_ROM : REG_ROM;
        vf->bars[ROM_INDEX] = (struct vbar){
            .kind = PCI_BAR_MEM32,
            .size = f->rom.size,
            .host = f->rom.base,
            .guest = pf->rom,
            .enabled = (pci_cfg_read(f->addr, reg, 4) & ROM_ENABLE) != 0,
        };
    }
    if (f->header_type == HEADER_BRIDGE) {
        vf->bus_numbers = pci_cfg_read(f->addr, REG_BUS_NUMBERS, 4);
        vf->mem_base = pf->mem_base;
        vf->mem_limit = pf->mem_limit;
        vf->pref_base = pf->pref_base;
        vf->pref_limit = pf->pref_limit;
        vf->pref_64 = (pci_cfg_read(f->addr, REG_PREF, 4) & 0xf) == WINDOW_PREF_64;
    }
}

void pci_virt_init(void) {
    const struct pci_topology *t = pci_enum_topology();
    const struct pci_plan *plan = pci_plan();
    if (t == NULL || plan == NULL || virt.on) {
        return;
    }
    virt.on = true;
    uint32_t extended = cpuid(0x80000000, 0).eax;
    unsigned bits = extended >= 0x80000008 ? cpuid(0x80000008, 0).eax & 0xff : 36;
    virt.address_limit = 1ull << bits;

    virt.count = t->function_count;
    virt.functions = kmalloc((virt.count != 0 ? virt.count : 1) * sizeof(*virt.functions));
    for (uint32_t i = 0; i < virt.count; i++) {
        vfunction_init(&virt.functions[i], &t->functions[i], &plan->functions[i]);
    }
    uint32_t mapped = 0;
    for (uint32_t i = 0; i < virt.count; i++) {
        struct vfunction *vf = &virt.functions[i];
        for (unsigned k = 0; k <= BAR_COUNT; k++) {
            if (vf->bars[k].size != 0) {
                bar_update(&vf->bars[k], k == ROM_INDEX);
                mapped += vf->bars[k].mapped != 0;
            }
        }
    }

    struct io_handler ports = {CONFIG_ADDRESS, CONFIG_PORTS, config_ports, NULL};
    io_register(&ports);
    ecam_windows(plan);
    gpt_flush();
    printf("PCI: %u of the guest's BARs mapped onto the host's\n", mapped);
}
