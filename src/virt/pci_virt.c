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
#include <lib/printf.h>
#include <lib/spinlock.h>
#include <sys/mm.h>
#include <sys/x86.h>
#include <hw/lapic.h>
#include <virt/gpt.h>
#include <virt/guest_mem.h>
#include <virt/io.h>
#include <virt/irq.h>
#include <virt/layout.h>
#include <virt/mmio.h>
#include <virt/pci_plan.h>
#include <virt/pci_virt.h>
#include <virt/vlapic.h>

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

// PCI Local Bus Specification 3.0, 6.8.1: MSI's Message Control register and
// the registers after it, where the address is 64 bits wide.
#define MSI_CONTROL 2
#define MSI_ENABLE 0x0001u
#define MSI_MMC_SHIFT 1
#define MSI_MME_SHIFT 4
#define MSI_MME_MASK 0x0070u
#define MSI_WIDE 0x0080u
#define MSI_ADDRESS 4
#define MSI_UPPER 8
#define MSI_DATA_NARROW 8
#define MSI_DATA_WIDE 0xc

// PCI Local Bus Specification 3.0, 6.8.2: MSI-X's Message Control register,
// where its table is, and a table entry's fields.
#define MSIX_CONTROL 2
#define MSIX_SIZE_MASK 0x07ffu
#define MSIX_ENABLE 0x8000u
#define MSIX_TABLE 4
#define MSIX_BIR_MASK 7u
#define MSIX_ENTRY_SIZE 16
#define MSIX_ENTRY_ADDRESS 0
#define MSIX_ENTRY_UPPER 4
#define MSIX_ENTRY_DATA 8
#define MSIX_ENTRY_CONTROL 12
#define MSIX_ENTRY_MASKED 1u

#define REG_COMMAND 4
#define COMMAND_MEMORY 0x2u

// A memory BAR or expansion ROM BAR as the guest has it.
#define BAR_TRAPS 4

struct bar_trap {
    uint64_t offset;
    uint64_t size;
    int region;
};

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
    // The pages holding ranges of it that the guest reaches through the
    // hypervisor's regions, by offset: an MSI-X table's, and other modules'.
    struct bar_trap traps[BAR_TRAPS];
    unsigned trap_count;
};

// A function's MSI as the guest programs it. The function sends a message of
// the hypervisor's on the slot's vectors, which delivery turns back into the
// guest's.
struct vmsi {
    bool wide;
    // MSI Enable and Multiple Message Enable as the guest wrote them.
    uint16_t control;
    uint32_t address;
    uint32_t upper;
    uint32_t data;
    // The guest's address and data for the processor that takes one.
    uint64_t message;
    struct irq_slot slot;
};

// An MSI-X table entry as the guest programs it, and whether its slot and the
// hypervisor's message are live at the function, unmasked.
struct vmsix_entry {
    uint32_t address;
    uint32_t upper;
    uint32_t data;
    uint32_t control;
    uint64_t message;
    bool armed;
    struct irq_slot slot;
};

// A function's MSI-X. Its table's pages are the hypervisor's, which keeps
// the guest's entries and gives the function messages of its own for those
// the guest has unmasked while it has MSI-X enabled.
struct vmsix {
    bool enabled;
    unsigned count;
    // The table's host address, and that of the first page it is on.
    uint64_t table;
    uint64_t page;
    struct vmsix_entry *entries;
};

