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
#include <virt/pci_virt.h>
#include <hw/lapic.h>
#include <hw/pci_cfg.h>
#include <hw/pci_enum.h>
#include <hw/pci_regs.h>
#include <hw/power.h>
#include <iommu/remap.h>
#include <lib/heap.h>
#include <lib/log.h>
#include <lib/printf.h>
#include <lib/spinlock.h>
#include <lib/util.h>
#include <memory.h>
#include <sys/mm.h>
#include <sys/x86.h>
#include <virt/gpt.h>
#include <virt/guest_mem.h>
#include <virt/io.h>
#include <virt/irq.h>
#include <virt/layout.h>
#include <virt/mmio.h>
#include <virt/pci_plan.h>
#include <virt/pci_port.h>
#include <virt/vcpu.h>
#include <virt/vga.h>
#include <virt/vlapic.h>

#define CONFIG_PORTS 8

#define FUNCTION_SIZE ((uint32_t)0x1000)
#define BUS_SIZE ((uint64_t)0x100000)

// PCI Local Bus Specification 3.0, 6.1 and 6.2.5, and PCI-to-PCI Bridge
// Architecture Specification 1.1, 3.2.
#define BAR_ADDRESS ((uint32_t)0xfffffff0)
#define WINDOW_ADDRESS ((uint32_t)0xfff0)
// The latency timer of a bridge's bus number register is the hardware's.
#define BUS_NUMBERS_VIRTUAL ((uint32_t)0x00ffffff)

// How many resets of one function the log tells of.
#define RESETS_LOGGED 4

// PCI Express Base Specification 6.0, 7.8.6: the capability header, then a
// capability and a control register for each of up to six BARs.
#define REBAR_SIZE ((uint32_t)0x34)

#define BAR_COUNT 6
#define ROM_INDEX 6

// PCI Local Bus Specification 3.0, 6.8.1: MSI's Message Control register and
// the registers after it, where the address is 64 bits wide.
#define MSI_MMC_SHIFT 1
#define MSI_MME_SHIFT 4

// A memory BAR or expansion ROM BAR as the guest has it.
#define BAR_TRAPS 4

struct bar_trap {
    size_t offset;
    size_t size;
    int region;
};

struct vbar {
    uint8_t kind;
    // The read-only type bits of its (lower) register.
    uint8_t type_bits;
    size_t size;
    uint64_t host;
    uint64_t guest;
    bool enabled;
    // Where the guest's range maps to the host's, 0 if nowhere.
    uint64_t mapped;
    // The pages holding ranges of it that the guest reaches through the
    // hypervisor's regions, by offset: an MSI-X table's, and other modules'.
    struct bar_trap traps[BAR_TRAPS];
    size_t trap_count;
    // Those of one smaller than a page reach the page's other residents too,
    // so it has only this, for the whole of it.
    const struct mmio_ops *sub_page_ops;
    void *sub_page_ctx;
};

// A function's MSI as the guest programs it. The function sends a message of
// the hypervisor's on the slot's vectors, which delivery turns back into the
// guest's.
struct vmsi {
    bool wide;
    // MSI Enable and Multiple Message Enable as the guest wrote them.
    uint16_t control;
    uint32_t address;
    _Atomic(uint32_t) upper;
    uint32_t data;
    // The guest's address and data for the processor that takes one.
    _Atomic(uint64_t) message;
    struct irq_slot slot;
};

// An MSI-X table entry as the guest programs it, and whether its slot and the
// hypervisor's message are live at the function, unmasked.
struct vmsix_entry {
    uint32_t address;
    _Atomic(uint32_t) upper;
    uint32_t data;
    uint32_t control;
    _Atomic(uint64_t) message;
    bool armed;
    struct irq_slot slot;
};

// A function's MSI-X. Its table's pages are the hypervisor's, which keeps the
// guest's entries and gives the function messages of its own for those the
// guest has unmasked while it has MSI-X enabled.
struct vmsix {
    bool enabled;
    size_t count;
    // The table's host address, and that of the first page it is on.
    uintptr_t table;
    uintptr_t page;
    struct vmsix_entry *entries;
};

struct vfunction {
    struct pci_enum_function *f;
    bool hidden;
    const struct pci_virt_hook *hook;
    void *hook_ctx;
    struct vmsi msi;
    struct vmsix msix;
    // A 64-bit BAR at the index of its lower register; a ROM at ROM_INDEX.
    struct vbar bars[BAR_COUNT + 1];
    uint32_t bus_numbers;
    uintptr_t mem_base;
    uintptr_t mem_limit;
    uintptr_t pref_base;
    uintptr_t pref_limit;
    bool pref_64;
    // Where its I/O BARs below 1000h are, whose ports the guest is given: 0
    // for the others.
    uint16_t legacy_io[BAR_COUNT];
    // A bridge's bus numbers and memory window registers as the host has them,
    // which the guest's writes do not reach.
    uint32_t host_buses;
    uint32_t host_windows[4];
    // Whether it is a bridge above the device that owns the legacy VGA
    // resources, and its VGA Enable bit as the guest has it while the VGA is
    // emulated.
    bool vga_path;
    bool vga_enable;
    // Whether its MSI-X entries go back to the function once it decodes memory
    // again, after a reset took them away.
    bool msix_rearm;
    size_t resets_logged;
};

static struct {
    bool on;
    // The guest's configuration address, which never reaches the hardware: the
    // hypervisor makes every data access itself.
    uint32_t config_address;
    struct spinlock lock;
    struct vfunction *functions;
    size_t count;
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
    size_t index;
};

struct shared_page {
    // 0 when unused.
    uint64_t page;
    int region;
    size_t count;
    struct resident residents[PAGE_RESIDENTS];
};

static struct shared_page shared_pages[SHARED_PAGES];

static uint16_t requester(const struct pci_enum_function *f) {
    return (uint16_t)(f->addr.bus << 8 | f->addr.device << 3 | f->addr.function);
}

static uint32_t all_ones(size_t size) {
    return size == 4 ? (uint32_t)0xffffffff : ((uint32_t)1 << (8 * size)) - 1;
}

// In the topology's order.
static uint32_t addr_key(struct pci_cfg_addr a) {
    return (uint32_t)a.segment << 16 | (uint32_t)a.bus << 8 | (uint32_t)a.device << 3 | a.function;
}

