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
#include <iommu/vtd.h>
#include <hw/pci_cfg.h>
#include <iommu/iommu_tables.h>
#include <lib/heap.h>
#include <lib/log.h>
#include <lib/spinlock.h>
#include <lib/time.h>
#include <sys/mm.h>
#include <sys/x86.h>

#define REG_VER 0x00
#define REG_CAP 0x08
#define REG_ECAP 0x10
#define REG_GCMD 0x18
#define REG_GSTS 0x1c
#define REG_RTADDR 0x20
#define REG_FSTS 0x34
#define REG_FECTL 0x38
#define REG_PMEN 0x64
#define REG_IQH 0x80
#define REG_IQT 0x88
#define REG_IQA 0x90
#define REG_IRTA 0xb8

#define CAP_PLMR ((uint64_t)1 << 5)
#define CAP_PHMR ((uint64_t)1 << 6)
#define CAP_SAGAW_SHIFT 8
#define CAP_SAGAW_39 ((uint32_t)1 << 1)
#define CAP_SAGAW_48 ((uint32_t)1 << 2)
#define CAP_FRO_SHIFT 24
#define CAP_SSLPS_SHIFT 34
#define CAP_SSLPS_2M ((uint32_t)1 << 0)
#define CAP_SSLPS_1G ((uint32_t)1 << 1)
#define CAP_NFR_SHIFT 40
#define CAP_DWD ((uint64_t)1 << 54)
#define CAP_DRD ((uint64_t)1 << 55)
#define CAP_PI ((uint64_t)1 << 59)

#define PCI_VENDOR_INTEL 0x8086
#define PCI_BASE_CLASS_DISPLAY 0x03

#define MODEL_SANDY_BRIDGE 0x2a
#define MODEL_SANDY_BRIDGE_X 0x2d

#define ECAP_C ((uint64_t)1 << 0)
#define ECAP_QI ((uint64_t)1 << 1)
#define ECAP_IR ((uint64_t)1 << 3)
#define ECAP_EIM ((uint64_t)1 << 4)
#define ECAP_EIMER ((uint64_t)1 << 61)

// The same bits report each command's state in the global status register.
#define GCMD_TE ((uint32_t)1 << 31)
#define GCMD_SRTP ((uint32_t)1 << 30)
#define GCMD_QIE ((uint32_t)1 << 26)
#define GCMD_IRE ((uint32_t)1 << 25)
#define GCMD_SIRTP ((uint32_t)1 << 24)
#define GCMD_CFI ((uint32_t)1 << 23)
// The status bits that are not one-shot commands, and so carry over into a
// global command write.
#define GSTS_PERSISTENT ((uint32_t)0x96ffffff)

#define PMEN_EPM ((uint32_t)1 << 31)
#define PMEN_PRS ((uint32_t)1 << 0)

#define FSTS_PFO ((uint32_t)1 << 0)
#define FSTS_PPF ((uint32_t)1 << 1)
#define FSTS_IQE ((uint32_t)1 << 4)
#define FSTS_ITE ((uint32_t)1 << 6)
#define FECTL_IM ((uint32_t)1 << 31)

#define FAULT_F ((uint64_t)1 << 63)

// Invalidation descriptors, of 128 bits in legacy mode.
#define DESC_CONTEXT 0x1
#define DESC_IOTLB 0x2
#define DESC_IEC 0x4
#define DESC_WAIT 0x5
#define DESC_GLOBAL ((uint64_t)1 << 4)
#define DESC_IOTLB_DW ((uint64_t)1 << 6)
#define DESC_IOTLB_DR ((uint64_t)1 << 7)
#define DESC_WAIT_SW ((uint64_t)1 << 5)
#define DESC_WAIT_FN ((uint64_t)1 << 6)
#define DESC_WAIT_DATA_SHIFT 32
#define DESC_IEC_INDEX ((uint64_t)1 << 4)
#define DESC_IEC_INDEX_SHIFT 32

