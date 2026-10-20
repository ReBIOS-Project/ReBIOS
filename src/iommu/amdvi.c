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
#include <hw/ioapic.h>
#include <hw/pci_cfg.h>
#include <hw/pci_enum.h>
#include <iommu/amdvi.h>
#include <iommu/iommu_tables.h>
#include <lib/heap.h>
#include <lib/log.h>
#include <lib/spinlock.h>
#include <lib/time.h>
#include <sys/mm.h>
#include <sys/x86.h>

#define REG_DEVICE_TABLE 0x0000
#define REG_COMMAND_BUFFER 0x0008
#define REG_EVENT_LOG 0x0010
#define REG_CONTROL 0x0018
#define REG_EXCLUSION_BASE 0x0020
#define REG_FEATURES 0x0030
#define REG_COMMAND_HEAD 0x2000
#define REG_COMMAND_TAIL 0x2008
#define REG_EVENT_HEAD 0x2010
#define REG_EVENT_TAIL 0x2018
#define REG_STATUS 0x2020

#define CONTROL_IOMMU_EN (1ull << 0)
#define CONTROL_EVENT_LOG_EN (1ull << 2)
#define CONTROL_COHERENT (1ull << 10)
#define CONTROL_COMMAND_BUFFER_EN (1ull << 12)
// What firmware may have turned on that ReBIOS does not use: interrupt
// enables, peripheral page requests, guest translation, guest virtual APIC,
// dual logs, and device table segmentation, along with the enables above.
#define CONTROL_TAKEN 0x0000001ffe03f01dull

#define STATUS_EVENT_OVERFLOW (1ull << 0)
#define STATUS_EVENT_LOG_RUN (1ull << 3)
#define STATUS_COMMAND_BUFFER_RUN (1ull << 4)

// 256 entries of 16 bytes, the least the length fields allow.
#define LOG_LENGTH_256 (8ull << 56)
#define LOG_ENTRIES 256

#define DEVICE_IDS 65536
// The device table holds every device ID, in (511 + 1) 4KiB pages.
#define DEVICE_TABLE_SIZE 0x1ffull

#define DTE_V (1ull << 0)
#define DTE_TV (1ull << 1)
#define DTE_MODE_4_LEVELS (4ull << 9)
#define DTE_IR (1ull << 61)
#define DTE_IW (1ull << 62)
// In the entry's second quadword.
#define DTE_SYS_MGT_SHIFT 40
// The interrupt half, in the third (AMD IOMMU 48882 rev. 3.11, Table 7, Table
// 9, Table 10): with IV clear, interrupts pass unremapped; with IV set, fixed
// and arbitrated ones are aborted until the device has a table, and the others
// pass only where the firmware's pass bits say.
#define DTE_IV (1ull << 0)
#define DTE_INT_TAB_LEN_SHIFT 1
#define DTE_INT_ROOT_MASK 0x000fffffffffffc0ull
#define DTE_INIT_PASS (1ull << 56)
#define DTE_EINT_PASS (1ull << 57)
#define DTE_NMI_PASS (1ull << 58)
#define DTE_INT_CTL_MASK (3ull << 60)
#define DTE_INT_CTL_REMAP (2ull << 60)
#define DTE_LINT0_PASS (1ull << 62)
#define DTE_LINT1_PASS (1ull << 63)
#define DTE_PASS (DTE_INIT_PASS | DTE_EINT_PASS | DTE_NMI_PASS | DTE_LINT0_PASS | DTE_LINT1_PASS)
#define DOMAIN_ID 1ull

