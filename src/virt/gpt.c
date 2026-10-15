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
#include <iommu/dma.h>
#include <lib/log.h>
#include <lib/spinlock.h>
#include <sys/mm.h>
#include <sys/smp.h>
#include <sys/x86.h>
#include <virt/gpt.h>
#include <virt/vcpu.h>

#define ENTRY_ADDR_MASK 0x000ffffffffff000ull

// EPT entry bits.
#define EPT_R (1ull << 0)
#define EPT_W (1ull << 1)
#define EPT_X (1ull << 2)
#define EPT_MEMTYPE_SHIFT 3
#define EPT_LARGE (1ull << 7)

// Nested paging uses the AMD64 page table format, where guest accesses count as
// user accesses.
#define NPT_P (1ull << 0)
#define NPT_RW (1ull << 1)
#define NPT_US (1ull << 2)
#define NPT_PWT (1ull << 3)
#define NPT_PCD (1ull << 4)
#define NPT_PS (1ull << 7)
#define NPT_PAT_4K (1ull << 7)
#define NPT_PAT_LARGE (1ull << 12)

uint64_t gpt_generation;

static bool use_ept;
static bool have_huge_pages;
static uint64_t *root;
static struct spinlock gpt_lock;

static uint64_t level_size(int level) {
    return 1ull << (12 + 9 * level);
}

static bool entry_present(uint64_t e) {
    if (use_ept) {
        return (e & (EPT_R | EPT_W | EPT_X)) != 0;
    }
    return (e & NPT_P) != 0;
}

static bool entry_large(uint64_t e, int level) {
    if (level == 0) {
        return true;
    }
    return (e & (use_ept ? EPT_LARGE : NPT_PS)) != 0;
}

static uint64_t table_entry(uint64_t table) {
    if (use_ept) {
        return table | EPT_R | EPT_W | EPT_X;
    }
    return table | NPT_P | NPT_RW | NPT_US;
}

static uint64_t leaf_entry(uint64_t hpa, uint32_t prot, uint8_t memtype, int level) {
    if (prot == 0) {
        return 0;
    }

    if (use_ept) {
        uint64_t e = hpa | (uint64_t)memtype << EPT_MEMTYPE_SHIFT;
        if (prot & GPT_R) {
            e |= EPT_R;
        }
        if (prot & GPT_W) {
            e |= EPT_R | EPT_W;
        }
        if (prot & GPT_X) {
            e |= EPT_X;
        }
        if (level > 0) {
            e |= EPT_LARGE;
        }
        return e;
    }

    uint64_t e = hpa | NPT_P | NPT_US;
    if (prot & GPT_W) {
        e |= NPT_RW;
    }
    if (level > 0) {
        e |= NPT_PS;
    }

    // Indices into HOST_PAT.
    uint64_t pat = level > 0 ? NPT_PAT_LARGE : NPT_PAT_4K;
    switch (memtype) {
        case MT_WB: {
            break;
        }
        case MT_WC: {
            e |= NPT_PWT;
            break;
        }
        case MT_WP: {
            e |= pat | NPT_PWT;
            break;
        }
        case MT_WT: {
            e |= pat | NPT_PCD | NPT_PWT;
            break;
        }
        default: {
            e |= NPT_PCD | NPT_PWT;
            break;
        }
    }
    return e;
}

static uint64_t leaf_addr(uint64_t e, int level) {
    return e & ENTRY_ADDR_MASK & ~(level_size(level) - 1);
}

// The access rights and memory type of a leaf, to split it into smaller ones.
static void leaf_decode(uint64_t e, int level, uint32_t *prot, uint8_t *memtype) {
    if (use_ept) {
        *prot = 0;
        if (e & EPT_R) {
            *prot |= GPT_R;
        }
        if (e & EPT_W) {
            *prot |= GPT_W;
        }
        if (e & EPT_X) {
            *prot |= GPT_X;
        }
        *memtype = (e >> EPT_MEMTYPE_SHIFT) & 7;
        return;
    }

    *prot = GPT_R | GPT_X;
    if (e & NPT_RW) {
        *prot |= GPT_W;
    }
    uint64_t pat = level > 0 ? NPT_PAT_LARGE : NPT_PAT_4K;
    unsigned index = ((e & pat) ? 4 : 0) | ((e & NPT_PCD) ? 2 : 0) | ((e & NPT_PWT) ? 1 : 0);
    *memtype = (HOST_PAT >> (index * 8)) & 7;
}

void gpt_init(bool ept, bool huge_pages) {
    use_ept = ept;
    have_huge_pages = huge_pages;
    root = page_alloc(1);
}

uint64_t gpt_root(void) {
    return (uint64_t)(uintptr_t)root;
}

// Returns the table that holds the entry for gpa at level, creating the tables
// on the way, and splitting large leaves into tables of the same mappings.
static uint64_t *table_for(uint64_t gpa, int level) {
    uint64_t *table = root;
    for (int l = 3; l > level; l--) {
        uint64_t *e = &table[(gpa >> (12 + 9 * l)) & 511];
        if (!entry_present(*e)) {
            uint64_t *next = page_alloc(1);
            *e = table_entry((uint64_t)(uintptr_t)next);
        } else if (entry_large(*e, l)) {
            uint32_t prot;
            uint8_t memtype;
            leaf_decode(*e, l, &prot, &memtype);
            uint64_t base = leaf_addr(*e, l);
            uint64_t *next = page_alloc(1);
            for (int i = 0; i < 512; i++) {
                next[i] = leaf_entry(base + i * level_size(l - 1), prot, memtype, l - 1);
            }
            *e = table_entry((uint64_t)(uintptr_t)next);
        }
        table = (uint64_t *)(uintptr_t)(*e & ENTRY_ADDR_MASK);
    }
    return table;
}

void gpt_map(uint64_t gpa, uint64_t hpa, uint64_t size, uint32_t prot, uint8_t memtype) {
    dma_map(gpa, hpa, size, prot);

    spinlock_acquire(&gpt_lock);

    while (size != 0) {
        int level = 0;
        for (int l = have_huge_pages ? 2 : 1; l > 0; l--) {
            uint64_t ls = level_size(l);
            if ((gpa & (ls - 1)) == 0 && (hpa & (ls - 1)) == 0 && size >= ls) {
                level = l;
                break;
            }
        }

        uint64_t *table = table_for(gpa, level);
        // A leaf that replaces a table leaks the table, as other CPUs may
        // still walk it until their next flush.
        table[(gpa >> (12 + 9 * level)) & 511] = leaf_entry(hpa, prot, memtype, level);

        gpa += level_size(level);
        hpa += level_size(level);
        size -= level_size(level);
    }

    spinlock_release(&gpt_lock);
}

bool gpt_translate(uint64_t gpa, uint64_t *hpa, uint32_t *prot) {
    uint64_t *table = root;
    for (int l = 3; l >= 0; l--) {
        uint64_t e = table[(gpa >> (12 + 9 * l)) & 511];
        if (!entry_present(e)) {
            return false;
        }
        if (entry_large(e, l)) {
            uint8_t memtype;
            leaf_decode(e, l, prot, &memtype);
            *hpa = leaf_addr(e, l) | (gpa & (level_size(l) - 1));
            return true;
        }
        table = (uint64_t *)(uintptr_t)(e & ENTRY_ADDR_MASK);
    }
    return false;
}

void gpt_flush(void) {
    __atomic_add_fetch(&gpt_generation, 1, __ATOMIC_SEQ_CST);
    for (uint32_t i = 0; i < smp_cpu_count(); i++) {
        vcpu_kick(smp_cpu(i));
    }
}