// The interrupt remapping table every unit shares: 2 << REMAP_SIZE entries of
// 16 bytes (D51397-019 rev 5.20, 9.9 and the IRTA register).
#define REMAP_SIZE 11
#define REMAP_ENTRIES ((uint32_t)2 << REMAP_SIZE)
#define IRTA_EIME ((uint64_t)1 << 11)
#define IRTE_P ((uint64_t)1 << 0)
#define IRTE_TM ((uint64_t)1 << 4)
#define IRTE_IM ((uint64_t)1 << 15)
#define IRTE_VECTOR_SHIFT 16
#define IRTE_DEST_SHIFT 40
// Remappable format messages (5.1.5.2) and I/O APIC entries (5.1.5.1).
#define MSI_ADDRESS ((uint32_t)0xfee00000)
#define MSI_REMAPPABLE ((uint32_t)1 << 4)
#define MSI_SHV ((uint32_t)1 << 3)
#define RTE_REMAPPABLE ((uint64_t)1 << 48)
#define RTE_ACTIVE_LOW ((uint64_t)1 << 13)
#define RTE_LEVEL ((uint64_t)1 << 15)

// One page of 128-bit descriptors.
#define QUEUE_ENTRIES 256

#define ROOT_P ((uint64_t)1 << 0)
#define CONTEXT_P ((uint64_t)1 << 0)
#define CONTEXT_AW_39 ((uint64_t)1)
#define CONTEXT_AW_48 ((uint64_t)2)
#define CONTEXT_DID_SHIFT 8
// Domain 0 is reserved under caching mode.
#define DOMAIN_ID ((uint64_t)1)

#define ENTRY_ADDR_MASK ((uint64_t)0x000ffffffffff000)
#define TIMEOUT_NS ((uint64_t)1000000000)

struct vtd_unit {
    uintptr_t base;
    uint64_t cap;
    uint64_t ecap;
    bool four_levels;
    bool keep_translating;
    uint64_t *root_table;
    uint64_t *context_table;
    uint64_t *queue;
    uint32_t queue_tail;
    _Atomic(uint32_t) wait_done;
};

static struct vtd_unit *units;
static size_t unit_count;
// Over the units' invalidation queues, which any processor may fill.
static struct spinlock queue_lock;
// The table the units remap through, the first of two where they can remap in
// x2APIC format too (D51397-019 rev. 5.20, 9.9), the second in that format.
static uint64_t *remap_tables[2];
static uint64_t *remap_table;
static bool x2apic_capable;
static bool x2apic_format;
static uint64_t remap_used[REMAP_ENTRIES / 64];
// Over the entries, and over which table the units use.
static struct spinlock remap_lock;
// Every unit posts interrupts to descriptors in memory too (D51397-019 rev.
// 5.20, 5.2).
static bool posting;
// Whether every unit snoops the processor's caches when reading the tables.
static bool coherent = true;

static uint32_t reg_read32(const struct vtd_unit *u, uint32_t reg) {
    return *(volatile uint32_t *)(uintptr_t)(u->base + reg);
}

static uint64_t reg_read64(const struct vtd_unit *u, uint32_t reg) {
    return *(volatile uint64_t *)(uintptr_t)(u->base + reg);
}

// Fenced, as a write may hand the unit tables or descriptors just written.
static void reg_write32(const struct vtd_unit *u, uint32_t reg, uint32_t value) {
    atomic_thread_fence(memory_order_seq_cst);
    *(volatile uint32_t *)(uintptr_t)(u->base + reg) = value;
}

static void reg_write64(const struct vtd_unit *u, uint32_t reg, uint64_t value) {
    atomic_thread_fence(memory_order_seq_cst);
    *(volatile uint64_t *)(uintptr_t)(u->base + reg) = value;
}

static void wait_status(const struct vtd_unit *u, uint32_t bit, bool set, const char *what) {
    uint64_t deadline = time_ns() + TIMEOUT_NS;
    while (((reg_read32(u, REG_GSTS) & bit) != 0) != set) {
        if (time_ns() > deadline) {
            panic("VT-d unit at %#llx does not %s\n", (unsigned long long)u->base, what);
        }
        cpu_relax();
    }
}

