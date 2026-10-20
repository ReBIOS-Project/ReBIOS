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
#include <iommu/iommu_tables.h>
#include <iommu/vtd.h>
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

#define CAP_PLMR (1ull << 5)
#define CAP_PHMR (1ull << 6)
#define CAP_SAGAW_SHIFT 8
#define CAP_SAGAW_39 (1u << 1)
#define CAP_SAGAW_48 (1u << 2)
#define CAP_FRO_SHIFT 24
#define CAP_SSLPS_SHIFT 34
#define CAP_SSLPS_2M (1u << 0)
#define CAP_SSLPS_1G (1u << 1)
#define CAP_NFR_SHIFT 40
#define CAP_DWD (1ull << 54)
#define CAP_DRD (1ull << 55)

#define PCI_VENDOR_INTEL 0x8086
#define PCI_BASE_CLASS_DISPLAY 0x03

#define MODEL_SANDY_BRIDGE 0x2a
#define MODEL_SANDY_BRIDGE_X 0x2d

#define ECAP_C (1ull << 0)
#define ECAP_QI (1ull << 1)
#define ECAP_IR (1ull << 3)
#define ECAP_EIMER (1ull << 61)

// The same bits report each command's state in the global status register.
#define GCMD_TE (1u << 31)
#define GCMD_SRTP (1u << 30)
#define GCMD_QIE (1u << 26)
#define GCMD_IRE (1u << 25)
#define GCMD_SIRTP (1u << 24)
#define GCMD_CFI (1u << 23)
// The status bits that are not one-shot commands, and so carry over into a
// global command write.
#define GSTS_PERSISTENT 0x96ffffffu

#define PMEN_EPM (1u << 31)
#define PMEN_PRS (1u << 0)

#define FSTS_PFO (1u << 0)
#define FSTS_PPF (1u << 1)
#define FSTS_IQE (1u << 4)
#define FSTS_ITE (1u << 6)
#define FECTL_IM (1u << 31)

#define FAULT_F (1ull << 63)

// Invalidation descriptors, of 128 bits in legacy mode.
#define DESC_CONTEXT 0x1
#define DESC_IOTLB 0x2
#define DESC_IEC 0x4
#define DESC_WAIT 0x5
#define DESC_GLOBAL (1ull << 4)
#define DESC_IOTLB_DW (1ull << 6)
#define DESC_IOTLB_DR (1ull << 7)
#define DESC_WAIT_SW (1ull << 5)
#define DESC_WAIT_FN (1ull << 6)
#define DESC_WAIT_DATA_SHIFT 32
#define DESC_IEC_INDEX (1ull << 4)
#define DESC_IEC_INDEX_SHIFT 32

// The interrupt remapping table every unit shares: 2 << REMAP_SIZE entries
// of 16 bytes (D51397-019 rev 5.20, 9.9 and the IRTA register).
#define REMAP_SIZE 11
#define REMAP_ENTRIES (2u << REMAP_SIZE)
#define IRTE_P (1ull << 0)
#define IRTE_TM (1ull << 4)
#define IRTE_VECTOR_SHIFT 16
#define IRTE_DEST_SHIFT 40
// Remappable format messages (5.1.5.2) and I/O APIC entries (5.1.5.1).
#define MSI_ADDRESS 0xfee00000u
#define MSI_REMAPPABLE (1u << 4)
#define MSI_SHV (1u << 3)
#define RTE_REMAPPABLE (1ull << 48)
#define RTE_ACTIVE_LOW (1ull << 13)
#define RTE_LEVEL (1ull << 15)

// One page of 128-bit descriptors.
#define QUEUE_ENTRIES 256

#define ROOT_P (1ull << 0)
#define CONTEXT_P (1ull << 0)
#define CONTEXT_AW_39 1ull
#define CONTEXT_AW_48 2ull
#define CONTEXT_DID_SHIFT 8
// Domain 0 is reserved under caching mode.
#define DOMAIN_ID 1ull

#define ENTRY_ADDR_MASK 0x000ffffffffff000ull
#define TIMEOUT_NS 1000000000ull

struct vtd_unit {
    uint64_t base;
    uint64_t cap;
    uint64_t ecap;
    bool four_levels;
    bool keep_translating;
    uint64_t *root_table;
    uint64_t *context_table;
    uint64_t *queue;
    uint32_t queue_tail;
    uint32_t wait_done;
};

static struct vtd_unit *units;
static uint32_t unit_count;
// Over the units' invalidation queues, which any processor may fill.
static struct spinlock queue_lock;
static uint64_t *remap_table;
static uint64_t remap_used[REMAP_ENTRIES / 64];
static struct spinlock remap_lock;
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
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    *(volatile uint32_t *)(uintptr_t)(u->base + reg) = value;
}

