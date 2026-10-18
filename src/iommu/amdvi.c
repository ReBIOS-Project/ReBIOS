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
#include <hw/pci_cfg.h>
#include <iommu/amdvi.h>
#include <iommu/iommu_tables.h>
#include <lib/heap.h>
#include <lib/log.h>
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
#define DOMAIN_ID 1ull

#define CMD_COMPLETION_WAIT 0x1u
#define CMD_INVALIDATE_DEVTAB_ENTRY 0x2u
#define CMD_INVALIDATE_IOMMU_PAGES 0x3u
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

static struct amdvi_unit *units;
static uint32_t unit_count;
static uint64_t *device_table;

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
    command_submit(u, 0, CMD_INVALIDATE_IOMMU_PAGES << CMD_SHIFT | (uint32_t)DOMAIN_ID,
                   0xfffff000u | CMD_PAGES_PDE | CMD_PAGES_SIZE, 0x7fffffffu);
    command_sync(u);
}

// Device table entries are invalidated one by one, as AMD IOMMU 48882 rev. 3.11,
// 2.4.2 asks of software that changes them, even where INVALIDATE_IOMMU_ALL is supported:
// QEMU's IOMMU only starts translating a device's DMA then.
static void invalidate_all(struct amdvi_unit *u) {
    for (uint32_t id = 0; id < DEVICE_IDS; id++) {
        command_submit(u, id, CMD_INVALIDATE_DEVTAB_ENTRY << CMD_SHIFT, 0, 0);
    }
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

static void device_table_build(const struct iommu_tables *tables, uint64_t root) {
    device_table = page_alloc(DEVICE_IDS * 32 / PAGE_SIZE);
    for (uint32_t id = 0; id < DEVICE_IDS; id++) {
        uint64_t *dte = &device_table[id * 4];
        dte[0] = root | DTE_V | DTE_TV | DTE_MODE_4_LEVELS | DTE_IR | DTE_IW;
        dte[1] = DOMAIN_ID;
    }

    // Interrupts pass unremapped, but the firmware's system management
    // message settings apply.
    for (uint32_t i = 0; i < tables->range_count; i++) {
        const struct iommu_device_range *r = &tables->ranges[i];
        uint64_t sys_mgt = (r->amd_settings & IOMMU_AMD_SYS_MGT_MASK) >> IOMMU_AMD_SYS_MGT_SHIFT;
        for (uint32_t id = r->first; id <= r->last; id++) {
            device_table[id * 4 + 1] = DOMAIN_ID | sys_mgt << DTE_SYS_MGT_SHIFT;
        }
    }
    for (uint32_t i = 0; i < tables->special_count; i++) {
        const struct iommu_special_device *s = &tables->specials[i];
        uint64_t sys_mgt = (s->amd_settings & IOMMU_AMD_SYS_MGT_MASK) >> IOMMU_AMD_SYS_MGT_SHIFT;
        device_table[s->rid * 4 + 1] = DOMAIN_ID | sys_mgt << DTE_SYS_MGT_SHIFT;
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

void amdvi_enable(uint64_t root) {
    device_table_build(iommu_tables_parse(), root);
    for (uint32_t i = 0; i < unit_count; i++) {
        unit_enable(&units[i]);
    }
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