static void global_command(const struct vtd_unit *u, uint32_t bit, bool set, const char *what) {
    uint32_t status = reg_read32(u, REG_GSTS) & GSTS_PERSISTENT;
    reg_write32(u, REG_GCMD, set ? status | bit : status & ~bit);
    wait_status(u, bit, set, what);
}

static void tables_flush_caches(void) {
    if (!coherent) {
        wbinvd();
    }
}

static void queue_submit(struct vtd_unit *u, uint64_t low, uint64_t high) {
    uint64_t *desc = &u->queue[u->queue_tail * 2];
    desc[0] = low;
    desc[1] = high;
    u->queue_tail = (u->queue_tail + 1) % QUEUE_ENTRIES;
    tables_flush_caches();
    reg_write64(u, REG_IQT, (uint64_t)u->queue_tail << 4);
}

// Waits for everything submitted so far to complete.
static void queue_sync(struct vtd_unit *u) {
    atomic_store_explicit(&u->wait_done, 0, memory_order_relaxed);
    queue_submit(
        u, DESC_WAIT | DESC_WAIT_SW | DESC_WAIT_FN | (uint64_t)1 << DESC_WAIT_DATA_SHIFT,
        (uint64_t)(uintptr_t)&u->wait_done
    );

    uint64_t deadline = time_ns() + TIMEOUT_NS;
    while (atomic_load_explicit(&u->wait_done, memory_order_acquire) != 1) {
        uint32_t status = reg_read32(u, REG_FSTS);
        if ((status & (FSTS_IQE | FSTS_ITE)) != 0) {
            panic(
                "VT-d unit at %#llx rejects an invalidation, fault status %#x\n",
                (unsigned long long)u->base, status
            );
        }
        if (time_ns() > deadline) {
            panic(
                "VT-d unit at %#llx does not complete invalidations\n", (unsigned long long)u->base
            );
        }
        cpu_relax();
    }
}

static void invalidate_all(struct vtd_unit *u) {
    uint64_t drain = 0;
    if ((u->cap & CAP_DRD) != 0) {
        drain |= DESC_IOTLB_DR;
    }
    if ((u->cap & CAP_DWD) != 0) {
        drain |= DESC_IOTLB_DW;
    }
    spinlock_acquire(&queue_lock);
    queue_submit(u, DESC_CONTEXT | DESC_GLOBAL, 0);
    queue_submit(u, DESC_IOTLB | DESC_GLOBAL | drain, 0);
    queue_sync(u);
    spinlock_release(&queue_lock);
}

// Every unit's cached copies of the entries that an interrupt entry cache
// invalidation descriptor names.
static void remap_invalidate_desc(uint64_t desc) {
    tables_flush_caches();
    spinlock_acquire(&queue_lock);
    for (size_t i = 0; i < unit_count; i++) {
        queue_submit(&units[i], desc, 0);
        queue_sync(&units[i]);
    }
    spinlock_release(&queue_lock);
}

static void remap_invalidate(uint16_t index) {
    remap_invalidate_desc(DESC_IEC | DESC_IEC_INDEX | (uint64_t)index << DESC_IEC_INDEX_SHIFT);
}

static void remap_invalidate_all(void) {
    remap_invalidate_desc(DESC_IEC);
}

// Intel's integrated graphics of these device ID families, behind a unit of
// their own, can hang if the translation the firmware left on stops under
// them, so it stays on (as Linux 7.3's quirk_igfx_skip_te_disable() has it).
static const uint8_t keep_translating_ids[] = {
    0x45, 0x46, 0x4c, 0x4e, 0x8a, 0x98, 0x9a, 0xa7, 0x7d
};

static bool keep_translating(const struct iommu_tables *tables, uint32_t index, uint64_t cap) {
    if (tables->units[index].include_all || (cap & (CAP_DRD | CAP_DWD)) == 0) {
        return false;
    }
    bool listed = false;
    for (size_t i = 0; i < tables->range_count; i++) {
        const struct iommu_device_range *r = &tables->ranges[i];
        if (r->unit != index) {
            continue;
        }
        for (size_t rid = r->first; rid <= r->last; rid++) {
            struct pci_addr a = {
                r->segment, (uint8_t)(rid >> 8), (uint8_t)(rid >> 3 & 0x1f), (uint8_t)(rid & 7)
            };
            uint32_t id = pci_cfg_read(a, 0, 4);
            if (id == UINT32_MAX) {
                continue;
            }
            if (pci_cfg_read(a, 0x0b, 1) != PCI_BASE_CLASS_DISPLAY) {
                return false;
            }
            for (size_t j = 0; j < sizeof(keep_translating_ids); j++) {
                if ((id & 0xffff) == PCI_VENDOR_INTEL && id >> 24 == keep_translating_ids[j]) {
                    listed = true;
                }
            }
        }
    }
    return listed;
}