// A device's interrupt remapping table: 512 entries in the basic format, as
// many as a fixed or arbitrated message's data bits 8:0 name (AMD IOMMU 48882
// rev. 3.11, Table 19, Table 20).
#define REMAP_LOG2 9
#define REMAP_ENTRIES (1u << REMAP_LOG2)
#define IRTE_REMAP_EN (1u << 0)
#define IRTE_DEST_SHIFT 8
#define IRTE_VECTOR_SHIFT 16
#define MSI_ADDRESS 0xfee00000u
#define RTE_ACTIVE_LOW (1ull << 13)
#define RTE_LEVEL (1ull << 15)
// PCI Express Capabilities register Device/Port Types.
#define PCIE_ROOT_PORT 0x4
#define PCIE_TO_PCI_BRIDGE 0x7
#define PCI_TO_PCIE_BRIDGE 0x8
#define MAX_ALIASES 16

#define CMD_COMPLETION_WAIT 0x1u
#define CMD_INVALIDATE_DEVTAB_ENTRY 0x2u
#define CMD_INVALIDATE_IOMMU_PAGES 0x3u
#define CMD_INVALIDATE_INTERRUPT_TABLE 0x5u
#define CMD_SHIFT 28
#define CMD_WAIT_STORE (1u << 0)
#define CMD_PAGES_SIZE (1u << 0)
#define CMD_PAGES_PDE (1u << 1)

#define TIMEOUT_NS 1000000000ull

struct amdvi_unit {
    uint64_t base;
    uint32_t *commands;
    uint32_t command_tail;
    uint32_t *events;
    uint64_t wait_done;
};

// A table in a page of its own: its entries, the ones in use, and the device
// IDs whose entries point at it, as a device's messages may come with any of
// several.
struct remap_table {
    uint32_t entries[REMAP_ENTRIES];
    uint64_t used[REMAP_ENTRIES / 64];
    uint32_t user_count;
    uint16_t users[(PAGE_SIZE - REMAP_ENTRIES * 4 - REMAP_ENTRIES / 8 - 4) / 2];
};
_Static_assert(sizeof(struct remap_table) <= PAGE_SIZE, "a remapping table takes a page");

static struct amdvi_unit *units;
static uint32_t unit_count;
static uint64_t *device_table;
// Over the units' command buffers, which any processor may fill.
static struct spinlock command_lock;
static bool remap_enabled;
static struct spinlock remap_lock;

#define EVENT_WRITE_TIMEOUT_NS 1000000ull
#define L2_CONFIG_INDEX 0xf0
#define L2_CONFIG_WRITE (1u << 8)
#define L2_CONFIG_DATA 0xf4
#define L2_L2B_CK_GATE_CONTROL 0x90
#define CK_GATE_L2B_MISC_DISABLE (1u << 2)

// Erratum 746 (Revision Guide for AMD Family 15h Models 10h-1Fh Processors
// 48931 rev. 3.08): logging can stall the IOMMU's translations while the L2B
// miscellaneous clock is gated, which the firmware is to have turned off
// (registers from the BKDG, 42300 rev. 3.12, D0F2xF0 and D0F2xF4_x90).
static void erratum_746(struct pci_addr a) {
    struct cpu_signature s = cpu_signature();
    if (s.family != 0x15 || s.model < 0x10 || s.model > 0x1f) {
        return;
    }
    pci_cfg_write(a, L2_CONFIG_INDEX, 4, L2_L2B_CK_GATE_CONTROL);
    uint32_t value = pci_cfg_read(a, L2_CONFIG_DATA, 4);
    if (!(value & CK_GATE_L2B_MISC_DISABLE)) {
        pci_cfg_write(a, L2_CONFIG_INDEX, 4, L2_L2B_CK_GATE_CONTROL | L2_CONFIG_WRITE);
        pci_cfg_write(a, L2_CONFIG_DATA, 4, value | CK_GATE_L2B_MISC_DISABLE);
        pci_cfg_write(a, L2_CONFIG_INDEX, 4, L2_L2B_CK_GATE_CONTROL);
        printf("Working around AMD erratum 746\n");
    }
}

static uint64_t reg_read(const struct amdvi_unit *u, uint32_t reg) {
    return *(volatile uint64_t *)(uintptr_t)(u->base + reg);
}