static void reg_write64(const struct vtd_unit *u, uint32_t reg, uint64_t value) {
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
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
    __atomic_store_n(&u->wait_done, 0, __ATOMIC_RELAXED);
    queue_submit(u, DESC_WAIT | DESC_WAIT_SW | DESC_WAIT_FN | 1ull << DESC_WAIT_DATA_SHIFT,
                 (uint64_t)(uintptr_t)&u->wait_done);

    uint64_t deadline = time_ns() + TIMEOUT_NS;
    while (__atomic_load_n(&u->wait_done, __ATOMIC_ACQUIRE) != 1) {
        uint32_t status = reg_read32(u, REG_FSTS);
        if (status & (FSTS_IQE | FSTS_ITE)) {
            panic("VT-d unit at %#llx rejects an invalidation, fault status %#x\n",
                  (unsigned long long)u->base, status);
        }
        if (time_ns() > deadline) {
            panic("VT-d unit at %#llx does not complete invalidations\n",
                  (unsigned long long)u->base);
        }
        cpu_relax();
    }
}

static void invalidate_all(struct vtd_unit *u) {
    uint64_t drain = 0;
    if (u->cap & CAP_DRD) {
        drain |= DESC_IOTLB_DR;
    }
    if (u->cap & CAP_DWD) {
        drain |= DESC_IOTLB_DW;
    }
    spinlock_acquire(&queue_lock);
    queue_submit(u, DESC_CONTEXT | DESC_GLOBAL, 0);
    queue_submit(u, DESC_IOTLB | DESC_GLOBAL | drain, 0);
    queue_sync(u);
    spinlock_release(&queue_lock);
}

// Every unit's cached copy of the entry, or of all of them where index is -1.
static void remap_invalidate(int index) {
    uint64_t desc = DESC_IEC;
    if (index >= 0) {
        desc |= DESC_IEC_INDEX | (uint64_t)index << DESC_IEC_INDEX_SHIFT;
    }
    tables_flush_caches();
    spinlock_acquire(&queue_lock);
    for (uint32_t i = 0; i < unit_count; i++) {
        queue_submit(&units[i], desc, 0);
        queue_sync(&units[i]);
    }
    spinlock_release(&queue_lock);
}

// Intel's integrated graphics of these device ID families, behind a unit of
// their own, can hang if the translation the firmware left on stops under them,
// so it stays on (as Linux 7.3's quirk_igfx_skip_te_disable() has it).
static const uint8_t keep_translating_ids[] = {
    0x45, 0x46, 0x4c, 0x4e, 0x8a, 0x98, 0x9a, 0xa7, 0x7d,
};