bool vtd_probe(const struct iommu_tables *tables, size_t *leaf_level) {
    unit_count = tables->unit_count;
    units = kmalloc(unit_count * sizeof(struct vtd_unit));
    size_t leaf = 2;
    // Sandy Bridge's units claim large pages they do not have, and go wrong
    // when a walk's length changes under DMA in flight (as Xen 4.22's
    // quirk_iommu_caps() has it): small pages only, then.
    struct cpu_signature s = cpu_signature();
    if (s.family == 6 && (s.model == MODEL_SANDY_BRIDGE || s.model == MODEL_SANDY_BRIDGE_X)) {
        leaf = 0;
    }

    for (size_t i = 0; i < unit_count; i++) {
        struct vtd_unit *u = &units[i];
        u->base = tables->units[i].base;
        if (u->base == 0) {
            printf("VT-d unit %zu has no registers: the DMAR table puts it at address 0\n", i);
            return false;
        }
        u->cap = reg_read64(u, REG_CAP);
        u->ecap = reg_read64(u, REG_ECAP);
        // No unit answers there, which Linux 7.3 takes for a broken DMAR table
        // (dmar_validate_one_drhd()).
        if (u->cap == UINT64_MAX && u->ecap == UINT64_MAX) {
            printf(
                "VT-d unit %zu at %#llx reads all ones: the DMAR table puts it where none is\n",
                i, (unsigned long long)u->base
            );
            return false;
        }
        uint32_t version = reg_read32(u, REG_VER);
        uint32_t sagaw = (u->cap >> CAP_SAGAW_SHIFT) & 0x1f;
        uint32_t sslps = (u->cap >> CAP_SSLPS_SHIFT) & 0xf;
        printf(
            "VT-d unit %zu at %#llx: version %u.%u, capabilities %#llx, extended %#llx\n", i,
            (unsigned long long)u->base, (version >> 4) & 0xf, version & 0xf,
            (unsigned long long)u->cap, (unsigned long long)u->ecap
        );

        if ((u->ecap & ECAP_QI) == 0) {
            printf("VT-d unit %zu has no queued invalidation\n", i);
            return false;
        }
        if ((sagaw & CAP_SAGAW_48) != 0) {
            u->four_levels = true;
        } else if ((sagaw & CAP_SAGAW_39) == 0) {
            printf("VT-d unit %zu takes neither 3-level nor 4-level second-stage tables\n", i);
            return false;
        }
        size_t unit_leaf = (sslps & CAP_SSLPS_1G) != 0 ? 2 : (sslps & CAP_SSLPS_2M) != 0 ? 1 : 0;
        if (unit_leaf < leaf) {
            leaf = unit_leaf;
        }
        if ((u->ecap & ECAP_C) == 0) {
            coherent = false;
        }
        u->keep_translating = keep_translating(tables, i, u->cap);
    }

    *leaf_level = leaf;
    return true;
}