// Fenced, as a write may hand the unit tables or commands just written.
static void reg_write(const struct amdvi_unit *u, uint32_t reg, uint64_t value) {
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    *(volatile uint64_t *)(uintptr_t)(u->base + reg) = value;
}

static void command_submit(struct amdvi_unit *u, uint32_t w0, uint32_t w1, uint32_t w2,
                           uint32_t w3) {
    uint32_t next = (u->command_tail + 1) % LOG_ENTRIES;
    uint64_t deadline = time_ns() + TIMEOUT_NS;
    while (((reg_read(u, REG_COMMAND_HEAD) >> 4) & (LOG_ENTRIES - 1)) == next) {
        if (time_ns() > deadline) {
            panic("AMD-Vi unit at %#llx does not process commands\n",
                  (unsigned long long)u->base);
        }
        cpu_relax();
    }
    uint32_t *command = &u->commands[u->command_tail * 4];
    command[0] = w0;
    command[1] = w1;
    command[2] = w2;
    command[3] = w3;
    u->command_tail = next;
    reg_write(u, REG_COMMAND_TAIL, (uint64_t)next << 4);
}

// Waits for everything submitted so far to complete.
static void command_sync(struct amdvi_unit *u) {
    __atomic_store_n(&u->wait_done, 0, __ATOMIC_RELAXED);
    uint64_t store = (uint64_t)(uintptr_t)&u->wait_done;
    command_submit(u, (uint32_t)store | CMD_WAIT_STORE,
                   (uint32_t)(store >> 32) | CMD_COMPLETION_WAIT << CMD_SHIFT, 1, 0);

    uint64_t deadline = time_ns() + TIMEOUT_NS;
    while (__atomic_load_n(&u->wait_done, __ATOMIC_ACQUIRE) != 1) {
        if (time_ns() > deadline) {
            panic("AMD-Vi unit at %#llx does not complete commands\n",
                  (unsigned long long)u->base);
        }
        cpu_relax();
    }
}

static void invalidate_pages(struct amdvi_unit *u) {
    // Every address of the domain, its page size encoded in the address.
    spinlock_acquire(&command_lock);
    command_submit(u, 0, CMD_INVALIDATE_IOMMU_PAGES << CMD_SHIFT | (uint32_t)DOMAIN_ID,
                   0xfffff000u | CMD_PAGES_PDE | CMD_PAGES_SIZE, 0x7fffffffu);
    command_sync(u);
    spinlock_release(&command_lock);
}

// A device's table entry and interrupt table, on every unit.
static void invalidate_device(uint16_t id) {
    spinlock_acquire(&command_lock);
    for (uint32_t i = 0; i < unit_count; i++) {
        command_submit(&units[i], id, CMD_INVALIDATE_DEVTAB_ENTRY << CMD_SHIFT, 0, 0);
        command_submit(&units[i], id, CMD_INVALIDATE_INTERRUPT_TABLE << CMD_SHIFT, 0, 0);
        command_sync(&units[i]);
    }
    spinlock_release(&command_lock);
}

// Device table entries are invalidated one by one, as AMD IOMMU 48882 rev. 3.11,
// 2.4.2 asks of software that changes them, even where INVALIDATE_IOMMU_ALL is supported:
// QEMU's IOMMU only starts translating a device's DMA then.
static void invalidate_all(struct amdvi_unit *u) {
    spinlock_acquire(&command_lock);
    for (uint32_t id = 0; id < DEVICE_IDS; id++) {
        command_submit(u, id, CMD_INVALIDATE_DEVTAB_ENTRY << CMD_SHIFT, 0, 0);
    }
    command_sync(u);
    spinlock_release(&command_lock);
    invalidate_pages(u);
}

