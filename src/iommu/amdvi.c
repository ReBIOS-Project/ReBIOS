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
#include <iommu/amdvi.h>
#include <hw/ioapic.h>
#include <hw/lapic.h>
#include <hw/pci_cfg.h>
#include <hw/pci_enum.h>
#include <hw/pci_regs.h>
#include <iommu/iommu_tables.h>
#include <iommu/remap.h>
#include <lib/heap.h>
#include <lib/log.h>
#include <lib/spinlock.h>
#include <lib/time.h>
#include <lib/util.h>
#include <sys/cpu.h>
#include <sys/mm.h>
#include <sys/smp.h>
#include <sys/x86.h>

#define REG_DEVICE_TABLE 0x0000
#define REG_COMMAND_BUFFER 0x0008
#define REG_EVENT_LOG 0x0010
#define REG_CONTROL 0x0018
#define REG_EXCLUSION_BASE 0x0020
#define REG_FEATURES 0x0030
#define REG_GA_LOG 0x00e0
#define REG_GA_LOG_TAIL_ADDRESS 0x00e8
#define REG_COMMAND_HEAD 0x2000
#define REG_COMMAND_TAIL 0x2008
#define REG_EVENT_HEAD 0x2010
#define REG_EVENT_TAIL 0x2018
#define REG_STATUS 0x2020
#define REG_GA_LOG_HEAD 0x2040
#define REG_GA_LOG_TAIL 0x2048

#define CONTROL_IOMMU_EN ((uint64_t)1 << 0)
#define CONTROL_EVENT_LOG_EN ((uint64_t)1 << 2)
#define CONTROL_COHERENT ((uint64_t)1 << 10)
#define CONTROL_COMMAND_BUFFER_EN ((uint64_t)1 << 12)
#define CONTROL_GA_EN ((uint64_t)1 << 17)
#define CONTROL_GAM_GUEST_VAPIC ((uint64_t)1 << 25)
#define CONTROL_GA_LOG_EN ((uint64_t)1 << 28)
#define CONTROL_GA_INT_EN ((uint64_t)1 << 29)
// What firmware may have turned on that ReBIOS does not use: interrupt
// enables, peripheral page requests, guest translation, guest virtual APIC,
// dual logs, and device table segmentation, along with the enables above.
#define CONTROL_TAKEN ((uint64_t)0x0000001ffe03f01d)

// The guest virtual APIC and its modes (AMD IOMMU 48882 rev. 3.11, 3.4.4).
#define FEATURE_GA ((uint64_t)1 << 7)
#define FEATURE_GAM_MASK ((uint64_t)7 << 21)
#define FEATURE_GAM_GUEST_VAPIC ((uint64_t)1 << 21)

#define STATUS_EVENT_OVERFLOW ((uint64_t)1 << 0)
#define STATUS_EVENT_LOG_RUN ((uint64_t)1 << 3)
#define STATUS_COMMAND_BUFFER_RUN ((uint64_t)1 << 4)
#define STATUS_GA_LOG_RUN ((uint64_t)1 << 8)
#define STATUS_GA_OVERFLOW ((uint64_t)1 << 9)
#define STATUS_GA_INT ((uint64_t)1 << 10)

// 256 entries of 16 bytes, the least the length fields allow.
#define LOG_LENGTH_256 ((uint64_t)8 << 56)
#define LOG_ENTRIES 256

// The GA log of interrupts for vCPUs that did not run: 512 entries of 8 bytes,
// the least its length field allows, and the opcode of an entry for one, with
// the entry's GATag in bits 31:0 (AMD IOMMU 48882 rev. 3.11, MMIO Offset
// 00E0h, Table 76).
#define GA_LOG_LENGTH_512 ((uint64_t)8 << 56)
#define GA_LOG_ENTRIES 512
#define GA_OPCODE_SHIFT 60
#define GA_GUEST_NR 0x1
// Rounds of clearing the GA log's interrupt status and reading the entries
// before it, until it reads as clear.
#define GA_ROUNDS 4

#define DEVICE_IDS 65536
// The device table holds every device ID, in (511 + 1) 4KiB pages.
#define DEVICE_TABLE_SIZE ((uint64_t)0x1ff)

#define DTE_V ((uint64_t)1 << 0)
#define DTE_TV ((uint64_t)1 << 1)
#define DTE_MODE_4_LEVELS ((uint64_t)4 << 9)
#define DTE_IR ((uint64_t)1 << 61)
#define DTE_IW ((uint64_t)1 << 62)
// In the entry's second quadword.
#define DTE_SYS_MGT_SHIFT 40
// The interrupt half, in the third (AMD IOMMU 48882 rev. 3.11, Table 7, Table
// 9, Table 10): with IV clear, interrupts pass unremapped; with IV set, fixed
// and arbitrated ones are aborted until the device has a table, and the others
// pass only where the firmware's pass bits say.
#define DTE_IV ((uint64_t)1 << 0)
#define DTE_INT_TAB_LEN_SHIFT 1
#define DTE_INT_ROOT_MASK ((uint64_t)0x000fffffffffffc0)
#define DTE_INIT_PASS ((uint64_t)1 << 56)
#define DTE_EINT_PASS ((uint64_t)1 << 57)
#define DTE_NMI_PASS ((uint64_t)1 << 58)
#define DTE_INT_CTL_MASK ((uint64_t)3 << 60)
#define DTE_INT_CTL_REMAP ((uint64_t)2 << 60)
#define DTE_LINT0_PASS ((uint64_t)1 << 62)
#define DTE_LINT1_PASS ((uint64_t)1 << 63)
#define DTE_PASS (DTE_INIT_PASS | DTE_EINT_PASS | DTE_NMI_PASS | DTE_LINT0_PASS | DTE_LINT1_PASS)
#define DOMAIN_ID ((uint64_t)1)