// Firmware may have left translation, interrupt remapping, its invalidation
// queue, or protected memory regions on, all of which would stand in the way.
static void unit_take_over(struct vtd_unit *u) {
    uint32_t status = reg_read32(u, REG_GSTS);
    if ((status & GCMD_TE) != 0 && u->keep_translating) {
        printf(
            "VT-d unit at %#llx goes on translating for the graphics behind it\n",
            (unsigned long long)u->base
        );
    } else if ((status & GCMD_TE) != 0) {
        global_command(u, GCMD_TE, false, "stop translating");
    }
    if ((status & GCMD_IRE) != 0) {
        global_command(u, GCMD_IRE, false, "stop remapping interrupts");
    }
    if ((status & GCMD_QIE) != 0) {
        uint64_t deadline = time_ns() + TIMEOUT_NS;
        while (reg_read64(u, REG_IQH) != reg_read64(u, REG_IQT)) {
            if (time_ns() > deadline) {
                panic(
                    "VT-d unit at %#llx does not drain the firmware's invalidations\n",
                    (unsigned long long)u->base
                );
            }
            cpu_relax();
        }
        global_command(u, GCMD_QIE, false, "disable queued invalidation");
    }
    if ((u->cap & (CAP_PLMR | CAP_PHMR)) != 0 && (reg_read32(u, REG_PMEN) & PMEN_EPM) != 0) {
        reg_write32(u, REG_PMEN, 0);
        uint64_t deadline = time_ns() + TIMEOUT_NS;
        while ((reg_read32(u, REG_PMEN) & PMEN_PRS) != 0) {
            if (time_ns() > deadline) {
                panic(
                    "VT-d unit at %#llx keeps its protected memory regions\n",
                    (unsigned long long)u->base
                );
            }
            cpu_relax();
        }
    }
    reg_write32(u, REG_FECTL, FECTL_IM);
}

static void unit_enable(struct vtd_unit *u, uintptr_t pml4) {
    // A 3-level table is the 4-level one's first 512GiB.
    uintptr_t table = pml4;
    if (!u->four_levels) {
        table = *(uint64_t *)(uintptr_t)pml4 & ENTRY_ADDR_MASK;
    }

    // Every device of every bus gets the one domain.
    u->context_table = page_alloc(1);
    for (size_t i = 0; i < 256; i++) {
        u->context_table[i * 2] = table | CONTEXT_P;
        u->context_table[i * 2 + 1] = (u->four_levels ? CONTEXT_AW_48 : CONTEXT_AW_39)
                                    | DOMAIN_ID << CONTEXT_DID_SHIFT;
    }
    u->root_table = page_alloc(1);
    for (size_t i = 0; i < 256; i++) {
        u->root_table[i * 2] = (uint64_t)(uintptr_t)u->context_table | ROOT_P;
    }
    tables_flush_caches();

    reg_write64(u, REG_RTADDR, (uint64_t)(uintptr_t)u->root_table);
    global_command(u, GCMD_SRTP, true, "take the root table");
    invalidate_all(u);
    global_command(u, GCMD_TE, true, "start translating");
}

// The units the hypervisor's, with queued invalidation, and interrupt
// remapping on where every unit has it, before any source is programmed for
// it: compatibility format interrupts are blocked from then on, and the table
// starts out with no entry present.
void vtd_start(void) {
    bool remap = true;
    bool post = true;
    bool eim = true;
    for (size_t i = 0; i < unit_count; i++) {
        struct vtd_unit *u = &units[i];
        unit_take_over(u);
        u->queue = page_alloc(1);
        u->queue_tail = 0;
        reg_write64(u, REG_IQT, 0);
        reg_write64(u, REG_IQA, (uint64_t)(uintptr_t)u->queue);
        global_command(u, GCMD_QIE, true, "enable queued invalidation");
        remap = remap && (u->ecap & ECAP_IR) != 0;
        post = post && (u->cap & CAP_PI) != 0;
        eim = eim && (u->ecap & ECAP_EIM) != 0;
        // Remapping only in x2APIC mode would need every local APIC in it.
        if ((u->ecap & ECAP_EIMER) != 0) {
            printf(
                "VT-d unit at %#llx remaps interrupts in x2APIC mode only\n",
                (unsigned long long)u->base
            );
            remap = false;
        }
    }
    if (!remap) {
        printf("VT-d: interrupts are not remapped, so the guest's devices can send any\n");
        return;
    }
    remap_table = page_alloc(REMAP_ENTRIES * 16 / PAGE_SIZE);
    remap_tables[0] = remap_table;
    if (post && eim) {
        remap_tables[1] = page_alloc(REMAP_ENTRIES * 16 / PAGE_SIZE);
        x2apic_capable = true;
    }
    tables_flush_caches();
    for (size_t i = 0; i < unit_count; i++) {
        struct vtd_unit *u = &units[i];
        reg_write64(u, REG_IRTA, (uint64_t)(uintptr_t)remap_table | REMAP_SIZE);
        global_command(u, GCMD_SIRTP, true, "take the interrupt remapping table");
    }
    remap_invalidate_all();
    for (size_t i = 0; i < unit_count; i++) {
        global_command(&units[i], GCMD_IRE, true, "remap interrupts");
        global_command(&units[i], GCMD_CFI, false, "block compatibility format interrupts");
    }
    posting = post;
}