bool amdvi_probe(const struct iommu_tables *tables, int *leaf_level) {
    unit_count = tables->unit_count;
    units = kmalloc(unit_count * sizeof(*units));
    for (uint32_t i = 0; i < unit_count; i++) {
        struct amdvi_unit *u = &units[i];
        u->base = tables->units[i].base;
        if (u->base == 0) {
            printf("AMD-Vi unit %u has no registers\n", i);
            return false;
        }
        const struct iommu_unit *t = &tables->units[i];
        uint16_t rid = t->amd_rid;
        erratum_746((struct pci_addr){t->segment, (uint8_t)(rid >> 8), (uint8_t)(rid >> 3 & 0x1f),
                                      (uint8_t)(rid & 7)});
        printf("AMD-Vi unit %u at %#llx: features %#llx, control %#llx\n", i,
               (unsigned long long)u->base, (unsigned long long)reg_read(u, REG_FEATURES),
               (unsigned long long)reg_read(u, REG_CONTROL));
    }
    // Every implementation takes 4-level tables, with leaves at levels 2 and 3.
    *leaf_level = 2;
    return true;
}

// The interrupts the firmware lets a device send unremapped (AMD IOMMU 48882
// rev. 3.11, Table 106).
static uint64_t pass_bits(uint8_t settings) {
    uint64_t pass = 0;
    if (settings & IOMMU_AMD_INIT_PASS) {
        pass |= DTE_INIT_PASS;
    }
    if (settings & IOMMU_AMD_EINT_PASS) {
        pass |= DTE_EINT_PASS;
    }
    if (settings & IOMMU_AMD_NMI_PASS) {
        pass |= DTE_NMI_PASS;
    }
    if (settings & IOMMU_AMD_LINT0_PASS) {
        pass |= DTE_LINT0_PASS;
    }
    if (settings & IOMMU_AMD_LINT1_PASS) {
        pass |= DTE_LINT1_PASS;
    }
    return pass;
}

static void device_settings(uint32_t id, uint8_t settings) {
    uint64_t sys_mgt = (settings & IOMMU_AMD_SYS_MGT_MASK) >> IOMMU_AMD_SYS_MGT_SHIFT;
    device_table[id * 4 + 1] = DOMAIN_ID | sys_mgt << DTE_SYS_MGT_SHIFT;
    device_table[id * 4 + 2] = pass_bits(settings);
}

// DMA untranslated until amdvi_enable(), interrupts unremapped until then, and
// the firmware's settings for each device.
static void device_table_build(const struct iommu_tables *tables) {
    device_table = page_alloc(DEVICE_IDS * 32 / PAGE_SIZE);
    for (uint32_t id = 0; id < DEVICE_IDS; id++) {
        device_table[id * 4 + 1] = DOMAIN_ID;
    }
    for (uint32_t i = 0; i < tables->range_count; i++) {
        const struct iommu_device_range *r = &tables->ranges[i];
        for (uint32_t id = r->first; id <= r->last; id++) {
            device_settings(id, r->amd_settings);
        }
    }
    for (uint32_t i = 0; i < tables->special_count; i++) {
        device_settings(tables->specials[i].rid, tables->specials[i].amd_settings);
    }
}

static void unit_enable(struct amdvi_unit *u) {
    // Stopped first, as the buffers and logs must not change under a running
    // unit.
    uint64_t control = reg_read(u, REG_CONTROL) & ~CONTROL_TAKEN;
    reg_write(u, REG_CONTROL, control);
    uint64_t deadline = time_ns() + TIMEOUT_NS;
    while (reg_read(u, REG_STATUS) & (STATUS_COMMAND_BUFFER_RUN | STATUS_EVENT_LOG_RUN)) {
        if (time_ns() > deadline) {
            panic("AMD-Vi unit at %#llx does not stop\n", (unsigned long long)u->base);
        }
        cpu_relax();
    }

    u->commands = page_alloc(1);
    u->command_tail = 0;
    u->events = page_alloc(1);
    reg_write(u, REG_EXCLUSION_BASE, 0);
    reg_write(u, REG_DEVICE_TABLE, (uint64_t)(uintptr_t)device_table | DEVICE_TABLE_SIZE);
    reg_write(u, REG_COMMAND_BUFFER, (uint64_t)(uintptr_t)u->commands | LOG_LENGTH_256);
    reg_write(u, REG_COMMAND_HEAD, 0);
    reg_write(u, REG_COMMAND_TAIL, 0);
    reg_write(u, REG_EVENT_LOG, (uint64_t)(uintptr_t)u->events | LOG_LENGTH_256);
    reg_write(u, REG_EVENT_HEAD, 0);
    reg_write(u, REG_EVENT_TAIL, 0);
    reg_write(u, REG_STATUS, STATUS_EVENT_OVERFLOW);

    control |= CONTROL_COHERENT | CONTROL_COMMAND_BUFFER_EN | CONTROL_EVENT_LOG_EN;
    reg_write(u, REG_CONTROL, control);
    reg_write(u, REG_CONTROL, control | CONTROL_IOMMU_EN);
    invalidate_all(u);
}