static struct vfunction *vfunction_find(struct pci_cfg_addr a) {
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

static uint64_t bar_mask(const struct vbar *b) {
    return ~(b->size - 1);
}

// The guest's pages for the BAR at guest, which must sit at the same offset in
// its page as the host's BAR does in the host's, as whole pages map.
static bool mappable(const struct vbar *b, uint64_t guest) {
    uintptr_t base = page_down(guest);
    uintptr_t end = page_up(guest + b->size);
    return ((guest ^ b->host) & (PAGE_SIZE - 1)) == 0 && end > base && end <= virt.address_limit
        && layout_mmio_free(base, end - base);
}

static void map_pages(uintptr_t guest, uintptr_t host, uintptr_t end) {
    if (end > guest) {
        gpt_map(guest, host, end - guest, GPT_RWX, MT_UC);
    }
}

static uintptr_t trap_start(const struct vbar *b, uint64_t at, size_t i) {
    return page_down(at + b->traps[i].offset);
}

static uintptr_t trap_end(const struct vbar *b, uint64_t at, size_t i) {
    return page_up(at + b->traps[i].offset + b->traps[i].size);
}

// Around the pages the guest reaches through the hypervisor, if any.
static void map(const struct vbar *b) {
    uintptr_t base = page_down(b->mapped);
    uintptr_t end = page_up(b->mapped + b->size);
    uint64_t host = page_down(b->host);
    uintptr_t at = base;
    for (size_t i = 0; i < b->trap_count; i++) {
        map_pages(at, host + (at - base), trap_start(b, b->mapped, i));
        if (trap_end(b, b->mapped, i) > at) {
            at = trap_end(b, b->mapped, i);
        }
    }
    map_pages(at, host + (at - base), end);
}

// Each of the hypervisor's regions to its pages of the BAR at at, or nowhere.
static void traps_move(const struct vbar *b, uint64_t at) {
    for (size_t i = 0; i < b->trap_count; i++) {
        uintptr_t start = trap_start(b, at, i);
        mmio_move(
            b->traps[i].region, at != 0 ? start : 0, at != 0 ? trap_end(b, at, i) - start : 0
        );
    }
}

static bool overlaps(const struct vbar *b, uintptr_t base, uintptr_t end) {
    return b->mapped != 0 && page_down(b->mapped) < end && page_up(b->mapped + b->size) > base;
}

static bool sub_page(const struct vbar *b) {
    return b->size < PAGE_SIZE;
}

static bool page_arbitrate(uintptr_t page);

// What else is mapped in [base, end), which an unmapping there took away.
static void map_again(uintptr_t base, uintptr_t end) {
    for (size_t i = 0; i < virt.count; i++) {
        for (size_t k = 0; k <= BAR_COUNT; k++) {
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
    for (size_t i = 0; i < virt.count; i++) {
        const struct vfunction *vf = &virt.functions[i];
        if (b < vf->bars || b > &vf->bars[BAR_COUNT]) {
            continue;
        }
        struct pci_cfg_addr a = vf->f->addr;
        size_t k = (unsigned int)(b - vf->bars);
        char name[8] = "ROM";
        if (k != ROM_INDEX) {
            snprintf(name, sizeof(name), "BAR%zu", k);
        }
        printf(
            "PCI " PCI_CFG_ADDR_FMT " %s at %#llx: %s\n", PCI_CFG_ADDR_ARGS(a), name,
            (unsigned long long)b->guest, what
        );
        return;
    }
}

// A BAR smaller than a page sits wherever the guest has nothing else, as the
// hypervisor reaches it where its page cannot be the host's.
static void sub_page_update(struct vbar *b, bool is_rom) {
    uint64_t want = 0;
    if (b->guest != 0 && (!is_rom || b->enabled) && !sizing(b)) {
        uintptr_t page = page_down(b->guest);
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
        uintptr_t base = page_down(b->mapped);
        uintptr_t end = page_up(b->mapped + b->size);
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
    unsigned int bars = vf->f->header_type == PCI_HEADER_TYPE_BRIDGE ? 2 : BAR_COUNT;
    uint32_t rom = vf->f->header_type == PCI_HEADER_TYPE_BRIDGE
                 ? PCI_ROM_ADDRESS1
                 : PCI_ROM_ADDRESS;
    *upper = false;
    if (reg == rom) {
        return vf->bars[ROM_INDEX].kind == PCI_ENUM_BAR_MEM32 ? &vf->bars[ROM_INDEX] : NULL;
    }
    if (reg < PCI_BASE_ADDRESS_0 || reg >= PCI_BASE_ADDRESS_0 + 4 * bars) {
        return NULL;
    }
    size_t i = (reg - PCI_BASE_ADDRESS_0) / 4;
    struct vbar *b = &vf->bars[i];
    if (b->kind == PCI_ENUM_BAR_MEM32 || b->kind == PCI_ENUM_BAR_MEM64) {
        return b;
    }
    if (i > 0 && vf->bars[i - 1].kind == PCI_ENUM_BAR_MEM64) {
        *upper = true;
        return &vf->bars[i - 1];
    }
    return NULL;
}

static bool is_bridge(const struct vfunction *vf) {
    return vf->f->header_type == PCI_HEADER_TYPE_BRIDGE;
}

static bool window_reg(const struct vfunction *vf, uint32_t reg) {
    return is_bridge(vf)
        && (reg == PCI_MEMORY_BASE || reg == PCI_PREF_MEMORY_BASE || reg == PCI_PREF_BASE_UPPER32
         || reg == PCI_PREF_LIMIT_UPPER32);
}

static uint32_t window_word(uintptr_t address) {
    return (uint32_t)(address >> 16) & WINDOW_ADDRESS;
}

// The dword at reg of a register the hypervisor keeps.
static uint32_t kept_read(struct vfunction *vf, uint32_t reg) {
    bool upper;
    struct vbar *b = bar_at(vf, reg, &upper);
    if (b != NULL) {
        if (b == &vf->bars[ROM_INDEX]) {
            return ((uint32_t)b->guest & PCI_ROM_ADDRESS_MASK & (uint32_t)bar_mask(b))
                 | (b->enabled ? PCI_ROM_ADDRESS_ENABLE : 0);
        }
        if (upper) {
            return (uint32_t)((b->guest & bar_mask(b)) >> 32);
        }
        return ((uint32_t)b->guest & BAR_ADDRESS & (uint32_t)bar_mask(b)) | b->type_bits;
    }
    switch (reg) {
        case PCI_PRIMARY_BUS: {
            return (vf->bus_numbers & BUS_NUMBERS_VIRTUAL)
                 | (pci_cfg_read(vf->f->addr, reg, 4) & ~BUS_NUMBERS_VIRTUAL);
        }
        case PCI_MEMORY_BASE: {
            return window_word(vf->mem_base) | window_word(vf->mem_limit) << 16;
        }
        case PCI_PREF_MEMORY_BASE: {
            uint32_t width = vf->pref_64 ? PCI_PREF_RANGE_TYPE_64 : 0;
            return (window_word(vf->pref_base) | width)
                 | (window_word(vf->pref_limit) | width) << 16;
        }
        case PCI_PREF_BASE_UPPER32: {
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
            b->guest = value & PCI_ROM_ADDRESS_MASK & (uint32_t)bar_mask(b);
            b->enabled = (value & PCI_ROM_ADDRESS_ENABLE) != 0;
            // The function decodes its ROM where the host has it.
            uint32_t enable = b->enabled ? PCI_ROM_ADDRESS_ENABLE : 0;
            pci_cfg_write(
                vf->f->addr, reg, 4, ((uint32_t)b->host & PCI_ROM_ADDRESS_MASK) | enable
            );
            bar_update(b, true);
        } else if (upper) {
            b->guest = (b->guest & (uint64_t)0xffffffff) | ((uint64_t)value << 32 & bar_mask(b));
            bar_update(b, false);
        } else {
            b->guest = (b->guest & ~(uint64_t)0xffffffff) | (value & BAR_ADDRESS & bar_mask(b));
            bar_update(b, false);
        }
        return;
    }
    switch (reg) {
        case PCI_PRIMARY_BUS: {
            pci_cfg_write(vf->f->addr, reg + 3, 1, value >> 24);
            break;
        }
        case PCI_MEMORY_BASE: {
            vf->mem_base = (uint64_t)(value & WINDOW_ADDRESS) << 16;
            vf->mem_limit = (uint64_t)(value >> 16 & WINDOW_ADDRESS) << 16
                          | (PCI_MEMORY_GRANULE - 1);
            break;
        }
        case PCI_PREF_MEMORY_BASE: {
            vf->pref_base = (vf->pref_base & ~(uint64_t)0xffffffff)
                          | (uint64_t)(value & WINDOW_ADDRESS) << 16;
            vf->pref_limit = (vf->pref_limit & ~(uint64_t)0xffffffff)
                           | (uint64_t)(value >> 16 & WINDOW_ADDRESS) << 16
                           | (PCI_MEMORY_GRANULE - 1);
            break;
        }
        case PCI_PREF_BASE_UPPER32: {
            if (vf->pref_64) {
                vf->pref_base = (vf->pref_base & (uint64_t)0xffffffff) | (uint64_t)value << 32;
            }
            break;
        }
        default: {
            if (vf->pref_64) {
                vf->pref_limit = (vf->pref_limit & (uint64_t)0xffffffff) | (uint64_t)value << 32;
            }
            break;
        }
    }
}

static bool is_kept(struct vfunction *vf, uint32_t reg) {
    bool upper;
    return bar_at(vf, reg, &upper) != NULL || window_reg(vf, reg)
        || (is_bridge(vf) && reg == PCI_PRIMARY_BUS);
}

// The VGA Enable bit in the value of an access that reaches it on a bridge
// above the emulated VGA's device, else 0. The guest has its own bit there,
// set as firmware sets it on the bridges above the VGA it starts: Windows 7
// does not start a VGA device behind a bridge without it, and Linux 7.3's VGA
// arbiter gives such a device no legacy resources. The hardware's bit stays as
// the firmware left it, as the hypervisor answers the legacy ranges itself.
static uint32_t vga_enable_bit(const struct vfunction *vf, uint32_t offset, size_t size) {
    if (vf == NULL || !vf->vga_path || !vga_emulated() || offset > PCI_BRIDGE_CONTROL
     || offset + size <= PCI_BRIDGE_CONTROL) {
        return 0;
    }
    return (uint32_t)PCI_BRIDGE_CTL_VGA << (8 * (PCI_BRIDGE_CONTROL - offset));
}

static uint32_t msi_data_reg(const struct vfunction *vf) {
    return vf->f->msi_cap + (vf->msi.wide ? PCI_MSI_DATA_64 : PCI_MSI_DATA_32);
}

// Whether the guest has its own of the MSI register at reg.
static bool msi_reg(const struct vfunction *vf, uint32_t reg) {
    uint32_t cap = vf->f->msi_cap;
    return cap != 0
        && (reg == cap || reg == cap + PCI_MSI_ADDRESS_LO || reg == msi_data_reg(vf)
         || (vf->msi.wide && reg == cap + PCI_MSI_ADDRESS_HI));
}

static uint32_t msi_read(struct vfunction *vf, uint32_t reg) {
    uint32_t cap = vf->f->msi_cap;
    if (reg == cap) {
        uint32_t hardware = pci_cfg_read(vf->f->addr, reg, 4);
        return (hardware & ~((uint32_t)(PCI_MSI_FLAGS_ENABLE | PCI_MSI_FLAGS_QSIZE) << 16))
             | (uint32_t)vf->msi.control << 16;
    }
    if (reg == cap + PCI_MSI_ADDRESS_LO) {
        return vf->msi.address;
    }
    return reg == msi_data_reg(vf)
         ? vf->msi.data
         : atomic_load_explicit(&vf->msi.upper, memory_order_relaxed);
}

// What a guest's message does where it goes: an interrupt, or a write of its
// data, as a function's message anywhere else is a memory write.
static void deliver_message(uint32_t upper, uint32_t address, uint32_t data) {
    if (upper == 0 && (address & MSI_ADDRESS_MASK) == MSI_ADDRESS_BASE) {
        vlapic_deliver_msi(address, data);
        return;
    }
    guest_mem_phys_write((uint64_t)upper << 32 | address, &data, sizeof(uint32_t));
}

// A function sending a multiple message's index puts it in the data's low bits
// (PCI Local Bus Specification 3.0, 6.8.3.4).
static void msi_taken(void *ctx, size_t index) {
    struct vfunction *vf = ctx;
    uint64_t message = atomic_load_explicit(&vf->msi.message, memory_order_acquire);
    uint32_t data = ((uint32_t)message & ~(vf->msi.slot.count - (uint32_t)1)) | (uint32_t)index;
    deliver_message(
        atomic_load_explicit(&vf->msi.upper, memory_order_relaxed), (uint32_t)(message >> 32), data
    );
}

// Where the IOMMUs send a guest's message that a slot backs: posted to the one
// vCPU it reaches where they can do what delivering it does, else to the slot,
// whose taking delivers it.
static void message_route(struct irq_slot *s, uint32_t upper, uint32_t address, uint32_t data) {
    if (!s->remapped || !remap_posting()) {
        return;
    }
    struct remap_target target;
    bool post = upper == 0 && vlapic_post_target(address, data, s->count, &target);
    irq_slot_route(s, post ? &target : NULL, (uint8_t)(data & ~(uint32_t)(s->count - 1)));
}

// The function's MSI after the guest changed it: off at the function while its
// vectors move, then on with the hypervisor's message, its Multiple Message
// Enable cut to what it can send.
static void msi_update(struct vfunction *vf) {
    struct vmsi *m = &vf->msi;
    struct pci_cfg_addr a = vf->f->addr;
    uint32_t cap = vf->f->msi_cap;
    atomic_store_explicit(
        &m->message, (uint64_t)m->address << 32 | (m->data & 0xffff), memory_order_release
    );
    uint16_t hardware = (uint16_t)pci_cfg_read(a, cap + PCI_MSI_FLAGS, 2);
    unsigned int capable = (hardware >> MSI_MMC_SHIFT) & 7;
    unsigned int enable = (m->control & PCI_MSI_FLAGS_QSIZE) >> MSI_MME_SHIFT;
    if (enable > capable) {
        enable = capable;
    }
    uint16_t control = (uint16_t)((hardware & ~(PCI_MSI_FLAGS_ENABLE | PCI_MSI_FLAGS_QSIZE))
                                | enable << MSI_MME_SHIFT);
    size_t count = (uint32_t)1 << enable;
    if ((m->control & PCI_MSI_FLAGS_ENABLE) == 0) {
        pci_cfg_write(a, cap + PCI_MSI_FLAGS, 2, control);
        irq_slot_free(&m->slot);
        return;
    }
    if (m->slot.cpu == NULL || m->slot.count != count) {
        pci_cfg_write(a, cap + PCI_MSI_FLAGS, 2, control);
        if (!irq_slot_place(&m->slot, vlapic_msi_target(m->address), count)) {
            printf(
                "PCI " PCI_CFG_ADDR_FMT ": no room for %zu MSI vectors\n", PCI_CFG_ADDR_ARGS(a),
                count
            );
            return;
        }
        uint32_t address;
        uint32_t data;
        irq_slot_message(&m->slot, &address, &data);
        pci_cfg_write(a, cap + PCI_MSI_ADDRESS_LO, 4, address);
        if (m->wide) {
            pci_cfg_write(a, cap + PCI_MSI_ADDRESS_HI, 4, 0);
        }
        pci_cfg_write(a, msi_data_reg(vf), 2, data);
    }
    uint32_t upper = atomic_load_explicit(&m->upper, memory_order_relaxed);
    message_route(&m->slot, upper, m->address, m->data);
    pci_cfg_write(a, cap + PCI_MSI_FLAGS, 2, control | PCI_MSI_FLAGS_ENABLE);
}

static void msi_write(struct vfunction *vf, uint32_t reg, uint32_t value) {
    uint32_t cap = vf->f->msi_cap;
    if (reg == cap) {
        vf->msi.control = (uint16_t)(value >> 16) & (PCI_MSI_FLAGS_ENABLE | PCI_MSI_FLAGS_QSIZE);
    } else if (reg == cap + PCI_MSI_ADDRESS_LO) {
        vf->msi.address = value & ~(uint32_t)3;
    } else if (reg == msi_data_reg(vf)) {
        vf->msi.data = value & 0xffff;
    } else {
        atomic_store_explicit(&vf->msi.upper, value, memory_order_relaxed);
    }
    msi_update(vf);
}

static void msix_taken(void *ctx, size_t index) {
    (void)index;
    struct vmsix_entry *e = ctx;
    uint64_t message = atomic_load_explicit(&e->message, memory_order_acquire);
    deliver_message(
        atomic_load_explicit(&e->upper, memory_order_relaxed), (uint32_t)(message >> 32),
        (uint32_t)message
    );
}

// The function's entry after the guest changed it or MSI-X Enable: masked at
// the function while it gets its vector and the hypervisor's message.
static void msix_sync(struct vfunction *vf, size_t i) {
    struct vmsix *x = &vf->msix;
    struct vmsix_entry *e = &x->entries[i];
    uint64_t at = x->table + (uint64_t)i * PCI_MSIX_ENTRY_SIZE;
    atomic_store_explicit(&e->message, (uint64_t)e->address << 32 | e->data, memory_order_release);
    uint32_t control = (uint32_t)mmio_read_host(at + PCI_MSIX_ENTRY_VECTOR_CTRL, 4);
    if (!x->enabled || (e->control & PCI_MSIX_ENTRY_CTRL_MASKBIT) != 0) {
        if (e->armed) {
            mmio_write_host(
                at + PCI_MSIX_ENTRY_VECTOR_CTRL, 4, control | PCI_MSIX_ENTRY_CTRL_MASKBIT
            );
            e->armed = false;
        }
        irq_slot_free(&e->slot);
        return;
    }
    if (e->slot.cpu == NULL) {
        struct pci_cfg_addr a = vf->f->addr;
        if (!irq_slot_place(&e->slot, vlapic_msi_target(e->address), 1)) {
            printf(
                "PCI " PCI_CFG_ADDR_FMT ": no room for MSI-X entry %zu\n", PCI_CFG_ADDR_ARGS(a), i
            );
            return;
        }
        uint32_t address;
        uint32_t data;
        irq_slot_message(&e->slot, &address, &data);
        mmio_write_host(at + PCI_MSIX_ENTRY_LOWER_ADDR, 4, address);
        mmio_write_host(at + PCI_MSIX_ENTRY_UPPER_ADDR, 4, 0);
        mmio_write_host(at + PCI_MSIX_ENTRY_DATA, 4, data);
    }
    uint32_t upper = atomic_load_explicit(&e->upper, memory_order_relaxed);
    message_route(&e->slot, upper, e->address, e->data);
    if (!e->armed) {
        mmio_write_host(
            at + PCI_MSIX_ENTRY_VECTOR_CTRL, 4, control & ~PCI_MSIX_ENTRY_CTRL_MASKBIT
        );
        e->armed = true;
    }
}

// The table's pages: entries the guest's, anything else the function's.
static uint64_t msix_read(struct vcpu *v, void *ctx, uint64_t offset, size_t size) {
    (void)v;
    struct vfunction *vf = ctx;
    struct vmsix *x = &vf->msix;
    uintptr_t hpa = x->page + offset;
    uint64_t at = hpa - x->table;
    if (at >= (uint64_t)x->count * PCI_MSIX_ENTRY_SIZE) {
        return mmio_read_host(hpa, size);
    }
    spinlock_acquire(&virt.lock);
    const struct vmsix_entry *e = &x->entries[at / PCI_MSIX_ENTRY_SIZE];
    uint32_t upper = atomic_load_explicit(&e->upper, memory_order_relaxed);
    uint32_t words[4] = { e->address, upper, e->data, e->control };
    spinlock_release(&virt.lock);
    uint64_t value = 0;
    memcpy(&value, (const uint8_t *)words + at % PCI_MSIX_ENTRY_SIZE, size);
    return value;
}

static void msix_write(struct vcpu *v, void *ctx, uint64_t offset, size_t size, uint64_t value) {
    (void)v;
    struct vfunction *vf = ctx;
    struct vmsix *x = &vf->msix;
    uintptr_t hpa = x->page + offset;
    uint64_t at = hpa - x->table;
    if (at >= (uint64_t)x->count * PCI_MSIX_ENTRY_SIZE) {
        mmio_write_host(hpa, size, value);
        return;
    }
    spinlock_acquire(&virt.lock);
    size_t i = (unsigned int)(at / PCI_MSIX_ENTRY_SIZE);
    struct vmsix_entry *e = &x->entries[i];
    uint32_t upper = atomic_load_explicit(&e->upper, memory_order_relaxed);
    uint32_t words[4] = { e->address, upper, e->data, e->control };
    memcpy((uint8_t *)words + at % PCI_MSIX_ENTRY_SIZE, &value, size);
    e->address = words[0] & ~(uint32_t)3;
    atomic_store_explicit(&e->upper, words[1], memory_order_relaxed);
    e->data = words[2];
    e->control = words[3];
    msix_sync(vf, i);
    spinlock_release(&virt.lock);
}

static const struct mmio_ops msix_ops = { .read = msix_read, .write = msix_write };

struct shared_target {
    struct vfunction *vf;
    uintptr_t hpa;
    bool msix;
    const struct mmio_ops *ops;
    void *ctx;
    uint64_t offset;
};

// The BAR of the page an access falls in, if any, found under the lock, which
// msix_read() and msix_write() take themselves.
static bool shared_target(
    const struct shared_page *sp,
    uint64_t offset,
    size_t size,
    struct shared_target *t
) {
    bool found = false;
    spinlock_acquire(&virt.lock);
    uintptr_t gpa = sp->page + offset;
    for (size_t i = 0; i < sp->count && !found; i++) {
        struct vfunction *vf = sp->residents[i].vf;
        const struct vbar *b = &vf->bars[sp->residents[i].index];
        if (gpa >= b->mapped && gpa + size <= b->mapped + b->size) {
            *t = (struct shared_target){
                vf, b->host + (gpa - b->mapped), b->trap_count != 0, b->sub_page_ops,
                b->sub_page_ctx, gpa - b->mapped
            };
            found = true;
        }
    }
    spinlock_release(&virt.lock);
    return found;
}

static uint64_t shared_read(struct vcpu *v, void *ctx, uint64_t offset, size_t size) {
    struct shared_target t;
    if (!shared_target(ctx, offset, size, &t)) {
        return size == 8 ? UINT64_MAX : all_ones(size);
    }
    if (t.ops != NULL) {
        return t.ops->read(v, t.ctx, t.offset, size);
    }
    if (t.msix) {
        return msix_read(v, t.vf, t.hpa - t.vf->msix.page, size);
    }
    return mmio_read_host(t.hpa, size);
}

static void shared_write(struct vcpu *v, void *ctx, uint64_t offset, size_t size, uint64_t value) {
    struct shared_target t;
    if (!shared_target(ctx, offset, size, &t)) {
        return;
    }
    if (t.ops != NULL) {
        t.ops->write(v, t.ctx, t.offset, size, value);
    } else if (t.msix) {
        msix_write(v, t.vf, t.hpa - t.vf->msix.page, size, value);
    } else {
        mmio_write_host(t.hpa, size, value);
    }
}

static const struct mmio_ops shared_ops = { .read = shared_read, .write = shared_write };

static struct shared_page *shared_find(uint64_t page) {
    for (size_t i = 0; i < SHARED_PAGES; i++) {
        if (shared_pages[i].page == page) {
            return &shared_pages[i];
        }
    }
    return NULL;
}

static struct shared_page *shared_take(uintptr_t page) {
    for (size_t i = 0; i < SHARED_PAGES; i++) {
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
static bool page_arbitrate(uintptr_t page) {
    struct resident found[PAGE_RESIDENTS];
    size_t n = 0;
    for (size_t i = 0; i < virt.count; i++) {
        struct vfunction *vf = &virt.functions[i];
        for (size_t k = 0; k <= BAR_COUNT && n < PAGE_RESIDENTS; k++) {
            const struct vbar *b = &vf->bars[k];
            if (b->size != 0 && sub_page(b) && b->mapped != 0 && page_down(b->mapped) == page) {
                found[n++] = (struct resident){ vf, k };
            }
        }
    }
    struct shared_page *sp = shared_find(page);
    struct vbar *only = n == 1 ? &found[0].vf->bars[found[0].index] : NULL;
    bool direct = only != NULL && only->sub_page_ops == NULL
               && ((only->mapped ^ only->host) & (PAGE_SIZE - 1)) == 0;
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

    for (size_t i = 0; i < n; i++) {
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
    memcpy(sp->residents, found, n * sizeof(struct resident));
    sp->count = n;
    return true;
}

// MSI-X Enable as the guest sets it: the entries it unmasked get their vectors
// before the function may send, and lose them after it may not.
static void msix_control(struct vfunction *vf, uint32_t value) {
    struct vmsix *x = &vf->msix;
    struct pci_cfg_addr a = vf->f->addr;
    uint32_t reg = vf->f->msix_cap;
    bool enable = (value >> 16 & PCI_MSIX_FLAGS_ENABLE) != 0;
    if (enable == x->enabled) {
        pci_cfg_write(a, reg, 4, value);
        return;
    }
    x->enabled = enable;
    if (!enable) {
        pci_cfg_write(a, reg, 4, value);
    }
    for (size_t i = 0; i < x->count; i++) {
        msix_sync(vf, i);
    }
    if (enable) {
        pci_cfg_write(a, reg, 4, value);
    }
}

// Disabled, as after a reset, every entry masked at the function, the guest's
// entries what the function held. Memory decoding is on meanwhile.
static void msix_init(struct vfunction *vf) {
    struct pci_enum_function *f = vf->f;
    struct vmsix *x = &vf->msix;
    uint32_t control = pci_cfg_read(f->addr, f->msix_cap + PCI_MSIX_FLAGS, 2);
    uint32_t table = pci_cfg_read(f->addr, f->msix_cap + PCI_MSIX_TABLE, 4);
    unsigned int bir = table & PCI_MSIX_TABLE_BIR;
    struct vbar *b = bir < BAR_COUNT ? &vf->bars[bir] : NULL;
    x->count = (control & PCI_MSIX_FLAGS_QSIZE) + 1;
    uint64_t offset = table & ~PCI_MSIX_TABLE_BIR;
    if (b == NULL || b->size == 0 || offset + (uint64_t)x->count * PCI_MSIX_ENTRY_SIZE > b->size) {
        x->count = 0;
        return;
    }
    // Without a region of its own, the guest has the table as it is.
    int region = mmio_register(0, 0, &msix_ops, vf);
    if (region < 0) {
        x->count = 0;
        return;
    }
    pci_cfg_write(f->addr, f->msix_cap + PCI_MSIX_FLAGS, 2, control & ~PCI_MSIX_FLAGS_ENABLE);
    x->table = b->host + offset;
    x->page = page_down(x->table);
    mm_map_device(x->page, page_up(x->table + (uint64_t)x->count * PCI_MSIX_ENTRY_SIZE) - x->page);
    x->entries = kmalloc(x->count * sizeof(struct vmsix_entry));
    uint32_t command = pci_cfg_read(f->addr, PCI_COMMAND, 2);
    pci_cfg_write(f->addr, PCI_COMMAND, 2, command | PCI_COMMAND_MEMORY);
    for (size_t i = 0; i < x->count; i++) {
        struct vmsix_entry *e = &x->entries[i];
        uint64_t at = x->table + (uint64_t)i * PCI_MSIX_ENTRY_SIZE;
        e->address = (uint32_t)mmio_read_host(at + PCI_MSIX_ENTRY_LOWER_ADDR, 4);
        uint32_t upper = (uint32_t)mmio_read_host(at + PCI_MSIX_ENTRY_UPPER_ADDR, 4);
        atomic_store_explicit(&e->upper, upper, memory_order_relaxed);
        e->data = (uint32_t)mmio_read_host(at + PCI_MSIX_ENTRY_DATA, 4);
        e->control = (uint32_t)mmio_read_host(at + PCI_MSIX_ENTRY_VECTOR_CTRL, 4);
        mmio_write_host(
            at + PCI_MSIX_ENTRY_VECTOR_CTRL, 4, e->control | PCI_MSIX_ENTRY_CTRL_MASKBIT
        );
        e->slot = (struct irq_slot){ .taken = msix_taken, .ctx = e, .requester = requester(f) };
    }
    pci_cfg_write(f->addr, PCI_COMMAND, 2, command);
    b->traps[0] = (struct bar_trap){ offset, (uint64_t)x->count * PCI_MSIX_ENTRY_SIZE, region };
    b->trap_count = 1;
}

static void msi_init(struct vfunction *vf) {
    struct vmsi *m = &vf->msi;
    struct pci_cfg_addr a = vf->f->addr;
    uint32_t cap = vf->f->msi_cap;
    uint16_t control = (uint16_t)pci_cfg_read(a, cap + PCI_MSI_FLAGS, 2);
    m->wide = (control & PCI_MSI_FLAGS_64BIT) != 0;
    // Disabled, as after a reset: the guest enables what it uses.
    m->control = control & PCI_MSI_FLAGS_QSIZE;
    pci_cfg_write(a, cap + PCI_MSI_FLAGS, 2, control & ~PCI_MSI_FLAGS_ENABLE);
    m->address = pci_cfg_read(a, cap + PCI_MSI_ADDRESS_LO, 4);
    uint32_t upper = m->wide ? pci_cfg_read(a, cap + PCI_MSI_ADDRESS_HI, 4) : 0;
    atomic_store_explicit(&m->upper, upper, memory_order_relaxed);
    m->data = pci_cfg_read(a, msi_data_reg(vf), 2);
    m->slot = (struct irq_slot){ .taken = msi_taken, .ctx = vf, .requester = requester(vf->f) };
}

// Resizing a BAR would move it on the host, behind the guest's back.
static bool in_rebar(const struct vfunction *vf, uint32_t offset) {
    uint32_t cap = vf->f->rebar_ext_cap;
    return cap != 0 && offset >= cap && offset < cap + REBAR_SIZE;
}

// The I/O BARs below 1000h, where ports are absent unless given.
#define LEGACY_PORTS_END 0x1000
#define IO_BAR_ADDRESS ((uint32_t)0xfffffffc)

// Where the function's I/O BAR decodes, if below 1000h: 0 where not.
static uint16_t legacy_io_base(const struct pci_enum_function *f, size_t i) {
    const struct pci_enum_bar *bar = &f->bars[i];
    if (bar->kind != PCI_ENUM_BAR_IO || bar->size == 0) {
        return 0;
    }
    uint32_t base = pci_cfg_read(f->addr, bar->reg, 4) & IO_BAR_ADDRESS;
    return base < LEGACY_PORTS_END ? (uint16_t)base : 0;
}

static bool legacy_io_moved(const struct vfunction *vf) {
    for (size_t i = 0; i < vf->f->bar_count && i < BAR_COUNT; i++) {
        if (legacy_io_base(vf->f, i) != vf->legacy_io[i]) {
            return true;
        }
    }
    return false;
}

// Gives the guest the ports of the function's I/O BARs below 1000h, and takes
// back those of the ones that left.
static void legacy_io_update(struct vfunction *vf) {
    const struct pci_enum_function *f = vf->f;
    for (size_t i = 0; i < f->bar_count && i < BAR_COUNT; i++) {
        uint16_t base = legacy_io_base(f, i);
        if (base == vf->legacy_io[i]) {
            continue;
        }
        if (vf->legacy_io[i] != 0) {
            io_unpass(vf->legacy_io[i], (size_t)f->bars[i].size);
        }
        if (base != 0) {
            io_pass(base, (size_t)f->bars[i].size);
        }
        vf->legacy_io[i] = base;
    }
}

// After the guest wrote to the function's BARs, outside virt.lock, as the
// other vCPUs may wait for it: where their ports moved, with those vCPUs out
// of the guest.
static void legacy_io_follow(struct vfunction *vf) {
    if (vf == NULL || !legacy_io_moved(vf)) {
        return;
    }
    vcpu_pause_others();
    legacy_io_update(vf);
    vcpu_resume_others();
}

static uint32_t function_read(struct pci_cfg_addr a, uint32_t offset, size_t size) {
    struct vfunction *vf = vfunction_find(a);
    if (vf != NULL && vf->hidden) {
        return all_ones(size);
    }
    uint32_t reg = offset & ~(uint32_t)3;
    uint32_t value;
    if (vf != NULL && vf->hook != NULL && vf->hook->read(vf->hook_ctx, reg, &value)) {
        return (value >> (8 * (offset & 3))) & all_ones(size);
    }
    if (vf != NULL && msi_reg(vf, reg)) {
        return (msi_read(vf, reg) >> (8 * (offset & 3))) & all_ones(size);
    }
    if (vf == NULL || offset >= PCI_STD_HEADER_SIZEOF || !is_kept(vf, reg)) {
        uint32_t vga = vga_enable_bit(vf, offset, size);
        value = pci_cfg_read(a, offset, size) & ~vga;
        return vga != 0 && vf->vga_enable ? value | vga : value;
    }
    return (kept_read(vf, reg) >> (8 * (offset & 3))) & all_ones(size);
}

// A memory BAR's register, or its lower one, as the host has it.
static uint32_t bar_host_word(const struct vfunction *vf, size_t k, uint32_t *reg) {
    const struct vbar *b = &vf->bars[k];
    if (k == ROM_INDEX) {
        *reg = is_bridge(vf) ? PCI_ROM_ADDRESS1 : PCI_ROM_ADDRESS;
        return ((uint32_t)b->host & PCI_ROM_ADDRESS_MASK)
             | (b->enabled ? PCI_ROM_ADDRESS_ENABLE : 0);
    }
    *reg = vf->f->bars[k].reg;
    return ((uint32_t)b->host & BAR_ADDRESS) | b->type_bits;
}

static bool bar_lost(const struct vfunction *vf, size_t k) {
    const struct vbar *b = &vf->bars[k];
    if (b->size == 0) {
        return false;
    }
    uint32_t reg;
    uint32_t want = bar_host_word(vf, k, &reg);
    struct pci_cfg_addr a = vf->f->addr;
    return pci_cfg_read(a, reg, 4) != want
        || (b->kind == PCI_ENUM_BAR_MEM64
         && pci_cfg_read(a, reg + 4, 4) != (uint32_t)(b->host >> 32));
}

// What a reset of the function clears on the host that the guest's writes do
// not reach, as the hypervisor keeps the guest's copies: its memory BARs and
// their sizes, a bridge's bus numbers and memory windows, and the messages MSI
// and MSI-X send, which MSI's and MSI-X's enables going off show. After a
// D3hot to D0 transition with No_Soft_Reset clear (PCI Bus Power Management
// Interface Specification 1.2, 3.2.4 and 5.4.1), a function level reset, or a
// bus reset, the guest restores its own copies and turns decoding back on, as
// Linux 7.3's pci_restore_state() does, and the host's go back first.
static void reset_repair(struct vfunction *vf) {
    struct pci_cfg_addr a = vf->f->addr;
    if (pci_cfg_read(a, PCI_VENDOR_ID, 2) == 0xffff) {
        return;
    }
    bool lost = false;
    for (size_t k = 0; k <= BAR_COUNT && !lost; k++) {
        lost = bar_lost(vf, k);
    }
    if (!lost && is_bridge(vf)) {
        lost = (pci_cfg_read(a, PCI_PRIMARY_BUS, 4) & BUS_NUMBERS_VIRTUAL) != vf->host_buses;
        for (size_t i = 0; i < ARRAY_COUNT(vf->host_windows) && !lost; i++) {
            lost = pci_cfg_read(a, PCI_MEMORY_BASE + 4 * (uint32_t)i, 4) != vf->host_windows[i];
        }
    }
    if (!lost && vf->f->msi_cap != 0 && vf->msi.slot.cpu != NULL
     && (vf->msi.control & PCI_MSI_FLAGS_ENABLE) != 0) {
        uint32_t flags = pci_cfg_read(a, vf->f->msi_cap + PCI_MSI_FLAGS, 2);
        lost = (flags & PCI_MSI_FLAGS_ENABLE) == 0;
    }
    if (!lost && vf->msix.count != 0 && vf->msix.enabled) {
        uint32_t flags = pci_cfg_read(a, vf->f->msix_cap + PCI_MSIX_FLAGS, 2);
        lost = (flags & PCI_MSIX_FLAGS_ENABLE) == 0;
    }
    if (!lost) {
        return;
    }

    uint32_t command = pci_cfg_read(a, PCI_COMMAND, 2);
    uint32_t decode = PCI_COMMAND_IO | PCI_COMMAND_MEMORY;
    if ((command & decode) != 0) {
        pci_cfg_write(a, PCI_COMMAND, 2, command & ~decode);
    }
    pci_enum_rebar_restore(vf->f);
    for (size_t k = 0; k <= BAR_COUNT; k++) {
        const struct vbar *b = &vf->bars[k];
        if (b->size == 0) {
            continue;
        }
        uint32_t reg;
        uint32_t low = bar_host_word(vf, k, &reg);
        // The upper register first, as in rebar_resize().
        if (b->kind == PCI_ENUM_BAR_MEM64) {
            pci_cfg_write(a, reg + 4, 4, (uint32_t)(b->host >> 32));
        }
        pci_cfg_write(a, reg, 4, low);
    }
    if (is_bridge(vf)) {
        uint32_t buses = pci_cfg_read(a, PCI_PRIMARY_BUS, 4);
        pci_cfg_write(a, PCI_PRIMARY_BUS, 4, (buses & ~BUS_NUMBERS_VIRTUAL) | vf->host_buses);
        for (size_t i = 0; i < ARRAY_COUNT(vf->host_windows); i++) {
            pci_cfg_write(a, PCI_MEMORY_BASE + 4 * (uint32_t)i, 4, vf->host_windows[i]);
        }
    }
    if (vf->f->msi_cap != 0) {
        irq_slot_free(&vf->msi.slot);
        msi_update(vf);
    }
    for (size_t i = 0; i < vf->msix.count; i++) {
        vf->msix.entries[i].armed = false;
        irq_slot_free(&vf->msix.entries[i].slot);
    }
    vf->msix_rearm = vf->msix.count != 0;
    if (vf->resets_logged < RESETS_LOGGED) {
        vf->resets_logged++;
        printf(
            "PCI " PCI_CFG_ADDR_FMT ": reset, its registers on the host set up again\n",
            PCI_CFG_ADDR_ARGS(a)
        );
    }
}

// The MSI-X entries a reset took from the function, once it decodes memory to
// reach its table again.
static void msix_rearm(struct vfunction *vf) {
    if (!vf->msix_rearm
     || (pci_cfg_read(vf->f->addr, PCI_COMMAND, 2) & PCI_COMMAND_MEMORY) == 0) {
        return;
    }
    vf->msix_rearm = false;
    for (size_t i = 0; i < vf->msix.count; i++) {
        msix_sync(vf, i);
    }
}

// The function whose BAR registers the guest wrote to, for legacy_io_follow():
// NULL for other writes.
static struct vfunction *config_write(
    struct vfunction *vf,
    struct pci_cfg_addr a,
    uint32_t offset,
    size_t size,
    uint32_t value
) {
    uint32_t reg = offset & ~(uint32_t)3;
    unsigned int shift = 8 * (offset & 3);
    uint32_t mask = all_ones(size) << shift;
    if (vf != NULL && vf->hook != NULL
     && vf->hook->write(vf->hook_ctx, reg, value << shift, mask)) {
        return NULL;
    }
    if (vf != NULL && msi_reg(vf, reg)) {
        msi_write(vf, reg, (msi_read(vf, reg) & ~mask) | ((value << shift) & mask));
        return NULL;
    }
    if (vf != NULL && vf->msix.count != 0 && reg == vf->f->msix_cap) {
        msix_control(vf, (pci_cfg_read(a, reg, 4) & ~mask) | ((value << shift) & mask));
        return NULL;
    }
    if (vf == NULL || offset >= PCI_STD_HEADER_SIZEOF || !is_kept(vf, reg)) {
        uint32_t vga = vga_enable_bit(vf, offset, size);
        if (vga != 0) {
            vf->vga_enable = (value & vga) != 0;
            value = (value & ~vga) | (pci_cfg_read(a, offset, size) & vga);
        }
        pci_cfg_write(a, offset, size, value);
        return reg >= PCI_BASE_ADDRESS_0 && reg < PCI_BASE_ADDRESS_0 + 4 * BAR_COUNT ? vf : NULL;
    }
    kept_write(vf, reg, (kept_read(vf, reg) & ~mask) | ((value << shift) & mask));
    return NULL;
}

// As config_write(), with what a reset of the function took away on the host
// set up again around the guest's writes to its command register.
static struct vfunction *function_write(
    struct pci_cfg_addr a,
    uint32_t offset,
    size_t size,
    uint32_t value
) {
    struct vfunction *vf = vfunction_find(a);
    if (vf != NULL && (vf->hidden || in_rebar(vf, offset))) {
        return NULL;
    }
    bool command = vf != NULL && (offset & ~(uint32_t)3) == PCI_COMMAND;
    if (command) {
        reset_repair(vf);
    }
    struct vfunction *wrote = config_write(vf, a, offset, size, value);
    if (command) {
        msix_rearm(vf);
    }
    return wrote;
}

// The guest's bus numbers are the PCI plan's: a port of the plan's, or a
// function of the host's, or nothing.
static uint32_t guest_read(
    uint8_t bus,
    uint8_t device,
    uint8_t function,
    uint32_t offset,
    size_t size
) {
    int port = bus == 0 ? pci_port_at(device, function) : -1;
    if (port >= 0) {
        return pci_port_read((size_t)port, offset, size);
    }
    const struct pci_plan_bus *b = &pci_plan()->buses[bus];
    if (!b->present) {
        return all_ones(size);
    }
    struct pci_cfg_addr a = { b->segment, b->bus, device, function };
    return function_read(a, offset, size);
}

// As function_write().
static struct vfunction *guest_write(
    uint8_t bus,
    uint8_t device,
    uint8_t function,
    uint32_t offset,
    size_t size,
    uint32_t value
) {
    int port = bus == 0 ? pci_port_at(device, function) : -1;
    if (port >= 0) {
        pci_port_write((size_t)port, offset, size, value);
        return NULL;
    }
    const struct pci_plan_bus *b = &pci_plan()->buses[bus];
    if (!b->present) {
        return NULL;
    }
    struct pci_cfg_addr a = { b->segment, b->bus, device, function };
    return function_write(a, offset, size, value);
}

// What the guest may write of the reset control register, whose RST_CPU's rise
// resets the machine, or with SYS_RST clear, is INIT#.
#define RST_CNT_WRITABLE (RST_CNT_SYS_RST | RST_CNT_RST_CPU | RST_CNT_FULL_RST)

static struct spinlock rst_cnt_lock;
static uint8_t rst_cnt;

static void rst_cnt_access(bool in, uint32_t *value) {
    spinlock_acquire(&rst_cnt_lock);
    if (in) {
        *value = (*value & ~(uint32_t)0xff) | rst_cnt;
        spinlock_release(&rst_cnt_lock);
        return;
    }
    uint8_t old = rst_cnt;
    uint8_t now = (uint8_t)*value & RST_CNT_WRITABLE;
    rst_cnt = now;
    spinlock_release(&rst_cnt_lock);
    if ((old & RST_CNT_RST_CPU) != 0 || (now & RST_CNT_RST_CPU) == 0) {
        return;
    }
    if ((now & RST_CNT_SYS_RST) != 0) {
        power_reset((now & RST_CNT_FULL_RST) == 0);
    }
    vcpu_init_all();
}

// Only dword accesses to CF8h are the configuration address: the others, but
// for the reset control register, and data accesses with the address disabled,
// reach nothing, as on a chipset without other devices there.
static void config_ports(
    struct vcpu *v,
    void *ctx,
    uint16_t offset,
    size_t size,
    bool in,
    uint32_t *value
) {
    (void)v;
    (void)ctx;
    uint16_t port = (uint16_t)(PCI_CFG_ADDRESS + offset);
    if (port == RST_CNT && size == 1) {
        rst_cnt_access(in, value);
        return;
    }
    spinlock_acquire(&virt.lock);
    if (port == PCI_CFG_ADDRESS && size == 4) {
        if (in) {
            *value = virt.config_address;
        } else {
            virt.config_address = *value & ~(uint32_t)3;
        }
        spinlock_release(&virt.lock);
        return;
    }
    uint32_t address = virt.config_address;
    unsigned int byte = (unsigned int)(port - PCI_CFG_DATA);
    if (port < PCI_CFG_DATA || (address & PCI_CFG_ENABLE) == 0 || byte + size > 4) {
        if (in) {
            *value = 0xffffffff;
        }
        spinlock_release(&virt.lock);
        return;
    }
    uint8_t bus = (uint8_t)(address >> 16);
    uint8_t device = (address >> 11) & 0x1f;
    uint8_t function = (address >> 8) & 7;
    // Bits 27:24 extend the register number where the chipset allows it.
    uint32_t reg = (address & 0xfc) | ((address >> 16) & 0xf00);
    struct vfunction *wrote = NULL;
    if (byte % size == 0) {
        if (in) {
            *value = guest_read(bus, device, function, reg + byte, size);
        } else {
            wrote = guest_write(bus, device, function, reg + byte, size, *value);
        }
    } else {
        uint32_t result = 0;
        for (size_t i = 0; i < size; i++) {
            uint32_t at = reg + byte + (uint32_t)i;
            if (in) {
                result |= guest_read(bus, device, function, at, 1) << (8 * i);
            } else {
                struct vfunction *vf = guest_write(
                    bus, device, function, at, 1, (*value >> (8 * i)) & 0xff
                );
                wrote = vf != NULL ? vf : wrote;
            }
        }
        if (in) {
            *value = result;
        }
    }
    spinlock_release(&virt.lock);
    legacy_io_follow(wrote);
}

static uint32_t ecam_reg(uint64_t offset) {
    return (uint32_t)(offset % FUNCTION_SIZE);
}

static uint8_t ecam_bus(uint64_t offset) {
    return (uint8_t)(offset / BUS_SIZE);
}

static uint8_t ecam_device(uint64_t offset) {
    return (uint8_t)((offset >> 15) & 0x1f);
}

static uint8_t ecam_function(uint64_t offset) {
    return (uint8_t)((offset >> 12) & 7);
}

static uint64_t ecam_read(struct vcpu *v, void *ctx, uint64_t offset, size_t size) {
    (void)v;
    (void)ctx;
    uint8_t bus = ecam_bus(offset);
    uint8_t device = ecam_device(offset);
    uint8_t function = ecam_function(offset);
    uint32_t reg = ecam_reg(offset);
    spinlock_acquire(&virt.lock);
    uint64_t value;
    if (size == 8) {
        value = guest_read(bus, device, function, reg, 4)
              | (uint64_t)guest_read(bus, device, function, reg + 4, 4) << 32;
    } else {
        value = guest_read(bus, device, function, reg, size);
    }
    spinlock_release(&virt.lock);
    return value;
}

static void ecam_write(struct vcpu *v, void *ctx, uint64_t offset, size_t size, uint64_t value) {
    (void)v;
    (void)ctx;
    uint8_t bus = ecam_bus(offset);
    uint8_t device = ecam_device(offset);
    uint8_t function = ecam_function(offset);
    uint32_t reg = ecam_reg(offset);
    spinlock_acquire(&virt.lock);
    struct vfunction *wrote;
    if (size == 8) {
        wrote = guest_write(bus, device, function, reg, 4, (uint32_t)value);
        struct vfunction *upper = guest_write(
            bus, device, function, reg + 4, 4, (uint32_t)(value >> 32)
        );
        wrote = upper != NULL ? upper : wrote;
    } else {
        wrote = guest_write(bus, device, function, reg, size, (uint32_t)value);
    }
    spinlock_release(&virt.lock);
    legacy_io_follow(wrote);
}

static const struct mmio_ops ecam_ops = { .read = ecam_read, .write = ecam_write };

// A bridge's bus numbers as the guest has them, where they are assigned.
static uint32_t guest_bus_numbers(const struct pci_enum_function *f, uint32_t value) {
    if (f->secondary_bus <= f->addr.bus) {
        return value;
    }
    uint32_t out = value & ~BUS_NUMBERS_VIRTUAL;
    for (size_t i = 0; i < 3; i++) {
        uint8_t host = (uint8_t)(value >> (8 * i));
        int guest = pci_plan_guest_bus(f->addr.segment, host);
        out |= (uint32_t)(guest >= 0 ? (uint8_t)guest : host) << (8 * i);
    }
    return out;
}

static void vfunction_init(
    struct vfunction *vf,
    struct pci_enum_function *f,
    const struct pci_plan_function *pf
) {
    *vf = (struct vfunction){ .f = f };
    // An AMD IOMMU's function, whose capability holds its registers' address.
    if (f->base_class == PCI_BASE_CLASS_SYSTEM && f->subclass == PCI_SUBCLASS_SYSTEM_IOMMU) {
        vf->hidden = true;
        return;
    }
    if (f->header_type != PCI_HEADER_TYPE_NORMAL && f->header_type != PCI_HEADER_TYPE_BRIDGE) {
        return;
    }
    if (f->msi_cap != 0) {
        msi_init(vf);
    }
    for (size_t i = 0; i < f->bar_count && i < BAR_COUNT; i++) {
        const struct pci_enum_bar *bar = &f->bars[i];
        if ((bar->kind != PCI_ENUM_BAR_MEM32 && bar->kind != PCI_ENUM_BAR_MEM64)
         || bar->size == 0) {
            continue;
        }
        vf->bars[i] = (struct vbar){
            .kind = bar->kind,
            .type_bits = (uint8_t)(pci_cfg_read(f->addr, bar->reg, 4)
                                 & ~BAR_ADDRESS),
            .size = bar->size, .host = bar->base, .guest = pf->bars[i]
        };
        // The hypervisor reaches it for the guest where no one page can.
        if (bar->size < PAGE_SIZE) {
            mm_map_device(page_down(bar->base), PAGE_SIZE);
        }
    }
    if (f->rom.kind == PCI_ENUM_BAR_MEM32 && f->rom.size != 0) {
        uint32_t reg = f->header_type == PCI_HEADER_TYPE_BRIDGE
                     ? PCI_ROM_ADDRESS1
                     : PCI_ROM_ADDRESS;
        vf->bars[ROM_INDEX] = (struct vbar){
            .kind = PCI_ENUM_BAR_MEM32,
            .size = f->rom.size,
            .host = f->rom.base,
            .guest = pf->rom,
            .enabled = (pci_cfg_read(f->addr, reg, 4) & PCI_ROM_ADDRESS_ENABLE) != 0
        };
        if (f->rom.size < PAGE_SIZE) {
            mm_map_device(page_down(f->rom.base), PAGE_SIZE);
        }
    }
    if (f->msix_cap != 0) {
        msix_init(vf);
    }
    if (f->header_type == PCI_HEADER_TYPE_BRIDGE) {
        vf->bus_numbers = guest_bus_numbers(f, pci_cfg_read(f->addr, PCI_PRIMARY_BUS, 4));
        vf->mem_base = pf->mem_base;
        vf->mem_limit = pf->mem_limit;
        vf->pref_base = pf->pref_base;
        vf->pref_limit = pf->pref_limit;
        vf->pref_64 = (pci_cfg_read(f->addr, PCI_PREF_MEMORY_BASE, 4) & 0xf)
                   == PCI_PREF_RANGE_TYPE_64;
        vf->host_buses = pci_cfg_read(f->addr, PCI_PRIMARY_BUS, 4) & BUS_NUMBERS_VIRTUAL;
        for (size_t i = 0; i < ARRAY_COUNT(vf->host_windows); i++) {
            vf->host_windows[i] = pci_cfg_read(f->addr, PCI_MEMORY_BASE + 4 * (uint32_t)i, 4);
        }
    }
}

static void vga_path_mark(void) {
    const struct pci_enum_function *f = pci_enum_vga();
    for (; f != NULL && f->parent != NULL; f = f->parent) {
        struct vfunction *vf = vfunction_find(f->parent->addr);
        if (vf != NULL) {
            vf->vga_path = true;
            vf->vga_enable = true;
        }
    }
}

void pci_virt_init(void) {
    const struct pci_enum_topology *t = pci_enum_topology();
    const struct pci_plan *plan = pci_plan();
    if (t == NULL || plan == NULL || virt.on) {
        return;
    }
    virt.on = true;
    pci_port_init();
    uint32_t extended = cpuid(0x80000000, 0).eax;
    unsigned int bits = extended >= 0x80000008 ? cpuid(0x80000008, 0).eax & 0xff : 36;
    virt.address_limit = (uint64_t)1 << bits;

    for (size_t i = 0; i < SHARED_PAGES; i++) {
        shared_pages[i].region = -1;
    }
    virt.count = t->function_count;
    virt.functions = kmalloc((virt.count != 0 ? virt.count : 1) * sizeof(struct vfunction));
    for (size_t i = 0; i < virt.count; i++) {
        vfunction_init(&virt.functions[i], &t->functions[i], &plan->functions[i]);
        if (!virt.functions[i].hidden) {
            legacy_io_update(&virt.functions[i]);
        }
    }
    vga_path_mark();
    uint32_t mapped = 0;
    for (size_t i = 0; i < virt.count; i++) {
        struct vfunction *vf = &virt.functions[i];
        for (size_t k = 0; k <= BAR_COUNT; k++) {
            if (vf->bars[k].size != 0) {
                bar_update(&vf->bars[k], k == ROM_INDEX);
                mapped += vf->bars[k].mapped != 0;
            }
        }
    }

    struct io_handler ports = { PCI_CFG_ADDRESS, CONFIG_PORTS, config_ports, NULL, false };
    io_register(&ports);
    if (plan->ecam != 0) {
        size_t size = ((size_t)plan->bus_end + 1) * BUS_SIZE;
        mmio_register(plan->ecam, size, &ecam_ops, NULL);
        gpt_map(plan->ecam, 0, size, 0, 0);
    }
    gpt_flush();
    printf("PCI: %u of the guest's BARs mapped onto the host's\n", mapped);
}

void pci_virt_hook(struct pci_cfg_addr addr, const struct pci_virt_hook *hook, void *ctx) {
    struct vfunction *vf = vfunction_find(addr);
    if (vf != NULL) {
        vf->hook_ctx = ctx;
        vf->hook = hook;
    }
}

bool pci_virt_trap(
    struct pci_cfg_addr addr,
    unsigned int bar,
    uint64_t offset,
    size_t size,
    const struct mmio_ops *ops,
    void *ctx
) {
    struct vfunction *vf = vfunction_find(addr);
    if (vf == NULL || bar >= BAR_COUNT) {
        return false;
    }
    struct vbar *b = &vf->bars[bar];
    if (b->size == 0 || b->trap_count == BAR_TRAPS || size == 0 || offset > b->size
     || size > b->size - offset) {
        return false;
    }
    if (sub_page(b)) {
        if (size != b->size || b->trap_count != 0 || b->sub_page_ops != NULL) {
            return false;
        }
        spinlock_acquire(&virt.lock);
        b->sub_page_ctx = ctx;
        b->sub_page_ops = ops;
        if (b->mapped != 0) {
            page_arbitrate(page_down(b->mapped));
            gpt_flush();
        }
        spinlock_release(&virt.lock);
        return true;
    }
    // Pages of their own, in order.
    unsigned int at = 0;
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
    for (size_t i = b->trap_count; i > at; i--) {
        b->traps[i] = b->traps[i - 1];
    }
    b->traps[at] = (struct bar_trap){ offset, size, region };
    b->trap_count++;
    if (b->mapped != 0) {
        uintptr_t start = trap_start(b, b->mapped, at);
        uintptr_t end = trap_end(b, b->mapped, at);
        gpt_map(start, 0, end - start, 0, 0);
        mmio_move(region, start, end - start);
        gpt_flush();
    }
    spinlock_release(&virt.lock);
    return true;
}

uint32_t pci_virt_config_read(struct pci_cfg_addr addr, uint32_t offset, size_t size) {
    spinlock_acquire(&virt.lock);
    uint32_t value = function_read(addr, offset, size);
    spinlock_release(&virt.lock);
    return value;
}

void pci_virt_config_write(
    struct pci_cfg_addr addr,
    uint32_t offset,
    size_t size,
    uint32_t value
) {
    spinlock_acquire(&virt.lock);
    struct vfunction *wrote = function_write(addr, offset, size, value);
    spinlock_release(&virt.lock);
    legacy_io_follow(wrote);
}

bool pci_virt_msix_access(
    struct pci_cfg_addr addr,
    uint64_t offset,
    size_t size,
    bool write,
    uint64_t *value
) {
    struct vfunction *vf = vfunction_find(addr);
    if (vf == NULL || vf->msix.count == 0
     || offset + size > (uint64_t)vf->msix.count * PCI_MSIX_ENTRY_SIZE) {
        return false;
    }
    uint64_t at = vf->msix.table + offset - vf->msix.page;
    if (write) {
        msix_write(NULL, vf, at, size, *value);
    } else {
        *value = msix_read(NULL, vf, at, size);
    }
    return true;
}

void pci_virt_untrap(struct pci_cfg_addr addr, unsigned int bar, uint64_t offset) {
    struct vfunction *vf = vfunction_find(addr);
    if (vf == NULL || bar >= BAR_COUNT) {
        return;
    }
    struct vbar *b = &vf->bars[bar];
    spinlock_acquire(&virt.lock);
    if (sub_page(b)) {
        b->sub_page_ops = NULL;
        if (b->mapped != 0) {
            page_arbitrate(page_down(b->mapped));
            gpt_flush();
        }
    }
    for (size_t at = 0; at < b->trap_count; at++) {
        if (b->traps[at].offset != offset) {
            continue;
        }
        mmio_move(b->traps[at].region, 0, 0);
        b->trap_count--;
        for (size_t i = at; i < b->trap_count; i++) {
            b->traps[i] = b->traps[i + 1];
        }
        if (b->mapped != 0) {
            map(b);
            gpt_flush();
        }
        break;
    }
    spinlock_release(&virt.lock);
}

void pci_virt_repost(void) {
    spinlock_acquire(&virt.lock);
    for (size_t i = 0; i < virt.count; i++) {
        struct vfunction *vf = &virt.functions[i];
        struct vmsi *m = &vf->msi;
        if (vf->hidden) {
            continue;
        }
        if (m->slot.cpu != NULL) {
            uint32_t upper = atomic_load_explicit(&m->upper, memory_order_relaxed);
            message_route(&m->slot, upper, m->address, m->data);
        }
        for (size_t k = 0; k < vf->msix.count; k++) {
            struct vmsix_entry *e = &vf->msix.entries[k];
            if (e->slot.cpu != NULL) {
                uint32_t upper = atomic_load_explicit(&e->upper, memory_order_relaxed);
                message_route(&e->slot, upper, e->address, e->data);
            }
        }
    }
    spinlock_release(&virt.lock);
}