bool vtd_remap_on(void) {
    return remap_table != NULL;
}

int vtd_remap_alloc(size_t count) {
    int found = -1;
    spinlock_acquire(&remap_lock);
    for (size_t base = 0; found < 0 && base + count <= REMAP_ENTRIES; base++) {
        size_t n = 0;
        while (n < count
            && (remap_used[(base + n) / 64] & (uint64_t)1 << ((base + n) % 64)) == 0) {
            n++;
        }
        if (n == count) {
            found = (int)base;
        }
    }
    for (size_t i = 0; found >= 0 && i < count; i++) {
        remap_used[(found + i) / 64] |= (uint64_t)1 << ((found + i) % 64);
    }
    spinlock_release(&remap_lock);
    return found;
}

// Its entries not present first, so that nothing reaches a freed vector.
void vtd_remap_free(uint16_t index, size_t count) {
    spinlock_acquire(&remap_lock);
    for (size_t i = 0; i < count; i++) {
        atomic_store_explicit(
            (_Atomic(uint64_t) *)&remap_table[(index + i) * 2], 0, memory_order_release
        );
        remap_invalidate((uint16_t)(index + i));
    }
    for (size_t i = 0; i < count; i++) {
        remap_used[(index + i) / 64] &= ~((uint64_t)1 << ((index + i) % 64));
    }
    spinlock_release(&remap_lock);
}

// Both halves of an entry at once, as the units read it (D51397-019 rev. 5.20,
// 5.1.4).
static void irte_store(uint16_t index, uint64_t low, uint64_t high) {
    store16(&remap_table[index * 2], low, high);
}

static void irte_write(uint16_t index, uint64_t low, uint64_t high) {
    spinlock_acquire(&remap_lock);
    if (remap_table[index * 2] != low || remap_table[index * 2 + 1] != high) {
        irte_store(index, low, high);
        remap_invalidate(index);
    }
    spinlock_release(&remap_lock);
}

// xAPIC format's 8-bit destination at bits 47:40, or x2APIC format's 32 bits
// at 63:32 (D51397-019 rev. 5.20, 9.9).
static uint64_t irte_destination(uint32_t apic_id, bool x2apic) {
    return x2apic ? (uint64_t)apic_id << 32 : (uint64_t)(apic_id & 0xff) << IRTE_DEST_SHIFT;
}

// Physical destination, fixed delivery, no source validation: the entries only
// ever name the hypervisor's vectors, whatever device sends them.
void vtd_remap_set(uint16_t index, uint32_t apic_id, uint8_t vector, bool level) {
    uint64_t entry = IRTE_P | (level ? IRTE_TM : 0) | (uint64_t)vector << IRTE_VECTOR_SHIFT
                   | irte_destination(apic_id, x2apic_format);
    irte_write(index, entry, 0);
}

bool vtd_posting(void) {
    return posting;
}

// Posted format (D51397-019 rev. 5.20, 9.10), not urgent: the unit sets the
// vector's bit in the descriptor's requests and notifies the processor the
// descriptor names, whatever device sends the message.
void vtd_remap_post(uint16_t index, uint8_t vector, uintptr_t descriptor) {
    uint64_t low = IRTE_P | IRTE_IM | (uint64_t)vector << IRTE_VECTOR_SHIFT
                 | ((uint64_t)descriptor & 0xffffffc0) << 32;
    irte_write(index, low, (uint64_t)descriptor & 0xffffffff00000000);
}

bool vtd_remap_x2apic_capable(void) {
    return x2apic_capable;
}