void amdvi_start(void) {
    device_table_build(iommu_tables_parse());
    for (uint32_t i = 0; i < unit_count; i++) {
        unit_enable(&units[i]);
    }
}

// From here on, the interrupts of devices without a table of the hypervisor's
// are only those their pass bits let through.
void amdvi_enable(uint64_t root) {
    for (uint32_t id = 0; id < DEVICE_IDS; id++) {
        device_table[id * 4] = root | DTE_V | DTE_TV | DTE_MODE_4_LEVELS | DTE_IR | DTE_IW;
        if (remap_enabled) {
            device_table[id * 4 + 2] |= DTE_IV;
        }
    }
    for (uint32_t i = 0; i < unit_count; i++) {
        invalidate_all(&units[i]);
    }
}

// The I/O APICs' interrupts come through the IOMMUs as well, so remapping
// needs their device IDs, as Linux 7.3's check_ioapic_information() also asks.
void amdvi_remap_start(void) {
    for (uint32_t i = 0; i < ioapic_count(); i++) {
        uint8_t id = ioapic_get(i)->id;
        if (iommu_special_for(IOMMU_SPECIAL_IOAPIC, id) == NULL) {
            printf("AMD-Vi: interrupts are not remapped, as the IVRS gives I/O APIC %u no device "
                   "ID\n", id);
            return;
        }
    }
    remap_enabled = true;
}

bool amdvi_remap_on(void) {
    return remap_enabled;
}

// Erratum 1063 (Revision Guide for AMD Family 17h Models 00h-0Fh Processors
// 55449 rev. 1.21): root ports send their own interrupts with requester ID 0.
static bool erratum_1063(void) {
    struct cpu_signature s = cpu_signature();
    return s.family == 0x17 && s.model <= 0x0f;
}

static void alias_add(uint16_t *ids, unsigned *n, uint16_t id) {
    for (unsigned i = 0; i < *n; i++) {
        if (ids[i] == id) {
            return;
        }
    }
    if (*n < MAX_ALIASES) {
        ids[(*n)++] = id;
    }
}

// The requester IDs a device's messages may come with: its own, the IVRS
// alias, and those of the bridges above it that pass requests on as their own,
// PCI Express to PCI bridges with their secondary bus number, as Linux 7.3's
// pci_for_each_dma_alias() walks them; conventional bridges count both ways,
// as some PCI Express to PCI bridges have no PCI Express capability.
static unsigned aliases(uint16_t id, uint16_t *ids) {
    unsigned n = 0;
    alias_add(ids, &n, id);
    const struct iommu_device_range *range = iommu_range_for(0, id);
    if (range != NULL && range->has_alias) {
        alias_add(ids, &n, range->alias);
    }
    const struct pci_function *f = pci_enum_find(
        (struct pci_addr){0, (uint8_t)(id >> 8), (uint8_t)(id >> 3 & 0x1f), (uint8_t)(id & 7)});
    if (f != NULL && f->pcie_cap != 0 && f->pcie_port_type == PCIE_ROOT_PORT && erratum_1063()) {
        alias_add(ids, &n, 0);
    }
    for (const struct pci_function *b = f != NULL ? f->parent : NULL; b != NULL; b = b->parent) {
        uint16_t own = (uint16_t)(b->addr.bus << 8 | b->addr.device << 3 | b->addr.function);
        uint16_t secondary = (uint16_t)(b->secondary_bus << 8);
        if (b->pcie_cap == 0) {
            alias_add(ids, &n, own);
            alias_add(ids, &n, secondary);
        } else if (b->pcie_port_type == PCIE_TO_PCI_BRIDGE) {
            alias_add(ids, &n, secondary);
        } else if (b->pcie_port_type == PCI_TO_PCIE_BRIDGE) {
            alias_add(ids, &n, own);
        }
    }
    return n;
}

