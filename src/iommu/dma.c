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
#include <iommu/amdvi.h>
#include <iommu/dma.h>
#include <iommu/iommu_tables.h>
#include <iommu/vtd.h>
#include <lib/log.h>
#include <lib/spinlock.h>
#include <sys/mm.h>
#include <sys/x86.h>
#include <virt/gpt.h>

#define ENTRY_ADDR_MASK 0x000ffffffffff000ull
// What 4-level tables translate.
#define DMA_ADDRESS_LIMIT (1ull << 48)

// VT-d second-stage paging entries.
#define VTD_R (1ull << 0)
#define VTD_W (1ull << 1)
#define VTD_PS (1ull << 7)

// AMD-Vi host page table entries, where a directory entry names the level of
// the table it points to, counting from 1, and a leaf names none.
#define AMD_PR (1ull << 0)
#define AMD_NEXT_LEVEL_SHIFT 9
#define AMD_NEXT_LEVEL_MASK (7ull << 9)
#define AMD_IR (1ull << 61)
#define AMD_IW (1ull << 62)

static uint32_t kind;
// The largest level a leaf may be at: 0 for 4KiB pages only, 1 for 2MiB, 2 for
// 1GiB, as every IOMMU takes.
static int leaf_level_max;
static uint64_t *root;
static bool enabled;
static struct spinlock dma_lock;

static uint64_t level_size(int level) {
    return 1ull << (12 + 9 * level);
}

static bool entry_present(uint64_t e) {
    if (kind == IOMMU_KIND_AMD) {
        return (e & AMD_PR) != 0;
    }
    return (e & (VTD_R | VTD_W)) != 0;
}

static bool entry_leaf(uint64_t e, int level) {
    if (level == 0) {
        return true;
    }
    if (kind == IOMMU_KIND_AMD) {
        return (e & AMD_NEXT_LEVEL_MASK) == 0;
    }
    return (e & VTD_PS) != 0;
}

// The entry at level that points to the table below it.
static uint64_t table_entry(uint64_t table, int level) {
    if (kind == IOMMU_KIND_AMD) {
        return table | AMD_PR | (uint64_t)level << AMD_NEXT_LEVEL_SHIFT | AMD_IR | AMD_IW;
    }
    return table | VTD_R | VTD_W;
}

// As in EPT, write access comes with read access.
static uint64_t leaf_entry(uint64_t hpa, uint32_t prot, int level) {
    if (!(prot & (GPT_R | GPT_W))) {
        return 0;
    }
    bool write = (prot & GPT_W) != 0;
    if (kind == IOMMU_KIND_AMD) {
        return hpa | AMD_PR | AMD_IR | (write ? AMD_IW : 0);
    }
    return hpa | VTD_R | (write ? VTD_W : 0) | (level > 0 ? VTD_PS : 0);
}

static uint32_t leaf_prot(uint64_t e) {
    bool write = (e & (kind == IOMMU_KIND_AMD ? AMD_IW : VTD_W)) != 0;
    return write ? GPT_R | GPT_W : GPT_R;
}

static uint64_t leaf_addr(uint64_t e, int level) {
    return e & ENTRY_ADDR_MASK & ~(level_size(level) - 1);
}

// Returns the table that holds the entry for addr at level, creating the
// tables on the way, and splitting large leaves into tables of the same
// mappings.
static uint64_t *table_for(uint64_t addr, int level) {
    uint64_t *table = root;
    for (int l = 3; l > level; l--) {
        uint64_t *e = &table[(addr >> (12 + 9 * l)) & 511];
        if (!entry_present(*e)) {
            uint64_t *next = page_alloc(1);
            *e = table_entry((uint64_t)(uintptr_t)next, l);
        } else if (entry_leaf(*e, l)) {
            uint64_t base = leaf_addr(*e, l);
            uint32_t prot = leaf_prot(*e);
            uint64_t *next = page_alloc(1);
            for (int i = 0; i < 512; i++) {
                next[i] = leaf_entry(base + i * level_size(l - 1), prot, l - 1);
            }
            *e = table_entry((uint64_t)(uintptr_t)next, l);
        }
        table = (uint64_t *)(uintptr_t)(*e & ENTRY_ADDR_MASK);
    }
    return table;
}