// A device's interrupt remapping table: 512 entries, as many as a fixed or
// arbitrated message's data bits 8:0 name, in the basic format or in the guest
// virtual APIC's (AMD IOMMU 48882 rev. 3.11, Table 19 to Table 22).
#define REMAP_LOG2 9
#define REMAP_ENTRIES ((uint32_t)1 << REMAP_LOG2)
#define IRTE_REMAP_EN ((uint32_t)1 << 0)
#define IRTE_DEST_SHIFT 8
#define IRTE_VECTOR_SHIFT 16
// With GuestMode, an entry posts to a guest virtual APIC: its backing page's
// address in place in the high half, with the vector, and its doorbell's
// destination, 8 bits wide without x2APIC support (AMD IOMMU 48882 rev. 3.11,
// Table 23, Figure 20).
#define IRTE_GA_LOG_INTR ((uint64_t)1 << 2)
#define IRTE_IS_RUN ((uint64_t)1 << 6)
#define IRTE_GUEST_MODE ((uint64_t)1 << 7)
#define IRTE_GA_TAG_SHIFT 32
#define IRTE_GA_ROOT_MASK ((uint64_t)0x000ffffffffff000)
// The highest APIC ID an 8-bit physical destination names, 0xff being
// broadcast.
#define MAX_DEST_ID 0xfe
// PCI Express Capabilities register Device/Port Types.
#define PCIE_ROOT_PORT 0x4
#define PCIE_TO_PCI_BRIDGE 0x7
#define PCI_TO_PCIE_BRIDGE 0x8
#define MAX_ALIASES 16

#define CMD_COMPLETION_WAIT ((uint32_t)0x1)
#define CMD_INVALIDATE_DEVTAB_ENTRY ((uint32_t)0x2)
#define CMD_INVALIDATE_IOMMU_PAGES ((uint32_t)0x3)
#define CMD_INVALIDATE_INTERRUPT_TABLE ((uint32_t)0x5)
#define CMD_SHIFT 28
#define CMD_WAIT_STORE ((uint32_t)1 << 0)
#define CMD_PAGES_SIZE ((uint32_t)1 << 0)
#define CMD_PAGES_PDE ((uint32_t)1 << 1)

// PCI Local Bus Specification 3.0, 6.8.1: MSI's Message Control register and
// the registers after it.

#define TIMEOUT_NS ((uint64_t)1000000000)
// As long as Linux 7.3's iommu_ga_log_enable() waits for the log to start.
#define GA_LOG_TIMEOUT_NS ((uint64_t)20000000)

struct amdvi_unit {
    uintptr_t base;
    // The function whose MSI capability the unit's own interrupts use.
    struct pci_cfg_addr function;
    uint32_t *commands;
    uint32_t command_tail;
    uint32_t *events;
    _Atomic(uint64_t) wait_done;
    // The GA log, the entry to read next, where the unit keeps a copy of its
    // tail, and whether its interrupt status may have been left set.
    uint64_t *ga_log;
    uint32_t ga_head;
    _Atomic(uint64_t) ga_tail;
    bool ga_unsettled;
};

// A table in pages of its own: its entries of either format, the ones in use,
// and the device IDs whose entries point at it, as a device's messages may
// come with any of several.
#define REMAP_TABLE_PAGES 3
#define REMAP_USERS \
    ((REMAP_TABLE_PAGES * PAGE_SIZE - REMAP_ENTRIES * 16 - REMAP_ENTRIES / 8 - 4) / 2)
struct remap_table {
    union {
        uint32_t basic[REMAP_ENTRIES];
        uint64_t wide[REMAP_ENTRIES * 2];
    } entries;
    uint64_t used[REMAP_ENTRIES / 64];
    uint32_t user_count;
    uint16_t users[REMAP_USERS];
};
_Static_assert(
    sizeof(struct remap_table) <= REMAP_TABLE_PAGES * PAGE_SIZE,
    "a remapping table takes its pages"
);

static struct amdvi_unit *units;
static size_t unit_count;
static uint64_t *device_table;
// Over the units' command buffers, which any processor may fill.
static struct spinlock command_lock;
static bool remap_enabled;
// The tables' entries are in the guest virtual APIC's format.
static bool wide_entries;
static struct spinlock remap_lock;