static bool keep_translating(const struct iommu_tables *tables, uint32_t index, uint64_t cap) {
    if (tables->units[index].include_all || !(cap & (CAP_DRD | CAP_DWD))) {
        return false;
    }
    bool listed = false;
    for (uint32_t i = 0; i < tables->range_count; i++) {
        const struct iommu_device_range *r = &tables->ranges[i];
        if (r->unit != index) {
            continue;
        }
        for (uint32_t rid = r->first; rid <= r->last; rid++) {
            struct pci_addr a = {r->segment, (uint8_t)(rid >> 8), (uint8_t)(rid >> 3 & 0x1f),
                                 (uint8_t)(rid & 7)};
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

bool vtd_probe(const struct iommu_tables *tables, int *leaf_level) {
    unit_count = tables->unit_count;
    units = kmalloc(unit_count * sizeof(*units));
    int leaf = 2;
    // Sandy Bridge's units claim large pages they do not have, and go wrong
    // when a walk's length changes under DMA in flight (as Xen 4.22's
    // quirk_iommu_caps() has it): small pages only, then.
    struct cpu_signature s = cpu_signature();
    if (s.family == 6 && (s.model == MODEL_SANDY_BRIDGE || s.model == MODEL_SANDY_BRIDGE_X)) {
        leaf = 0;
    }

    for (uint32_t i = 0; i < unit_count; i++) {
        struct vtd_unit *u = &units[i];
        u->base = tables->units[i].base;
        u->cap = reg_read64(u, REG_CAP);
        u->ecap = reg_read64(u, REG_ECAP);
        uint32_t version = reg_read32(u, REG_VER);
        uint32_t sagaw = (u->cap >> CAP_SAGAW_SHIFT) & 0x1f;
        uint32_t sslps = (u->cap >> CAP_SSLPS_SHIFT) & 0xf;
        printf("VT-d unit %u at %#llx: version %u.%u, capabilities %#llx, extended %#llx\n", i,
               (unsigned long long)u->base, (version >> 4) & 0xf, version & 0xf,
               (unsigned long long)u->cap, (unsigned long long)u->ecap);

        if (!(u->ecap & ECAP_QI)) {
            printf("VT-d unit %u has no queued invalidation\n", i);
            return false;
        }
        if (sagaw & CAP_SAGAW_48) {
            u->four_levels = true;
        } else if (!(sagaw & CAP_SAGAW_39)) {
            printf("VT-d unit %u takes neither 3-level nor 4-level second-stage tables\n", i);
            return false;
        }
        int unit_leaf = (sslps & CAP_SSLPS_1G) ? 2 : (sslps & CAP_SSLPS_2M) ? 1 : 0;
        if (unit_leaf < leaf) {
            leaf = unit_leaf;
        }
        if (!(u->ecap & ECAP_C)) {
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
    if ((status & GCMD_TE) && u->keep_translating) {
        printf("VT-d unit at %#llx goes on translating for the graphics behind it\n",
               (unsigned long long)u->base);
    } else if (status & GCMD_TE) {
        global_command(u, GCMD_TE, false, "stop translating");
    }
    if (status & GCMD_IRE) {
        global_command(u, GCMD_IRE, false, "stop remapping interrupts");
    }
    if (status & GCMD_QIE) {
        uint64_t deadline = time_ns() + TIMEOUT_NS;
        while (reg_read64(u, REG_IQH) != reg_read64(u, REG_IQT)) {
            if (time_ns() > deadline) {
                panic("VT-d unit at %#llx does not drain the firmware's invalidations\n",
                      (unsigned long long)u->base);
            }
            cpu_relax();
        }
        global_command(u, GCMD_QIE, false, "disable queued invalidation");
    }
    if ((u->cap & (CAP_PLMR | CAP_PHMR)) && (reg_read32(u, REG_PMEN) & PMEN_EPM)) {
        reg_write32(u, REG_PMEN, 0);
        uint64_t deadline = time_ns() + TIMEOUT_NS;
        while (reg_read32(u, REG_PMEN) & PMEN_PRS) {
            if (time_ns() > deadline) {
                panic("VT-d unit at %#llx keeps its protected memory regions\n",
                      (unsigned long long)u->base);
            }
            cpu_relax();
        }
    }
    reg_write32(u, REG_FECTL, FECTL_IM);
}

static void unit_enable(struct vtd_unit *u, uint64_t pml4) {
    // A 3-level table is the 4-level one's first 512GiB.
    uint64_t table = pml4;
    if (!u->four_levels) {
        table = *(uint64_t *)(uintptr_t)pml4 & ENTRY_ADDR_MASK;
    }

    // Every device of every bus gets the one domain.
    u->context_table = page_alloc(1);
    for (int i = 0; i < 256; i++) {
        u->context_table[i * 2] = table | CONTEXT_P;
        u->context_table[i * 2 + 1] = (u->four_levels ? CONTEXT_AW_48 : CONTEXT_AW_39)
                                      | DOMAIN_ID << CONTEXT_DID_SHIFT;
    }
    u->root_table = page_alloc(1);
    for (int i = 0; i < 256; i++) {
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
    for (uint32_t i = 0; i < unit_count; i++) {
        struct vtd_unit *u = &units[i];
        unit_take_over(u);
        u->queue = page_alloc(1);
        u->queue_tail = 0;
        reg_write64(u, REG_IQT, 0);
        reg_write64(u, REG_IQA, (uint64_t)(uintptr_t)u->queue);
        global_command(u, GCMD_QIE, true, "enable queued invalidation");
        remap = remap && (u->ecap & ECAP_IR);
        // Remapping only in x2APIC mode would need every local APIC in it.
        if (u->ecap & ECAP_EIMER) {
            printf("VT-d unit at %#llx remaps interrupts in x2APIC mode only\n",
                   (unsigned long long)u->base);
            remap = false;
        }
    }
    if (!remap) {
        printf("VT-d: interrupts are not remapped, so the guest's devices can send any\n");
        return;
    }
    remap_table = page_alloc(REMAP_ENTRIES * 16 / PAGE_SIZE);
    tables_flush_caches();
    for (uint32_t i = 0; i < unit_count; i++) {
        struct vtd_unit *u = &units[i];
        reg_write64(u, REG_IRTA, (uint64_t)(uintptr_t)remap_table | REMAP_SIZE);
        global_command(u, GCMD_SIRTP, true, "take the interrupt remapping table");
    }
    remap_invalidate(-1);
    for (uint32_t i = 0; i < unit_count; i++) {
        global_command(&units[i], GCMD_IRE, true, "remap interrupts");
        global_command(&units[i], GCMD_CFI, false, "block compatibility format interrupts");
    }
}

bool vtd_remap_on(void) {
    return remap_table != NULL;
}

int vtd_remap_alloc(unsigned count) {
    int found = -1;
    spinlock_acquire(&remap_lock);
    for (unsigned base = 0; found < 0 && base + count <= REMAP_ENTRIES; base++) {
        unsigned n = 0;
        while (n < count && !(remap_used[(base + n) / 64] & 1ull << ((base + n) % 64))) {
            n++;
        }
        if (n == count) {
            found = (int)base;
        }
    }
    for (unsigned i = 0; found >= 0 && i < count; i++) {
        remap_used[(found + i) / 64] |= 1ull << ((found + i) % 64);
    }
    spinlock_release(&remap_lock);
    return found;
}

// Its entries not present first, so that nothing reaches a freed vector.
void vtd_remap_free(int index, unsigned count) {
    for (unsigned i = 0; i < count; i++) {
        __atomic_store_n(&remap_table[(index + i) * 2], 0, __ATOMIC_RELEASE);
        remap_invalidate(index + (int)i);
    }
    spinlock_acquire(&remap_lock);
    for (unsigned i = 0; i < count; i++) {
        remap_used[(index + i) / 64] &= ~(1ull << ((index + i) % 64));
    }
    spinlock_release(&remap_lock);
}

// Physical destination, fixed delivery, no source validation: the entries
// only ever name the hypervisor's vectors, whatever device sends them.
void vtd_remap_set(int index, uint32_t apic_id, uint8_t vector, bool level) {
    uint64_t entry = IRTE_P | (level ? IRTE_TM : 0) | (uint64_t)vector << IRTE_VECTOR_SHIFT
                     | (uint64_t)(apic_id & 0xff) << IRTE_DEST_SHIFT;
    if (__atomic_load_n(&remap_table[index * 2], __ATOMIC_ACQUIRE) == entry) {
        return;
    }
    remap_table[index * 2 + 1] = 0;
    __atomic_store_n(&remap_table[index * 2], entry, __ATOMIC_RELEASE);
    remap_invalidate(index);
}

void vtd_remap_message(int index, uint32_t *address, uint32_t *data) {
    *address = MSI_ADDRESS | ((uint32_t)index & 0x7fff) << 5 | MSI_REMAPPABLE | MSI_SHV
               | ((uint32_t)index >> 15 & 1) << 2;
    *data = 0;
}

uint64_t vtd_remap_rte(int index, uint8_t vector, bool level, bool active_low) {
    return vector | (uint64_t)((uint32_t)index & 0x7fff) << 49 | RTE_REMAPPABLE
           | (uint64_t)((uint32_t)index >> 15 & 1) << 11 | (level ? RTE_LEVEL : 0)
           | (active_low ? RTE_ACTIVE_LOW : 0);
}

void vtd_enable(uint64_t pml4) {
    for (uint32_t i = 0; i < unit_count; i++) {
        unit_enable(&units[i], pml4);
    }
}

void vtd_flush(void) {
    for (uint32_t i = 0; i < unit_count; i++) {
        invalidate_all(&units[i]);
    }
}

void vtd_report_faults(void) {
    for (uint32_t i = 0; i < unit_count; i++) {
        struct vtd_unit *u = &units[i];
        uint32_t status = reg_read32(u, REG_FSTS);
        if (!(status & (FSTS_PPF | FSTS_PFO))) {
            continue;
        }
        uint32_t first = ((u->cap >> CAP_FRO_SHIFT) & 0x3ff) * 16;
        uint32_t count = ((u->cap >> CAP_NFR_SHIFT) & 0xff) + 1;
        for (uint32_t n = 0; n < count; n++) {
            uint32_t record = first + n * 16;
            uint64_t high = reg_read64(u, record + 8);
            if (!(high & FAULT_F)) {
                continue;
            }
            uint64_t low = reg_read64(u, record);
            uint32_t source = high & 0xffff;
            printf("VT-d unit %u: DMA fault from %02x:%02x.%x at %#llx, reason %#x\n", i,
                   source >> 8, (source >> 3) & 0x1f, source & 7,
                   (unsigned long long)(low & ~0xfffull), (unsigned)((high >> 32) & 0xff));
            reg_write64(u, record + 8, FAULT_F);
        }
        if (status & FSTS_PFO) {
            printf("VT-d unit %u: more DMA faults were lost\n", i);
            reg_write32(u, REG_FSTS, FSTS_PFO);
        }
    }
}