static struct remap_table *table_of(uint16_t id) {
    uint64_t dte = __atomic_load_n(&device_table[id * 4 + 2], __ATOMIC_RELAXED);
    if ((dte & DTE_INT_CTL_MASK) != DTE_INT_CTL_REMAP) {
        return NULL;
    }
    return (struct remap_table *)(uintptr_t)(dte & DTE_INT_ROOT_MASK);
}

// The IOMMUs cache entries per device ID.
static void invalidate_table(const struct remap_table *t) {
    spinlock_acquire(&command_lock);
    for (uint32_t i = 0; i < unit_count; i++) {
        for (uint32_t j = 0; j < t->user_count; j++) {
            command_submit(&units[i], t->users[j], CMD_INVALIDATE_INTERRUPT_TABLE << CMD_SHIFT, 0,
                           0);
        }
        command_sync(&units[i]);
    }
    spinlock_release(&command_lock);
}

static void table_install(uint16_t id, struct remap_table *t) {
    if (t->user_count == sizeof(t->users) / sizeof(t->users[0])) {
        printf("AMD-Vi: no room for device ID %04x in an interrupt remapping table\n", id);
        return;
    }
    t->users[t->user_count++] = id;
    uint64_t *dte = &device_table[id * 4 + 2];
    __atomic_store_n(dte,
                     (*dte & DTE_PASS) | DTE_IV | (uint64_t)REMAP_LOG2 << DTE_INT_TAB_LEN_SHIFT
                         | (uint64_t)(uintptr_t)t | DTE_INT_CTL_REMAP,
                     __ATOMIC_RELEASE);
    invalidate_device(id);
}

// The table of a requester ID and the IDs that may stand for it, which all
// point at one.
static struct remap_table *table_for(uint16_t id) {
    uint16_t ids[MAX_ALIASES];
    unsigned n = aliases(id, ids);
    struct remap_table *t = NULL;
    for (unsigned i = 0; i < n && t == NULL; i++) {
        t = table_of(ids[i]);
    }
    if (t == NULL) {
        t = page_alloc(1);
    }
    for (unsigned i = 0; i < n; i++) {
        struct remap_table *had = table_of(ids[i]);
        if (had == t) {
            continue;
        }
        if (had != NULL) {
            printf("AMD-Vi: device ID %04x moves to the interrupt remapping table of %04x\n",
                   ids[i], id);
        }
        table_install(ids[i], t);
    }
    return t;
}

// Aligned to count, as a multiple message's index goes in the data's low bits.
int amdvi_remap_alloc(int32_t requester, unsigned count) {
    if (requester < 0) {
        return -1;
    }
    spinlock_acquire(&remap_lock);
    struct remap_table *t = table_for((uint16_t)requester);
    int found = -1;
    for (unsigned base = 0; found < 0 && base + count <= REMAP_ENTRIES; base += count) {
        unsigned n = 0;
        while (n < count && !(t->used[(base + n) / 64] & 1ull << ((base + n) % 64))) {
            n++;
        }
        if (n == count) {
            found = (int)base;
        }
    }
    for (unsigned i = 0; found >= 0 && i < count; i++) {
        t->used[(found + i) / 64] |= 1ull << ((found + i) % 64);
    }
    spinlock_release(&remap_lock);
    return found;
}