struct vfunction {
    struct pci_function *f;
    bool hidden;
    const struct pci_virt_hook *hook;
    struct vmsi msi;
    struct vmsix msix;
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

// A guest page where BARs smaller than a page share it, or sit at another
// offset than in their host pages, so that no one host page can back it: the
// hypervisor reaches each of them for the guest.
#define PAGE_RESIDENTS 16
#define SHARED_PAGES 64

struct resident {
    struct vfunction *vf;
    unsigned index;
};

struct shared_page {
    // 0 when unused.
    uint64_t page;
    int region;
    unsigned count;
    struct resident residents[PAGE_RESIDENTS];
};

static struct shared_page shared_pages[SHARED_PAGES];

static uint16_t requester(const struct pci_function *f) {
    return (uint16_t)(f->addr.bus << 8 | f->addr.device << 3 | f->addr.function);
}

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

static uint64_t bar_mask(const struct vbar *b) {
    return ~(b->size - 1);
}

// The guest's pages for the BAR at guest, which must sit at the same offset in
// its page as the host's BAR does in the host's, as whole pages map.
static bool mappable(const struct vbar *b, uint64_t guest) {
    uint64_t base = page_down(guest);
    uint64_t end = page_up(guest + b->size);
    return ((guest ^ b->host) & (PAGE_SIZE - 1)) == 0 && end > base && end <= virt.address_limit
           && layout_mmio_free(base, end - base);
}

static void map_pages(uint64_t guest, uint64_t host, uint64_t end) {
    if (end > guest) {
        gpt_map(guest, host, end - guest, GPT_RWX, MT_UC);
    }
}

static uint64_t trap_start(const struct vbar *b, uint64_t at, unsigned i) {
    return page_down(at + b->traps[i].offset);
}

static uint64_t trap_end(const struct vbar *b, uint64_t at, unsigned i) {
    return page_up(at + b->traps[i].offset + b->traps[i].size);
}

// Around the pages the guest reaches through the hypervisor, if any.
static void map(const struct vbar *b) {
    uint64_t base = page_down(b->mapped);
    uint64_t end = page_up(b->mapped + b->size);
    uint64_t host = page_down(b->host);
    uint64_t at = base;
    for (unsigned i = 0; i < b->trap_count; i++) {
        map_pages(at, host + (at - base), trap_start(b, b->mapped, i));
        if (trap_end(b, b->mapped, i) > at) {
            at = trap_end(b, b->mapped, i);
        }
    }
    map_pages(at, host + (at - base), end);
}

// Each of the hypervisor's regions to its pages of the BAR at at, or nowhere.
static void traps_move(const struct vbar *b, uint64_t at) {
    for (unsigned i = 0; i < b->trap_count; i++) {
        uint64_t start = trap_start(b, at, i);
        mmio_move(b->traps[i].region, at != 0 ? start : 0,
                  at != 0 ? trap_end(b, at, i) - start : 0);
    }
}

static bool overlaps(const struct vbar *b, uint64_t base, uint64_t end) {
    return b->mapped != 0 && page_down(b->mapped) < end && page_up(b->mapped + b->size) > base;
}

static bool sub_page(const struct vbar *b) {
    return b->size < PAGE_SIZE;
}

static bool page_arbitrate(uint64_t page);

// What else is mapped in [base, end), which an unmapping there took away.
static void map_again(uint64_t base, uint64_t end) {
    for (uint32_t i = 0; i < virt.count; i++) {
        for (unsigned k = 0; k <= BAR_COUNT; k++) {
            struct vbar *b = &virt.functions[i].bars[k];
            if (!overlaps(b, base, end)) {
                continue;
            }
            if (sub_page(b)) {
                page_arbitrate(page_down(b->mapped));
            } else {
                map(b);
            }
        }
    }
}

// The lower register's address bits all ones, as the guest sizes the BAR.
static bool sizing(const struct vbar *b) {
    return (uint32_t)b->guest == ((uint32_t)bar_mask(b) & BAR_ADDRESS);
}

static void bar_log(const struct vbar *b, const char *what) {
    for (uint32_t i = 0; i < virt.count; i++) {
        const struct vfunction *vf = &virt.functions[i];
        if (b < vf->bars || b > &vf->bars[BAR_COUNT]) {
            continue;
        }
        struct pci_addr a = vf->f->addr;
        unsigned k = (unsigned)(b - vf->bars);
        char name[8] = "ROM";
        if (k != ROM_INDEX) {
            snprintf(name, sizeof(name), "BAR%u", k);
        }
        printf("PCI %04x:%02x:%02x.%x %s at %#llx: %s\n", a.segment, a.bus, a.device, a.function,
               name, (unsigned long long)b->guest, what);
        return;
    }
}

// A BAR smaller than a page sits wherever the guest has nothing else, as the
// hypervisor reaches it where its page cannot be the host's.
static void sub_page_update(struct vbar *b, bool is_rom) {
    uint64_t want = 0;
    if (b->guest != 0 && (!is_rom || b->enabled) && !sizing(b)) {
        uint64_t page = page_down(b->guest);
        if (page + PAGE_SIZE <= virt.address_limit && layout_mmio_free(page, PAGE_SIZE)) {
            want = b->guest;
        } else {
            bar_log(b, "in no place, as the guest has something else there");
        }
    }
    if (want == b->mapped) {
        return;
    }
    uint64_t old = b->mapped;
    b->mapped = want;
    if (want == 0 && b->trap_count != 0) {
        mmio_move(b->traps[0].region, 0, 0);
    }
    bool reached = false;
    if (old != 0) {
        reached = page_arbitrate(page_down(old));
    }
    if (want != 0 && page_down(want) != page_down(old)) {
        reached = page_arbitrate(page_down(want));
    }
    if (want != 0 && reached) {
        bar_log(b, "reached through the hypervisor");
    }
    gpt_flush();
}

// Follows the guest's address and enables for the BAR. Where the guest puts it
// somewhere it cannot map, such as at the size mask while it sizes the BAR, it
// is in no place.
static void bar_update(struct vbar *b, bool is_rom) {
    if (sub_page(b)) {
        sub_page_update(b, is_rom);
        return;
    }
    uint64_t want = 0;
    if (b->guest != 0 && (!is_rom || b->enabled)) {
        if (mappable(b, b->guest)) {
            want = b->guest;
        } else if (!sizing(b)) {
            bar_log(b, "in no place, as the guest has something else there");
        }
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
    traps_move(b, want);
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

static uint32_t msi_data_reg(const struct vfunction *vf) {
    return vf->f->msi_cap + (vf->msi.wide ? MSI_DATA_WIDE : MSI_DATA_NARROW);
}

// Whether the guest has its own of the MSI register at reg.
static bool msi_reg(const struct vfunction *vf, uint32_t reg) {
    uint32_t cap = vf->f->msi_cap;
    return cap != 0
           && (reg == cap || reg == cap + MSI_ADDRESS || reg == msi_data_reg(vf)
               || (vf->msi.wide && reg == cap + MSI_UPPER));
}

static uint32_t msi_read(struct vfunction *vf, uint32_t reg) {
    uint32_t cap = vf->f->msi_cap;
    if (reg == cap) {
        uint32_t hardware = pci_cfg_read(vf->f->addr, reg, 4);
        return (hardware & ~((uint32_t)(MSI_ENABLE | MSI_MME_MASK) << 16))
               | (uint32_t)vf->msi.control << 16;
    }
    if (reg == cap + MSI_ADDRESS) {
        return vf->msi.address;
    }
    return reg == msi_data_reg(vf) ? vf->msi.data : vf->msi.upper;
}

// What a guest's message does where it goes: an interrupt, or a write of its
// data, as a function's message anywhere else is a memory write.
static void deliver_message(uint32_t upper, uint32_t address, uint32_t data) {
    if (upper == 0 && (address & MSI_ADDRESS_MASK) == MSI_ADDRESS_BASE) {
        vlapic_deliver_msi(address, data);
        return;
    }
    guest_phys_write((uint64_t)upper << 32 | address, &data, sizeof(data));
}

// A function sending a multiple message's index puts it in the data's low
// bits (PCI Local Bus Specification 3.0, 6.8.3.4).
static void msi_taken(void *ctx, unsigned index) {
    struct vfunction *vf = ctx;
    uint64_t message = __atomic_load_n(&vf->msi.message, __ATOMIC_ACQUIRE);
    uint32_t data = ((uint32_t)message & ~(vf->msi.slot.count - 1u)) | index;
    deliver_message(__atomic_load_n(&vf->msi.upper, __ATOMIC_RELAXED),
                    (uint32_t)(message >> 32), data);
}

// The function's MSI after the guest changed it: off at the function while its
// vectors move, then on with the hypervisor's message, its Multiple Message
// Enable cut to what it can send.
static void msi_update(struct vfunction *vf) {
    struct vmsi *m = &vf->msi;
    struct pci_addr a = vf->f->addr;
    uint32_t cap = vf->f->msi_cap;
    __atomic_store_n(&m->message, (uint64_t)m->address << 32 | (m->data & 0xffff),
                     __ATOMIC_RELEASE);
    uint16_t hardware = (uint16_t)pci_cfg_read(a, cap + MSI_CONTROL, 2);
    unsigned capable = (hardware >> MSI_MMC_SHIFT) & 7;
    unsigned enable = (m->control & MSI_MME_MASK) >> MSI_MME_SHIFT;
    if (enable > capable) {
        enable = capable;
    }
    uint16_t control = (uint16_t)((hardware & ~(MSI_ENABLE | MSI_MME_MASK)) | enable << MSI_MME_SHIFT);
    unsigned count = 1u << enable;
    if (!(m->control & MSI_ENABLE)) {
        pci_cfg_write(a, cap + MSI_CONTROL, 2, control);
        irq_slot_free(&m->slot);
        return;
    }
    if (m->slot.cpu == NULL || m->slot.count != count) {
        pci_cfg_write(a, cap + MSI_CONTROL, 2, control);
        if (!irq_slot_place(&m->slot, vlapic_msi_target(m->address), count)) {
            printf("PCI %04x:%02x:%02x.%x: no room for %u MSI vectors\n", a.segment, a.bus,
                   a.device, a.function, count);
            return;
        }
        uint32_t address;
        uint32_t data;
        irq_slot_message(&m->slot, &address, &data);
        pci_cfg_write(a, cap + MSI_ADDRESS, 4, address);
        if (m->wide) {
            pci_cfg_write(a, cap + MSI_UPPER, 4, 0);
        }
        pci_cfg_write(a, msi_data_reg(vf), 2, data);
    }
    pci_cfg_write(a, cap + MSI_CONTROL, 2, control | MSI_ENABLE);
}

static void msi_write(struct vfunction *vf, uint32_t reg, uint32_t value) {
    uint32_t cap = vf->f->msi_cap;
    if (reg == cap) {
        vf->msi.control = (uint16_t)(value >> 16) & (MSI_ENABLE | MSI_MME_MASK);
    } else if (reg == cap + MSI_ADDRESS) {
        vf->msi.address = value & ~3u;
    } else if (reg == msi_data_reg(vf)) {
        vf->msi.data = value & 0xffff;
    } else {
        __atomic_store_n(&vf->msi.upper, value, __ATOMIC_RELAXED);
    }
    msi_update(vf);
}

static void msix_taken(void *ctx, unsigned index) {
    (void)index;
    struct vmsix_entry *e = ctx;
    uint64_t message = __atomic_load_n(&e->message, __ATOMIC_ACQUIRE);
    deliver_message(__atomic_load_n(&e->upper, __ATOMIC_RELAXED), (uint32_t)(message >> 32),
                    (uint32_t)message);
}

// The function's entry after the guest changed it or MSI-X Enable: masked at
// the function while it gets its vector and the hypervisor's message.
static void msix_sync(struct vfunction *vf, unsigned i) {
    struct vmsix *x = &vf->msix;
    struct vmsix_entry *e = &x->entries[i];
    uint64_t at = x->table + (uint64_t)i * MSIX_ENTRY_SIZE;
    __atomic_store_n(&e->message, (uint64_t)e->address << 32 | e->data, __ATOMIC_RELEASE);
    uint32_t control = (uint32_t)mmio_read_host(at + MSIX_ENTRY_CONTROL, 4);
    if (!x->enabled || (e->control & MSIX_ENTRY_MASKED)) {
        if (e->armed) {
            mmio_write_host(at + MSIX_ENTRY_CONTROL, 4, control | MSIX_ENTRY_MASKED);
            e->armed = false;
        }
        irq_slot_free(&e->slot);
        return;
    }
    if (e->slot.cpu == NULL) {
        struct pci_addr a = vf->f->addr;
        if (!irq_slot_place(&e->slot, vlapic_msi_target(e->address), 1)) {
            printf("PCI %04x:%02x:%02x.%x: no room for MSI-X entry %u\n", a.segment, a.bus,
                   a.device, a.function, i);
            return;
        }
        uint32_t address;
        uint32_t data;
        irq_slot_message(&e->slot, &address, &data);
        mmio_write_host(at + MSIX_ENTRY_ADDRESS, 4, address);
        mmio_write_host(at + MSIX_ENTRY_UPPER, 4, 0);
        mmio_write_host(at + MSIX_ENTRY_DATA, 4, data);
    }
    if (!e->armed) {
        mmio_write_host(at + MSIX_ENTRY_CONTROL, 4, control & ~MSIX_ENTRY_MASKED);
        e->armed = true;
    }
}

// The table's pages: entries the guest's, anything else the function's.
static uint64_t msix_read(struct vcpu *v, void *ctx, uint64_t offset, unsigned size) {
    (void)v;
    struct vfunction *vf = ctx;
    struct vmsix *x = &vf->msix;
    uint64_t hpa = x->page + offset;
    uint64_t at = hpa - x->table;
    if (at >= (uint64_t)x->count * MSIX_ENTRY_SIZE) {
        return mmio_read_host(hpa, size);
    }
    spinlock_acquire(&virt.lock);
    const struct vmsix_entry *e = &x->entries[at / MSIX_ENTRY_SIZE];
    uint32_t words[4] = {e->address, e->upper, e->data, e->control};
    spinlock_release(&virt.lock);
    uint64_t value = 0;
    memcpy(&value, (const uint8_t *)words + at % MSIX_ENTRY_SIZE, size);
    return value;
}

static void msix_write(struct vcpu *v, void *ctx, uint64_t offset, unsigned size,
                       uint64_t value) {
    (void)v;
    struct vfunction *vf = ctx;
    struct vmsix *x = &vf->msix;
    uint64_t hpa = x->page + offset;
    uint64_t at = hpa - x->table;
    if (at >= (uint64_t)x->count * MSIX_ENTRY_SIZE) {
        mmio_write_host(hpa, size, value);
        return;
    }
    spinlock_acquire(&virt.lock);
    unsigned i = (unsigned)(at / MSIX_ENTRY_SIZE);
    struct vmsix_entry *e = &x->entries[i];
    uint32_t words[4] = {e->address, e->upper, e->data, e->control};
    memcpy((uint8_t *)words + at % MSIX_ENTRY_SIZE, &value, size);
    e->address = words[0] & ~3u;
    __atomic_store_n(&e->upper, words[1], __ATOMIC_RELAXED);
    e->data = words[2];
    e->control = words[3];
    msix_sync(vf, i);
    spinlock_release(&virt.lock);
}

static const struct mmio_ops msix_ops = {msix_read, msix_write, false};

struct shared_target {
    struct vfunction *vf;
    uint64_t hpa;
    bool msix;
};

// The BAR of the page an access falls in, if any, found under the lock, which
// msix_read() and msix_write() take themselves.
static bool shared_target(const struct shared_page *sp, uint64_t offset, unsigned size,
                          struct shared_target *t) {
    bool found = false;
    spinlock_acquire(&virt.lock);
    uint64_t gpa = sp->page + offset;
    for (unsigned i = 0; i < sp->count && !found; i++) {
        struct vfunction *vf = sp->residents[i].vf;
        const struct vbar *b = &vf->bars[sp->residents[i].index];
        if (gpa >= b->mapped && gpa + size <= b->mapped + b->size) {
            *t = (struct shared_target){vf, b->host + (gpa - b->mapped), b->trap_count != 0};
            found = true;
        }
    }
    spinlock_release(&virt.lock);
    return found;
}

static uint64_t shared_read(struct vcpu *v, void *ctx, uint64_t offset, unsigned size) {
    struct shared_target t;
    if (!shared_target(ctx, offset, size, &t)) {
        return size == 8 ? UINT64_MAX : all_ones(size);
    }
    if (t.msix) {
        return msix_read(v, t.vf, t.hpa - t.vf->msix.page, size);
    }
    return mmio_read_host(t.hpa, size);
}

static void shared_write(struct vcpu *v, void *ctx, uint64_t offset, unsigned size,
                         uint64_t value) {
    struct shared_target t;
    if (!shared_target(ctx, offset, size, &t)) {
        return;
    }
    if (t.msix) {
        msix_write(v, t.vf, t.hpa - t.vf->msix.page, size, value);
    } else {
        mmio_write_host(t.hpa, size, value);
    }
}

static const struct mmio_ops shared_ops = {shared_read, shared_write, false};

static struct shared_page *shared_find(uint64_t page) {
    for (unsigned i = 0; i < SHARED_PAGES; i++) {
        if (shared_pages[i].page == page) {
            return &shared_pages[i];
        }
    }
    return NULL;
}

static struct shared_page *shared_take(uint64_t page) {
    for (unsigned i = 0; i < SHARED_PAGES; i++) {
        struct shared_page *sp = &shared_pages[i];
        if (sp->page != 0) {
            continue;
        }
        if (sp->region < 0) {
            sp->region = mmio_register(page, PAGE_SIZE, &shared_ops, sp);
            if (sp->region < 0) {
                return NULL;
            }
        } else {
            mmio_move(sp->region, page, PAGE_SIZE);
        }
        sp->page = page;
        return sp;
    }
    return NULL;
}

// What the guest reaches in a page holding BARs smaller than a page: the host
// page where one BAR alone is there, at its host offset, else each of them
// through the hypervisor, which it returns whether it does. An MSI-X table's
// region stands in for a host page.
static bool page_arbitrate(uint64_t page) {
    struct resident found[PAGE_RESIDENTS];
    unsigned n = 0;
    for (uint32_t i = 0; i < virt.count; i++) {
        struct vfunction *vf = &virt.functions[i];
        for (unsigned k = 0; k <= BAR_COUNT && n < PAGE_RESIDENTS; k++) {
            const struct vbar *b = &vf->bars[k];
            if (b->size != 0 && sub_page(b) && b->mapped != 0 && page_down(b->mapped) == page) {
                found[n++] = (struct resident){vf, k};
            }
        }
    }
    struct shared_page *sp = shared_find(page);
    struct vbar *only = n == 1 ? &found[0].vf->bars[found[0].index] : NULL;
    bool direct = only != NULL && ((only->mapped ^ only->host) & (PAGE_SIZE - 1)) == 0;
    gpt_map(page, 0, PAGE_SIZE, 0, 0);
    if (n == 0 || direct) {
        if (sp != NULL) {
            mmio_move(sp->region, 0, 0);
            sp->page = 0;
        }
        if (direct && only->trap_count != 0) {
            mmio_move(only->traps[0].region, page, PAGE_SIZE);
        } else if (direct) {
            gpt_map(page, page_down(only->host), PAGE_SIZE, GPT_RWX, MT_UC);
        }
        return false;
    }

    for (unsigned i = 0; i < n; i++) {
        struct vbar *b = &found[i].vf->bars[found[i].index];
        if (b->trap_count != 0) {
            mmio_move(b->traps[0].region, 0, 0);
        }
    }
    if (sp == NULL) {
        sp = shared_take(page);
    }
    if (sp == NULL) {
        printf("PCI: no room to reach the BARs at %#llx\n", (unsigned long long)page);
        return false;
    }
    memcpy(sp->residents, found, n * sizeof(found[0]));
    sp->count = n;
    return true;
}

// MSI-X Enable as the guest sets it: the entries it unmasked get their
// vectors before the function may send, and lose them after it may not.
static void msix_control(struct vfunction *vf, uint32_t value) {
    struct vmsix *x = &vf->msix;
    struct pci_addr a = vf->f->addr;
    uint32_t reg = vf->f->msix_cap;
    bool enable = (value >> 16 & MSIX_ENABLE) != 0;
    if (enable == x->enabled) {
        pci_cfg_write(a, reg, 4, value);
        return;
    }
    x->enabled = enable;
    if (!enable) {
        pci_cfg_write(a, reg, 4, value);
    }
    for (unsigned i = 0; i < x->count; i++) {
        msix_sync(vf, i);
    }
    if (enable) {
        pci_cfg_write(a, reg, 4, value);
    }
}

// Disabled, as after a reset, every entry masked at the function, the guest's
// entries what the function held. Memory decoding is on meanwhile.
static void msix_init(struct vfunction *vf) {
    struct pci_function *f = vf->f;
    struct vmsix *x = &vf->msix;
    uint32_t control = pci_cfg_read(f->addr, f->msix_cap + MSIX_CONTROL, 2);
    uint32_t table = pci_cfg_read(f->addr, f->msix_cap + MSIX_TABLE, 4);
    unsigned bir = table & MSIX_BIR_MASK;
    struct vbar *b = bir < BAR_COUNT ? &vf->bars[bir] : NULL;
    x->count = (control & MSIX_SIZE_MASK) + 1;
    uint64_t offset = table & ~MSIX_BIR_MASK;
    if (b == NULL || b->size == 0 || offset + (uint64_t)x->count * MSIX_ENTRY_SIZE > b->size) {
        x->count = 0;
        return;
    }
    // Without a region of its own, the guest has the table as it is.
    int region = mmio_register(0, 0, &msix_ops, vf);
    if (region < 0) {
        x->count = 0;
        return;
    }
    pci_cfg_write(f->addr, f->msix_cap + MSIX_CONTROL, 2, control & ~MSIX_ENABLE);
    x->table = b->host + offset;
    x->page = page_down(x->table);
    mm_map_device(x->page, page_up(x->table + (uint64_t)x->count * MSIX_ENTRY_SIZE) - x->page);
    x->entries = kmalloc(x->count * sizeof(*x->entries));
    uint32_t command = pci_cfg_read(f->addr, REG_COMMAND, 2);
    pci_cfg_write(f->addr, REG_COMMAND, 2, command | COMMAND_MEMORY);
    for (unsigned i = 0; i < x->count; i++) {
        struct vmsix_entry *e = &x->entries[i];
        uint64_t at = x->table + (uint64_t)i * MSIX_ENTRY_SIZE;
        e->address = (uint32_t)mmio_read_host(at + MSIX_ENTRY_ADDRESS, 4);
        e->upper = (uint32_t)mmio_read_host(at + MSIX_ENTRY_UPPER, 4);
        e->data = (uint32_t)mmio_read_host(at + MSIX_ENTRY_DATA, 4);
        e->control = (uint32_t)mmio_read_host(at + MSIX_ENTRY_CONTROL, 4);
        mmio_write_host(at + MSIX_ENTRY_CONTROL, 4, e->control | MSIX_ENTRY_MASKED);
        e->slot = (struct irq_slot){.taken = msix_taken, .ctx = e, .requester = requester(f)};
    }
    pci_cfg_write(f->addr, REG_COMMAND, 2, command);
    b->traps[0] = (struct bar_trap){offset, (uint64_t)x->count * MSIX_ENTRY_SIZE, region};
    b->trap_count = 1;
}

static void msi_init(struct vfunction *vf) {
    struct vmsi *m = &vf->msi;
    struct pci_addr a = vf->f->addr;
    uint32_t cap = vf->f->msi_cap;
    uint16_t control = (uint16_t)pci_cfg_read(a, cap + MSI_CONTROL, 2);
    m->wide = (control & MSI_WIDE) != 0;
    // Disabled, as after a reset: the guest enables what it uses.
    m->control = control & MSI_MME_MASK;
    pci_cfg_write(a, cap + MSI_CONTROL, 2, control & ~MSI_ENABLE);
    m->address = pci_cfg_read(a, cap + MSI_ADDRESS, 4);
    m->upper = m->wide ? pci_cfg_read(a, cap + MSI_UPPER, 4) : 0;
    m->data = pci_cfg_read(a, msi_data_reg(vf), 2);
    m->slot = (struct irq_slot){.taken = msi_taken, .ctx = vf, .requester = requester(vf->f)};
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
    uint32_t value;
    if (vf != NULL && vf->hook != NULL && vf->hook->read(reg, &value)) {
        return (value >> (8 * (offset & 3))) & all_ones(size);
    }
    if (vf != NULL && msi_reg(vf, reg)) {
        return (msi_read(vf, reg) >> (8 * (offset & 3))) & all_ones(size);
    }
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
    unsigned shift = 8 * (offset & 3);
    uint32_t mask = all_ones(size) << shift;
    if (vf != NULL && vf->hook != NULL && vf->hook->write(reg, value << shift, mask)) {
        return;
    }
    if (vf != NULL && msi_reg(vf, reg)) {
        msi_write(vf, reg, (msi_read(vf, reg) & ~mask) | ((value << shift) & mask));
        return;
    }
    if (vf != NULL && vf->msix.count != 0 && reg == vf->f->msix_cap) {
        msix_control(vf, (pci_cfg_read(a, reg, 4) & ~mask) | ((value << shift) & mask));
        return;
    }
    if (vf == NULL || offset >= REG_HEADER_END || !is_kept(vf, reg)) {
        pci_cfg_write(a, offset, size, value);
        return;
    }
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
    if (f->msi_cap != 0) {
        msi_init(vf);
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
        // The hypervisor reaches it for the guest where no one page can.
        if (bar->size < PAGE_SIZE) {
            mm_map_device(page_down(bar->base), PAGE_SIZE);
        }
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
        if (f->rom.size < PAGE_SIZE) {
            mm_map_device(page_down(f->rom.base), PAGE_SIZE);
        }
    }
    if (f->msix_cap != 0) {
        msix_init(vf);
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

    for (unsigned i = 0; i < SHARED_PAGES; i++) {
        shared_pages[i].region = -1;
    }
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

void pci_virt_hook(struct pci_addr addr, const struct pci_virt_hook *hook) {
    struct vfunction *vf = vfunction_find(addr);
    if (vf != NULL) {
        vf->hook = hook;
    }
}

bool pci_virt_trap(struct pci_addr addr, unsigned bar, uint64_t offset, uint64_t size,
                   const struct mmio_ops *ops, void *ctx) {
    struct vfunction *vf = vfunction_find(addr);
    if (vf == NULL || bar >= BAR_COUNT) {
        return false;
    }
    struct vbar *b = &vf->bars[bar];
    if (b->size == 0 || sub_page(b) || b->trap_count == BAR_TRAPS || size == 0 || offset > b->size
        || size > b->size - offset) {
        return false;
    }
    // Pages of their own, in order.
    unsigned at = 0;
    for (; at < b->trap_count; at++) {
        if (page_up(b->traps[at].offset + b->traps[at].size) > page_down(offset)
            && b->traps[at].offset < page_up(offset + size)) {
            return false;
        }
        if (b->traps[at].offset > offset) {
            break;
        }
    }
    int region = mmio_register(0, 0, ops, ctx);
    if (region < 0) {
        return false;
    }
    spinlock_acquire(&virt.lock);
    for (unsigned i = b->trap_count; i > at; i--) {
        b->traps[i] = b->traps[i - 1];
    }
    b->traps[at] = (struct bar_trap){offset, size, region};
    b->trap_count++;
    if (b->mapped != 0) {
        uint64_t start = trap_start(b, b->mapped, at);
        uint64_t end = trap_end(b, b->mapped, at);
        gpt_map(start, 0, end - start, 0, 0);
        mmio_move(region, start, end - start);
        gpt_flush();
    }
    spinlock_release(&virt.lock);
    return true;
}