static void map(uint64_t addr, uint64_t hpa, uint64_t size, uint32_t prot) {
    while (size != 0) {
        int level = 0;
        for (int l = leaf_level_max; l > 0; l--) {
            uint64_t ls = level_size(l);
            if ((addr & (ls - 1)) == 0 && (hpa & (ls - 1)) == 0 && size >= ls) {
                level = l;
                break;
            }
        }

        uint64_t *table = table_for(addr, level);
        // A leaf that replaces a table leaks the table, as the IOMMUs may still
        // walk it until their caches are flushed.
        table[(addr >> (12 + 9 * level)) & 511] = leaf_entry(hpa, prot, level);

        addr += level_size(level);
        hpa += level_size(level);
        size -= level_size(level);
    }
}

static void flush(void) {
    if (kind == IOMMU_KIND_AMD) {
        amdvi_flush();
    } else {
        vtd_flush();
    }
}

void dma_init(void) {
    const struct iommu_tables *tables = iommu_tables_parse();
    kind = tables->kind;

    bool usable = false;
    if (kind == IOMMU_KIND_VTD) {
        usable = vtd_probe(tables, &leaf_level_max);
    } else if (kind == IOMMU_KIND_AMD) {
        usable = amdvi_probe(tables, &leaf_level_max);
    }
    if (!usable) {
        panic("No IOMMU can remap DMA\n");
    }
    if (tables->truncated) {
        printf("Devices of the IOMMUs the tables leave out keep untranslated DMA\n");
    }
    root = page_alloc(1);
}

void dma_map(uint64_t gpa, uint64_t hpa, uint64_t size, uint32_t prot) {
    if (root == NULL) {
        panic("Guest memory mapped before the IOMMU tables\n");
    }
    spinlock_acquire(&dma_lock);
    map(gpa, hpa, size, prot);
    if (enabled) {
        flush();
    }
    spinlock_release(&dma_lock);
}

// One domain serves every device, so memory the firmware needs mapped for some
// devices is mapped for all, one to one, as the guest has it too.
static void reserved_regions_apply(const struct iommu_tables *tables) {
    for (uint32_t i = 0; i < tables->reserved_count; i++) {
        const struct iommu_reserved_region *r = &tables->reserved[i];
        if (r->last >= DMA_ADDRESS_LIMIT) {
            printf("IOMMU region %#llx-%#llx is beyond the tables' reach\n",
                   (unsigned long long)r->base, (unsigned long long)r->last);
            continue;
        }
        uint64_t base = r->base & ~(uint64_t)(PAGE_SIZE - 1);
        uint64_t last = r->last | (PAGE_SIZE - 1);
        uint32_t prot = 0;
        if (r->exclusion || r->writable) {
            prot = GPT_R | GPT_W;
        } else if (r->readable) {
            prot = GPT_R;
        }
        map(base, base, last - base + 1, prot);
    }
}

void dma_enable(void) {
    const struct iommu_tables *tables = iommu_tables_parse();

    spinlock_acquire(&dma_lock);
    reserved_regions_apply(tables);
    if (kind == IOMMU_KIND_AMD) {
        amdvi_enable((uint64_t)(uintptr_t)root);
    } else {
        vtd_enable((uint64_t)(uintptr_t)root);
    }
    enabled = true;
    spinlock_release(&dma_lock);
    printf("Device DMA remapped\n");
}

void dma_report_faults(void) {
    if (!enabled) {
        return;
    }
    if (kind == IOMMU_KIND_AMD) {
        amdvi_report_faults();
    } else {
        vtd_report_faults();
    }
}