void amdvi_remap_free(int32_t requester, int index, unsigned count) {
    spinlock_acquire(&remap_lock);
    struct remap_table *t = table_of((uint16_t)requester);
    if (t != NULL) {
        for (unsigned i = 0; i < count; i++) {
            __atomic_store_n(&t->entries[index + i], 0, __ATOMIC_RELEASE);
            t->used[(index + i) / 64] &= ~(1ull << ((index + i) % 64));
        }
        invalidate_table(t);
    }
    spinlock_release(&remap_lock);
}

// Fixed delivery, physical destination.
void amdvi_remap_set(int32_t requester, int index, uint32_t apic_id, uint8_t vector) {
    uint32_t entry = IRTE_REMAP_EN | (apic_id & 0xff) << IRTE_DEST_SHIFT
                     | (uint32_t)vector << IRTE_VECTOR_SHIFT;
    spinlock_acquire(&remap_lock);
    struct remap_table *t = table_of((uint16_t)requester);
    if (t != NULL && __atomic_load_n(&t->entries[index], __ATOMIC_RELAXED) != entry) {
        __atomic_store_n(&t->entries[index], entry, __ATOMIC_RELEASE);
        invalidate_table(t);
    }
    spinlock_release(&remap_lock);
}

void amdvi_remap_message(int index, uint32_t *address, uint32_t *data) {
    *address = MSI_ADDRESS;
    *data = (uint32_t)index;
}

// An I/O APIC's message data is its entry's low 16 bits, so the index goes in
// the vector field and the delivery mode's low bit, fixed and arbitrated being
// remapped alike.
uint64_t amdvi_remap_rte(int index, bool level, bool active_low) {
    return ((uint64_t)index & (REMAP_ENTRIES - 1)) | (level ? RTE_LEVEL : 0)
           | (active_low ? RTE_ACTIVE_LOW : 0);
}

void amdvi_flush(void) {
    for (uint32_t i = 0; i < unit_count; i++) {
        invalidate_pages(&units[i]);
    }
}

void amdvi_report_faults(void) {
    for (uint32_t i = 0; i < unit_count; i++) {
        struct amdvi_unit *u = &units[i];
        uint32_t head = (reg_read(u, REG_EVENT_HEAD) >> 4) & (LOG_ENTRIES - 1);
        uint32_t tail = (reg_read(u, REG_EVENT_TAIL) >> 4) & (LOG_ENTRIES - 1);
        while (head != tail) {
            volatile uint32_t *event = &u->events[head * 4];
            // Erratum 732 (Revision Guide for AMD Family 15h Models 10h-1Fh
            // Processors 48931 rev. 3.08): the tail can move before the entry
            // is written. Entries start as zeros, and are zeroed once read, so
            // an event code of 0 means one still to come.
            uint64_t deadline = time_ns() + EVENT_WRITE_TIMEOUT_NS;
            while (event[1] >> CMD_SHIFT == 0 && time_ns() < deadline) {
                cpu_relax();
            }
            uint32_t source = event[0] & 0xffff;
            printf("AMD-Vi unit %u: event %#x from %02x:%02x.%x: %08x %08x %08x %08x\n", i,
                   event[1] >> CMD_SHIFT, source >> 8, (source >> 3) & 0x1f, source & 7,
                   event[0], event[1], event[2], event[3]);
            for (unsigned j = 0; j < 4; j++) {
                event[j] = 0;
            }
            head = (head + 1) % LOG_ENTRIES;
        }
        reg_write(u, REG_EVENT_HEAD, (uint64_t)head << 4);
        if (reg_read(u, REG_STATUS) & STATUS_EVENT_OVERFLOW) {
            printf("AMD-Vi unit %u: more events were lost\n", i);
            reg_write(u, REG_STATUS, STATUS_EVENT_OVERFLOW);
        }
    }
}