bool vtd_remap_x2apic_on(void) {
    return x2apic_format;
}

// Into the other table, its remapped entries' destinations in the other
// format, which the units take with EIME at once: a table that remaps alike,
// as the units may use either for interrupts in flight until they no longer
// cache the old (D51397-019 rev. 5.20, 6.7).
void vtd_remap_x2apic(bool on) {
    spinlock_acquire(&remap_lock);
    if (!x2apic_capable || on == x2apic_format) {
        spinlock_release(&remap_lock);
        return;
    }
    uint64_t *next = remap_tables[on ? 1 : 0];
    for (size_t i = 0; i < REMAP_ENTRIES; i++) {
        uint64_t low = remap_table[i * 2];
        if ((low & (IRTE_P | IRTE_IM)) == IRTE_P) {
            uint32_t dest = x2apic_format
                          ? (uint32_t)(low >> 32)
                          : (uint32_t)(low >> IRTE_DEST_SHIFT & 0xff);
            low = (low & 0xffffffff) | irte_destination(dest, on);
        }
        next[i * 2] = low;
        next[i * 2 + 1] = remap_table[i * 2 + 1];
    }
    tables_flush_caches();
    for (size_t i = 0; i < unit_count; i++) {
        struct vtd_unit *u = &units[i];
        reg_write64(u, REG_IRTA, (uint64_t)(uintptr_t)next | (on ? IRTA_EIME : 0) | REMAP_SIZE);
        global_command(u, GCMD_SIRTP, true, "take the interrupt remapping table");
    }
    remap_table = next;
    x2apic_format = on;
    remap_invalidate_all();
    spinlock_release(&remap_lock);
}

void vtd_remap_message(uint16_t index, uint32_t *address, uint32_t *data) {
    *address = MSI_ADDRESS | ((uint32_t)index & 0x7fff) << 5 | MSI_REMAPPABLE | MSI_SHV
             | ((uint32_t)index >> 15 & 1) << 2;
    *data = 0;
}

uint64_t vtd_remap_rte(uint16_t index, uint8_t vector, bool level, bool active_low) {
    return vector | (uint64_t)((uint32_t)index & 0x7fff) << 49 | RTE_REMAPPABLE
         | (uint64_t)((uint32_t)index >> 15 & 1) << 11 | (level ? RTE_LEVEL : 0)
         | (active_low ? RTE_ACTIVE_LOW : 0);
}

void vtd_enable(uintptr_t pml4) {
    for (size_t i = 0; i < unit_count; i++) {
        unit_enable(&units[i], pml4);
    }
}

void vtd_flush(void) {
    for (size_t i = 0; i < unit_count; i++) {
        invalidate_all(&units[i]);
    }
}

void vtd_report_faults(void) {
    for (size_t i = 0; i < unit_count; i++) {
        struct vtd_unit *u = &units[i];
        uint32_t status = reg_read32(u, REG_FSTS);
        if ((status & (FSTS_PPF | FSTS_PFO)) == 0) {
            continue;
        }
        uint32_t first = ((u->cap >> CAP_FRO_SHIFT) & 0x3ff) * 16;
        size_t count = ((u->cap >> CAP_NFR_SHIFT) & 0xff) + 1;
        for (size_t n = 0; n < count; n++) {
            uint32_t record = first + n * 16;
            uint64_t high = reg_read64(u, record + 8);
            if ((high & FAULT_F) == 0) {
                continue;
            }
            uint64_t low = reg_read64(u, record);
            uint32_t source = high & 0xffff;
            printf(
                "VT-d unit %zu: DMA fault from %02x:%02x.%x at %#llx, reason %#x\n", i,
                source >> 8, (source >> 3) & 0x1f, source & 7,
                (unsigned long long)(low & ~(uint64_t)0xfff), (unsigned int)((high >> 32) & 0xff)
            );
            reg_write64(u, record + 8, FAULT_F);
        }
        if ((status & FSTS_PFO) != 0) {
            printf("VT-d unit %zu: more DMA faults were lost\n", i);
            reg_write32(u, REG_FSTS, FSTS_PFO);
        }
    }
}