// The entries that post to each vCPU, by its tag, whose IsRun follows whether
// it runs: under its lock, which remap_lock comes before, and which entries
// they are under remap_lock too.
#define TARGET_ENTRIES 64
struct post_target {
    struct spinlock lock;
    bool running;
    size_t count;
    uint64_t *entries[TARGET_ENTRIES];
};
static struct post_target *targets;
static size_t target_count;
// Every unit logs the interrupts it posts for vCPUs that do not run, and
// interrupts the processor that reads the logs on ga_vector.
static bool posting;
static uint8_t ga_vector;

#define EVENT_WRITE_TIMEOUT_NS ((uint64_t)1000000)
#define L2_CONFIG_INDEX 0xf0
#define L2_CONFIG_WRITE ((uint32_t)1 << 8)
#define L2_CONFIG_DATA 0xf4
#define L2_L2B_CK_GATE_CONTROL 0x90
#define CK_GATE_L2B_MISC_DISABLE ((uint32_t)1 << 2)

// Erratum 746 (Revision Guide for AMD Family 15h Models 10h-1Fh Processors
// 48931 rev. 3.08): logging can stall the IOMMU's translations while the L2B
// miscellaneous clock is gated, which the firmware is to have turned off
// (registers from the BKDG, 42300 rev. 3.12, D0F2xF0 and D0F2xF4_x90).
static void erratum_746(struct pci_cfg_addr a) {
    struct x86_cpu_signature s = x86_cpu_signature();
    if (s.family != 0x15 || s.model < 0x10 || s.model > 0x1f) {
        return;
    }
    pci_cfg_write(a, L2_CONFIG_INDEX, 4, L2_L2B_CK_GATE_CONTROL);
    uint32_t value = pci_cfg_read(a, L2_CONFIG_DATA, 4);
    if ((value & CK_GATE_L2B_MISC_DISABLE) == 0) {
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
    atomic_thread_fence(memory_order_seq_cst);
    *(volatile uint64_t *)(uintptr_t)(u->base + reg) = value;
}

static void command_submit(
    struct amdvi_unit *u, uint32_t w0, uint32_t w1, uint32_t w2, uint32_t w3
) {
    uint32_t next = (u->command_tail + 1) % LOG_ENTRIES;
    uint64_t deadline = time_ns() + TIMEOUT_NS;
    while (((reg_read(u, REG_COMMAND_HEAD) >> 4) & (LOG_ENTRIES - 1)) == next) {
        if (time_ns() > deadline) {
            panic("AMD-Vi unit at %#llx does not process commands\n", (unsigned long long)u->base);
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
    atomic_store_explicit(&u->wait_done, 0, memory_order_relaxed);
    uintptr_t store = (uintptr_t)&u->wait_done;
    command_submit(
        u, (uint32_t)store | CMD_WAIT_STORE,
        (uint32_t)(store >> 32) | CMD_COMPLETION_WAIT << CMD_SHIFT, 1, 0
    );

    uint64_t deadline = time_ns() + TIMEOUT_NS;
    while (atomic_load_explicit(&u->wait_done, memory_order_acquire) != 1) {
        if (time_ns() > deadline) {
            panic(
                "AMD-Vi unit at %#llx does not complete commands\n", (unsigned long long)u->base
            );
        }
        cpu_relax();
    }
}

static void invalidate_pages(struct amdvi_unit *u) {
    // Every address of the domain, its page size encoded in the address.
    spinlock_acquire(&command_lock);
    command_submit(
        u, 0, CMD_INVALIDATE_IOMMU_PAGES << CMD_SHIFT | (uint32_t)DOMAIN_ID,
        (uint32_t)0xfffff000 | CMD_PAGES_PDE | CMD_PAGES_SIZE, (uint32_t)0x7fffffff
    );
    command_sync(u);
    spinlock_release(&command_lock);
}

// A device's table entry and interrupt table, on every unit.
static void invalidate_device(uint16_t id) {
    spinlock_acquire(&command_lock);
    for (size_t i = 0; i < unit_count; i++) {
        command_submit(&units[i], id, CMD_INVALIDATE_DEVTAB_ENTRY << CMD_SHIFT, 0, 0);
        command_submit(&units[i], id, CMD_INVALIDATE_INTERRUPT_TABLE << CMD_SHIFT, 0, 0);
        command_sync(&units[i]);
    }
    spinlock_release(&command_lock);
}

// Device table entries are invalidated one by one, as AMD IOMMU 48882 rev.
// 3.11, 2.4.2 asks of software that changes them, even where
// INVALIDATE_IOMMU_ALL is supported: QEMU's IOMMU only starts translating a
// device's DMA then.
static void invalidate_all(struct amdvi_unit *u) {
    spinlock_acquire(&command_lock);
    for (size_t id = 0; id < DEVICE_IDS; id++) {
        command_submit(u, id, CMD_INVALIDATE_DEVTAB_ENTRY << CMD_SHIFT, 0, 0);
    }
    command_sync(u);
    spinlock_release(&command_lock);
    invalidate_pages(u);
}

bool amdvi_probe(const struct iommu_tables *tables, size_t *leaf_level) {
    unit_count = tables->unit_count;
    units = kmalloc(unit_count * sizeof(struct amdvi_unit));
    for (size_t i = 0; i < unit_count; i++) {
        struct amdvi_unit *u = &units[i];
        u->base = tables->units[i].base;
        if (u->base == 0) {
            printf("AMD-Vi unit %zu has no registers\n", i);
            return false;
        }
        const struct iommu_unit *t = &tables->units[i];
        uint16_t rid = t->amd_rid;
        u->function = (struct pci_cfg_addr){
            t->segment, (uint8_t)(rid >> 8), (uint8_t)(rid >> 3 & 0x1f), (uint8_t)(rid & 7)
        };
        erratum_746(u->function);
        printf(
            "AMD-Vi unit %zu at %#llx: features %#llx, control %#llx\n", i,
            (unsigned long long)u->base, (unsigned long long)reg_read(u, REG_FEATURES),
            (unsigned long long)reg_read(u, REG_CONTROL)
        );
    }
    // Every implementation takes 4-level tables, with leaves at levels 2 and
    // 3.
    *leaf_level = 2;
    return true;
}

// The interrupts the firmware lets a device send unremapped (AMD IOMMU 48882
// rev. 3.11, Table 106).
static uint64_t pass_bits(uint8_t settings) {
    uint64_t pass = 0;
    if ((settings & IOMMU_AMD_INIT_PASS) != 0) {
        pass |= DTE_INIT_PASS;
    }
    if ((settings & IOMMU_AMD_EINT_PASS) != 0) {
        pass |= DTE_EINT_PASS;
    }
    if ((settings & IOMMU_AMD_NMI_PASS) != 0) {
        pass |= DTE_NMI_PASS;
    }
    if ((settings & IOMMU_AMD_LINT0_PASS) != 0) {
        pass |= DTE_LINT0_PASS;
    }
    if ((settings & IOMMU_AMD_LINT1_PASS) != 0) {
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
    for (size_t id = 0; id < DEVICE_IDS; id++) {
        device_table[id * 4 + 1] = DOMAIN_ID;
    }
    for (size_t i = 0; i < tables->range_count; i++) {
        const struct iommu_device_range *r = &tables->ranges[i];
        for (size_t id = r->first; id <= r->last; id++) {
            device_settings(id, r->amd_settings);
        }
    }
    for (size_t i = 0; i < tables->special_count; i++) {
        device_settings(tables->specials[i].rid, tables->specials[i].amd_settings);
    }
}

static void unit_enable(struct amdvi_unit *u) {
    // Stopped first, as the buffers and logs must not change under a running
    // unit.
    uint64_t control = reg_read(u, REG_CONTROL) & ~CONTROL_TAKEN;
    reg_write(u, REG_CONTROL, control);
    uint64_t deadline = time_ns() + TIMEOUT_NS;
    while ((reg_read(u, REG_STATUS) & (STATUS_COMMAND_BUFFER_RUN | STATUS_EVENT_LOG_RUN)) != 0) {
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
    for (size_t i = 0; i < unit_count; i++) {
        unit_enable(&units[i]);
    }
}

// From here on, the interrupts of devices without a table of the hypervisor's
// are only those their pass bits let through.
void amdvi_enable(uintptr_t root) {
    for (size_t id = 0; id < DEVICE_IDS; id++) {
        device_table[id * 4] = root | DTE_V | DTE_TV | DTE_MODE_4_LEVELS | DTE_IR | DTE_IW;
        if (remap_enabled) {
            device_table[id * 4 + 2] |= DTE_IV;
        }
    }
    for (size_t i = 0; i < unit_count; i++) {
        invalidate_all(&units[i]);
    }
}

// The unit's own interrupts, which it sends unremapped through its function's
// MSI capability (AMD IOMMU 48882 rev. 3.11, Capability Offset 10h), as Linux
// 7.3 has them without the units' x2APIC support: fixed, to the processor with
// the APIC ID, at vector.
static bool unit_msi(const struct amdvi_unit *u, uint32_t apic_id, uint8_t vector) {
    const struct pci_function *f = pci_enum_find(u->function);
    if (f == NULL || f->msi_cap == 0 || apic_id > MAX_DEST_ID) {
        return false;
    }
    uint32_t cap = f->msi_cap;
    uint32_t control = pci_cfg_read(u->function, cap + PCI_MSI_FLAGS, 2)
                     & ~(PCI_MSI_FLAGS_ENABLE | PCI_MSI_FLAGS_QSIZE);
    pci_cfg_write(u->function, cap + PCI_MSI_FLAGS, 2, control);
    pci_cfg_write(
        u->function, cap + PCI_MSI_ADDRESS_LO, 4,
        MSI_ADDRESS_BASE | apic_id << MSI_ADDRESS_DEST_SHIFT
    );
    uint32_t data = PCI_MSI_DATA_32;
    if ((control & PCI_MSI_FLAGS_64BIT) != 0) {
        pci_cfg_write(u->function, cap + PCI_MSI_ADDRESS_HI, 4, 0);
        data = PCI_MSI_DATA_64;
    }
    pci_cfg_write(u->function, cap + data, 2, vector);
    pci_cfg_write(u->function, cap + PCI_MSI_FLAGS, 2, control | PCI_MSI_FLAGS_ENABLE);
    return true;
}

// The GA log, started as Linux 7.3's iommu_ga_log_enable() starts it, with the
// unit interrupting for its entries.
static bool ga_log_start(struct amdvi_unit *u, uint32_t apic_id, uint8_t vector) {
    if (!unit_msi(u, apic_id, vector)) {
        printf(
            "AMD-Vi unit at %#llx cannot interrupt the processor with APIC ID %#x\n",
            (unsigned long long)u->base, apic_id
        );
        return false;
    }
    u->ga_log = page_alloc(1);
    u->ga_head = 0;
    u->ga_unsettled = false;
    atomic_store_explicit(&u->ga_tail, 0, memory_order_relaxed);
    reg_write(u, REG_GA_LOG, (uint64_t)(uintptr_t)u->ga_log | GA_LOG_LENGTH_512);
    reg_write(u, REG_GA_LOG_TAIL_ADDRESS, (uint64_t)(uintptr_t)&u->ga_tail);
    reg_write(u, REG_GA_LOG_HEAD, 0);
    reg_write(u, REG_GA_LOG_TAIL, 0);
    reg_write(u, REG_STATUS, STATUS_GA_INT | STATUS_GA_OVERFLOW);
    reg_write(u, REG_CONTROL, reg_read(u, REG_CONTROL) | CONTROL_GA_INT_EN | CONTROL_GA_LOG_EN);
    uint64_t deadline = time_ns() + GA_LOG_TIMEOUT_NS;
    while ((reg_read(u, REG_STATUS) & STATUS_GA_LOG_RUN) == 0) {
        if (time_ns() > deadline) {
            printf(
                "AMD-Vi unit at %#llx does not start its GA log\n", (unsigned long long)u->base
            );
            return false;
        }
        cpu_relax();
    }
    return true;
}

// The I/O APICs' interrupts come through the IOMMUs as well, so remapping
// needs their device IDs, as Linux 7.3's check_ioapic_information() also asks.
void amdvi_remap_start(bool guest_vapic, uint8_t vector) {
    for (size_t i = 0; i < ioapic_count(); i++) {
        uint8_t id = ioapic_get(i)->id;
        if (iommu_special_for(IOMMU_SPECIAL_IOAPIC, id) == NULL) {
            printf(
                "AMD-Vi: interrupts are not remapped, as the IVRS gives I/O APIC %u no device "
                "ID\n", id
            );
            return;
        }
    }
    // The guest virtual APIC's entries where interrupts may be posted to AVIC,
    // its mode the same on every unit (AMD IOMMU 48882 rev. 3.11, 2.2.5.2),
    // before any table is.
    bool wide = guest_vapic;
    for (size_t i = 0; i < unit_count && wide; i++) {
        uint64_t features = reg_read(&units[i], REG_FEATURES);
        wide = (features & FEATURE_GA) != 0
            && (features & FEATURE_GAM_MASK) == FEATURE_GAM_GUEST_VAPIC;
    }
    for (size_t i = 0; i < unit_count && wide; i++) {
        struct amdvi_unit *u = &units[i];
        uint64_t control = reg_read(u, REG_CONTROL);
        reg_write(u, REG_CONTROL, control | CONTROL_GA_EN | CONTROL_GAM_GUEST_VAPIC);
    }
    if (wide) {
        printf("AMD-Vi: interrupts are remapped in the guest virtual APIC's format\n");
    }
    wide_entries = wide;
    remap_enabled = true;
    // Posting, where every unit can tell the calling processor of what it
    // posts for vCPUs that do not run.
    bool post = wide;
    for (size_t i = 0; i < unit_count && post; i++) {
        post = ga_log_start(&units[i], this_cpu()->apic_id, vector);
    }
    if (post) {
        target_count = smp_cpu_count();
        targets = page_alloc(page_count(target_count * sizeof(struct post_target)));
        ga_vector = vector;
        posting = true;
        printf("AMD-Vi: interrupts are posted to the vCPUs that AVIC runs\n");
    }
}

bool amdvi_remap_on(void) {
    return remap_enabled;
}

// Erratum 1063 (Revision Guide for AMD Family 17h Models 00h-0Fh Processors
// 55449 rev. 1.21): root ports send their own interrupts with requester ID 0.
static bool erratum_1063(void) {
    struct x86_cpu_signature s = x86_cpu_signature();
    return s.family == 0x17 && s.model <= 0x0f;
}

static void alias_add(uint16_t *ids, size_t *n, uint16_t id) {
    for (size_t i = 0; i < *n; i++) {
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
static unsigned int aliases(uint16_t id, uint16_t *ids) {
    size_t n = 0;
    alias_add(ids, &n, id);
    const struct iommu_device_range *range = iommu_range_for(0, id);
    if (range != NULL && range->has_alias) {
        alias_add(ids, &n, range->alias);
    }
    struct pci_cfg_addr addr = {
        0, (uint8_t)(id >> 8), (uint8_t)(id >> 3 & 0x1f), (uint8_t)(id & 7)
    };
    const struct pci_function *f = pci_enum_find(addr);
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
    uint64_t dte = atomic_load_explicit(
        (_Atomic(uint64_t) *)&device_table[id * 4 + 2], memory_order_relaxed
    );
    if ((dte & DTE_INT_CTL_MASK) != DTE_INT_CTL_REMAP) {
        return NULL;
    }
    return (struct remap_table *)(uintptr_t)(dte & DTE_INT_ROOT_MASK);
}

// The IOMMUs cache entries per device ID.
static void invalidate_table(const struct remap_table *t) {
    spinlock_acquire(&command_lock);
    for (size_t i = 0; i < unit_count; i++) {
        for (size_t j = 0; j < t->user_count; j++) {
            command_submit(
                &units[i], t->users[j], CMD_INVALIDATE_INTERRUPT_TABLE << CMD_SHIFT, 0, 0
            );
        }
        command_sync(&units[i]);
    }
    spinlock_release(&command_lock);
}

static void table_install(uint16_t id, struct remap_table *t) {
    if (t->user_count == ARRAY_COUNT(t->users)) {
        printf("AMD-Vi: no room for device ID %04x in an interrupt remapping table\n", id);
        return;
    }
    t->users[t->user_count++] = id;
    uint64_t *dte = &device_table[id * 4 + 2];
    uint64_t value = (*dte & DTE_PASS) | DTE_IV | (uint64_t)REMAP_LOG2 << DTE_INT_TAB_LEN_SHIFT
                   | (uint64_t)(uintptr_t)t | DTE_INT_CTL_REMAP;
    atomic_store_explicit((_Atomic(uint64_t) *)dte, value, memory_order_release);
    invalidate_device(id);
}

// The table of a requester ID and the IDs that may stand for it, which all
// point at one.
static struct remap_table *table_for(uint16_t id) {
    uint16_t ids[MAX_ALIASES];
    size_t n = aliases(id, ids);
    struct remap_table *t = NULL;
    for (size_t i = 0; i < n && t == NULL; i++) {
        t = table_of(ids[i]);
    }
    if (t == NULL) {
        t = page_alloc(REMAP_TABLE_PAGES);
    }
    for (size_t i = 0; i < n; i++) {
        struct remap_table *had = table_of(ids[i]);
        if (had == t) {
            continue;
        }
        if (had != NULL) {
            printf(
                "AMD-Vi: device ID %04x moves to the interrupt remapping table of %04x\n", ids[i],
                id
            );
        }
        table_install(ids[i], t);
    }
    return t;
}

// Aligned to count, as a multiple message's index goes in the data's low bits.
int amdvi_remap_alloc(int32_t requester, size_t count) {
    if (requester < 0) {
        return -1;
    }
    spinlock_acquire(&remap_lock);
    struct remap_table *t = table_for((uint16_t)requester);
    int found = -1;
    for (size_t base = 0; found < 0 && base + count <= REMAP_ENTRIES; base += count) {
        size_t n = 0;
        while (n < count && (t->used[(base + n) / 64] & (uint64_t)1 << ((base + n) % 64)) == 0) {
            n++;
        }
        if (n == count) {
            found = (int)base;
        }
    }
    for (size_t i = 0; found >= 0 && i < count; i++) {
        t->used[(found + i) / 64] |= (uint64_t)1 << ((found + i) % 64);
    }
    spinlock_release(&remap_lock);
    return found;
}

// An entry, the high half only in the guest virtual APIC's format, written as
// the units read it: false where it already was.
static bool entry_write(struct remap_table *t, uint16_t index, uint64_t low, uint64_t high) {
    if (!wide_entries) {
        _Atomic(uint32_t) *entry = (_Atomic(uint32_t) *)&t->entries.basic[index];
        if (atomic_load_explicit(entry, memory_order_relaxed) == (uint32_t)low) {
            return false;
        }
        atomic_store_explicit(entry, (uint32_t)low, memory_order_release);
        return true;
    }
    uint64_t *entry = &t->entries.wide[index * 2];
    if (entry[0] == low && entry[1] == high) {
        return false;
    }
    x86_store16(entry, low, high);
    return true;
}

// Takes an entry that posts to a vCPU off the vCPU's list before it changes,
// so that its IsRun no longer follows the vCPU.
static void unpost(struct remap_table *t, uint16_t index) {
    if (!posting) {
        return;
    }
    uint64_t *entry = &t->entries.wide[index * 2];
    uint64_t low = atomic_load_explicit((_Atomic(uint64_t) *)entry, memory_order_relaxed);
    if ((low & (IRTE_REMAP_EN | IRTE_GUEST_MODE)) != (IRTE_REMAP_EN | IRTE_GUEST_MODE)) {
        return;
    }
    struct post_target *g = &targets[low >> IRTE_GA_TAG_SHIFT];
    spinlock_acquire(&g->lock);
    for (size_t i = 0; i < g->count; i++) {
        if (g->entries[i] == entry) {
            g->entries[i] = g->entries[--g->count];
            break;
        }
    }
    spinlock_release(&g->lock);
}

void amdvi_remap_free(int32_t requester, uint16_t index, size_t count) {
    spinlock_acquire(&remap_lock);
    struct remap_table *t = table_of((uint16_t)requester);
    if (t != NULL) {
        for (size_t i = 0; i < count; i++) {
            unpost(t, (uint16_t)(index + i));
            entry_write(t, (uint16_t)(index + i), 0, 0);
            t->used[(index + i) / 64] &= ~((uint64_t)1 << ((index + i) % 64));
        }
        invalidate_table(t);
    }
    spinlock_release(&remap_lock);
}

// Fixed delivery, physical destination: in the guest virtual APIC's format the
// vector in the high half, with the destination's top byte (AMD IOMMU 48882
// rev. 3.11, Figure 17, Figure 19).
void amdvi_remap_set(int32_t requester, uint16_t index, uint32_t apic_id, uint8_t vector) {
    uint64_t low = IRTE_REMAP_EN | (uint64_t)(apic_id & 0xff) << IRTE_DEST_SHIFT;
    uint64_t high = 0;
    if (wide_entries) {
        high = vector;
    } else {
        low |= (uint64_t)vector << IRTE_VECTOR_SHIFT;
    }
    spinlock_acquire(&remap_lock);
    struct remap_table *t = table_of((uint16_t)requester);
    if (t != NULL) {
        unpost(t, index);
        if (entry_write(t, index, low, high)) {
            invalidate_table(t);
        }
    }
    spinlock_release(&remap_lock);
}

bool amdvi_posting(void) {
    return posting;
}

// On the vCPU's list before the entry posts to it, with IsRun as the vCPU
// runs, and its GATag naming the list. The units cache all of it but IsRun,
// the destination, GALogIntr, and the GATag (AMD IOMMU 48882 rev. 3.11,
// 2.2.5.2), so IsRun changes without invalidations.
bool amdvi_remap_post(
    int32_t requester, uint16_t index, uint8_t vector, const struct remap_target *target
) {
    if (!posting || target->apic_id > MAX_DEST_ID || target->tag >= target_count) {
        return false;
    }
    uint64_t low = IRTE_REMAP_EN | IRTE_GA_LOG_INTR | IRTE_GUEST_MODE
                 | (uint64_t)target->apic_id << IRTE_DEST_SHIFT
                 | (uint64_t)target->tag << IRTE_GA_TAG_SHIFT;
    uint64_t high = vector | ((uint64_t)target->descriptor & IRTE_GA_ROOT_MASK);
    struct post_target *g = &targets[target->tag];
    spinlock_acquire(&remap_lock);
    struct remap_table *t = table_of((uint16_t)requester);
    bool posted = false;
    if (t != NULL) {
        uint64_t *entry = &t->entries.wide[index * 2];
        uint64_t had = atomic_load_explicit((_Atomic(uint64_t) *)entry, memory_order_relaxed);
        uint64_t guest = IRTE_REMAP_EN | IRTE_GUEST_MODE;
        bool listed = (had & guest) == guest && had >> IRTE_GA_TAG_SHIFT == target->tag;
        posted = listed || g->count < TARGET_ENTRIES;
        if (!listed && posted) {
            unpost(t, index);
        }
        bool changed = false;
        if (posted) {
            spinlock_acquire(&g->lock);
            if (!listed) {
                g->entries[g->count++] = entry;
            }
            changed = entry_write(t, index, low | (g->running ? IRTE_IS_RUN : 0), high);
            spinlock_release(&g->lock);
        }
        if (changed) {
            invalidate_table(t);
        }
    }
    spinlock_release(&remap_lock);
    return posted;
}

// The units set a posted vector's bit in the IRR, then read IsRun again, to
// ring the doorbell or log the interrupt (AMD IOMMU 48882 rev. 3.11, 2.2.5.2):
// a vCPU that stops running and then reads its IRR finds every interrupt
// posted before, and has those after logged.
void amdvi_running(uint32_t tag, bool running) {
    if (!posting || tag >= target_count) {
        return;
    }
    struct post_target *g = &targets[tag];
    spinlock_acquire(&g->lock);
    g->running = running;
    for (size_t i = 0; i < g->count; i++) {
        _Atomic(uint64_t) *low = (_Atomic(uint64_t) *)g->entries[i];
        if (running) {
            atomic_fetch_or_explicit(low, IRTE_IS_RUN, memory_order_seq_cst);
        } else {
            atomic_fetch_and_explicit(low, ~IRTE_IS_RUN, memory_order_seq_cst);
        }
    }
    spinlock_release(&g->lock);
}

// The interrupt status is cleared before the entries before it are read, so
// that an entry after them interrupts again, until it reads as clear, as Linux
// 7.3's amd_iommu_handle_irq() does for erratum ERBT1312: false where it may
// still be set. An overflow loses entries, so every vCPU wakes, and logging
// restarts where it stopped (AMD IOMMU 48882 rev. 3.11, 2.7.4), as Linux 7.3's
// amd_iommu_restart_log() restarts it.
static bool ga_drain(struct amdvi_unit *u, void (*woken)(uint32_t tag)) {
    for (size_t round = 0; round < GA_ROUNDS; round++) {
        uint64_t status = reg_read(u, REG_STATUS);
        if (round != 0 && (status & (STATUS_GA_INT | STATUS_GA_OVERFLOW)) == 0) {
            return true;
        }
        if ((status & STATUS_GA_INT) != 0) {
            reg_write(u, REG_STATUS, STATUS_GA_INT);
        }
        uint32_t tail = (reg_read(u, REG_GA_LOG_TAIL) >> 3) % GA_LOG_ENTRIES;
        while (u->ga_head != tail) {
            uint64_t entry = ((volatile uint64_t *)u->ga_log)[u->ga_head];
            u->ga_head = (u->ga_head + 1) % GA_LOG_ENTRIES;
            if (entry >> GA_OPCODE_SHIFT == GA_GUEST_NR) {
                woken((uint32_t)entry);
            }
        }
        reg_write(u, REG_GA_LOG_HEAD, (uint64_t)u->ga_head << 3);
        if ((status & STATUS_GA_OVERFLOW) == 0) {
            continue;
        }
        uint64_t control = reg_read(u, REG_CONTROL);
        bool stopped = (reg_read(u, REG_STATUS) & STATUS_GA_LOG_RUN) == 0;
        if (stopped) {
            reg_write(u, REG_CONTROL, control & ~(CONTROL_GA_LOG_EN | CONTROL_GA_INT_EN));
        }
        reg_write(u, REG_STATUS, STATUS_GA_OVERFLOW);
        if (stopped) {
            reg_write(u, REG_CONTROL, control | CONTROL_GA_LOG_EN | CONTROL_GA_INT_EN);
        }
        for (size_t tag = 0; tag < target_count; tag++) {
            woken((uint32_t)tag);
        }
    }
    return false;
}

// Only the processor the units interrupt reads the logs. A unit writes the
// copy of its tail before it interrupts, and an interrupt status that may
// still be set has the processor come back at once.
void amdvi_poll(void (*woken)(uint32_t tag)) {
    if (!posting) {
        return;
    }
    bool again = false;
    for (size_t i = 0; i < unit_count; i++) {
        struct amdvi_unit *u = &units[i];
        uint64_t tail = atomic_load_explicit(&u->ga_tail, memory_order_acquire);
        if (u->ga_unsettled || (tail >> 3) % GA_LOG_ENTRIES != u->ga_head) {
            u->ga_unsettled = !ga_drain(u, woken);
            again = again || u->ga_unsettled;
        }
    }
    if (again) {
        lapic_send_ipi(this_cpu()->apic_id, ICR_DELIVERY_FIXED | ga_vector);
    }
}

void amdvi_remap_message(uint16_t index, uint32_t *address, uint32_t *data) {
    *address = MSI_ADDRESS_BASE;
    *data = (uint32_t)index;
}

// An I/O APIC's message data is its entry's low 16 bits, so the index goes in
// the vector field and the delivery mode's low bit, fixed and arbitrated being
// remapped alike.
uint64_t amdvi_remap_rte(uint16_t index, bool level, bool active_low) {
    return ((uint64_t)index & (REMAP_ENTRIES - 1)) | (level ? IOAPIC_RTE_LEVEL : 0)
         | (active_low ? IOAPIC_RTE_ACTIVE_LOW : 0);
}

void amdvi_flush(void) {
    for (size_t i = 0; i < unit_count; i++) {
        invalidate_pages(&units[i]);
    }
}

void amdvi_report_faults(void) {
    for (size_t i = 0; i < unit_count; i++) {
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
            printf(
                "AMD-Vi unit %zu: event %#x from %02x:%02x.%x: %08x %08x %08x %08x\n", i,
                event[1] >> CMD_SHIFT, source >> 8, (source >> 3) & 0x1f, source & 7, event[0],
                event[1], event[2], event[3]
            );
            for (size_t j = 0; j < 4; j++) {
                event[j] = 0;
            }
            head = (head + 1) % LOG_ENTRIES;
        }
        reg_write(u, REG_EVENT_HEAD, (uint64_t)head << 4);
        if ((reg_read(u, REG_STATUS) & STATUS_EVENT_OVERFLOW) != 0) {
            printf("AMD-Vi unit %zu: more events were lost\n", i);
            reg_write(u, REG_STATUS, STATUS_EVENT_OVERFLOW);
        }
    }
}
