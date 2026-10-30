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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <virt/gpt.h>
#include <iommu/dma.h>
#include <lib/log.h>
#include <lib/spinlock.h>
#include <sys/mm.h>
#include <sys/smp.h>
#include <sys/x86.h>
#include <virt/vcpu.h>

#define ENTRY_ADDR_MASK ((uint64_t)0x000ffffffffff000)

// EPT entry bits.
#define EPT_R ((uint64_t)1 << 0)
#define EPT_W ((uint64_t)1 << 1)
#define EPT_X ((uint64_t)1 << 2)
#define EPT_MEMTYPE_SHIFT 3
#define EPT_LARGE ((uint64_t)1 << 7)
// Where the EPT pointer enables it (SDM 325462-093 Vol. 3C, 31.3.5).
#define EPT_DIRTY ((uint64_t)1 << 9)

// Nested paging uses the AMD64 page table format, where guest accesses count
// as user accesses.
#define NPT_P ((uint64_t)1 << 0)
#define NPT_RW ((uint64_t)1 << 1)
#define NPT_US ((uint64_t)1 << 2)
#define NPT_PWT ((uint64_t)1 << 3)
#define NPT_PCD ((uint64_t)1 << 4)
#define NPT_DIRTY ((uint64_t)1 << 6)
#define NPT_PS ((uint64_t)1 << 7)
#define NPT_PAT_4K ((uint64_t)1 << 7)
#define NPT_PAT_LARGE ((uint64_t)1 << 12)

// What a closed A20 gate folds, up to where a legacy guest reaches.
#define A20_BIT ((uint64_t)0x100000)
#define A20_LIMIT ((uint64_t)0x100000000)

uint64_t gpt_generation;

static bool use_ept;
static bool have_huge_pages;
static bool can_track_dirty;
static bool track_dirty;
static uint64_t *root;
// The processor's view while the guest's A20 gate is closed: below A20_LIMIT,
// every odd MiB is the even one before it, and above it, root's own tables.
// Built when first needed.
static uint64_t *masked_root;
static bool masked;
static struct spinlock gpt_lock;

static size_t level_size(size_t level) {
    return (uint64_t)1 << (12 + 9 * level);
}

static bool entry_present(uint64_t e) {
    if (use_ept) {
        return (e & (EPT_R | EPT_W | EPT_X)) != 0;
    }
    return (e & NPT_P) != 0;
}

static bool entry_large(uint64_t e, size_t level) {
    if (level == 0) {
        return true;
    }
    return (e & (use_ept ? EPT_LARGE : NPT_PS)) != 0;
}

static uint64_t table_entry(uintptr_t table) {
    if (use_ept) {
        return table | EPT_R | EPT_W | EPT_X;
    }
    return table | NPT_P | NPT_RW | NPT_US;
}

static uint64_t leaf_entry(
    uintptr_t hpa, uint32_t prot, uint8_t memtype, size_t level
) {
    if (prot == 0) {
        return 0;
    }

    if (use_ept) {
        uint64_t e = hpa | (uint64_t)memtype << EPT_MEMTYPE_SHIFT;
        if ((prot & GPT_R) != 0) {
            e |= EPT_R;
        }
        if ((prot & GPT_W) != 0) {
            e |= EPT_R | EPT_W;
        }
        if ((prot & GPT_X) != 0) {
            e |= EPT_X;
        }
        if (level > 0) {
            e |= EPT_LARGE;
        }
        return e;
    }

    uint64_t e = hpa | NPT_P | NPT_US;
    if ((prot & GPT_W) != 0) {
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

static uintptr_t leaf_addr(uint64_t e, size_t level) {
    return e & ENTRY_ADDR_MASK & ~(level_size(level) - 1);
}

// The access rights and memory type of a leaf, to split it into smaller ones.
static void leaf_decode(
    uint64_t e, size_t level, uint32_t *prot, uint8_t *memtype
) {
    if (use_ept) {
        *prot = 0;
        if ((e & EPT_R) != 0) {
            *prot |= GPT_R;
        }
        if ((e & EPT_W) != 0) {
            *prot |= GPT_W;
        }
        if ((e & EPT_X) != 0) {
            *prot |= GPT_X;
        }
        *memtype = (e >> EPT_MEMTYPE_SHIFT) & 7;
        return;
    }

    *prot = GPT_R | GPT_X;
    if ((e & NPT_RW) != 0) {
        *prot |= GPT_W;
    }
    uint64_t pat = level > 0 ? NPT_PAT_LARGE : NPT_PAT_4K;
    unsigned int index = ((e & pat) != 0 ? 4 : 0)
        | ((e & NPT_PCD) != 0 ? 2 : 0) | ((e & NPT_PWT) != 0 ? 1 : 0);
    *memtype = (HOST_PAT >> (index * 8)) & 7;
}

void gpt_init(bool ept, bool huge_pages, bool dirty_flags) {
    use_ept = ept;
    have_huge_pages = huge_pages;
    can_track_dirty = dirty_flags;
    root = page_alloc(1);
}

uintptr_t gpt_root(void) {
    return (uintptr_t)(__atomic_load_n(&masked, __ATOMIC_ACQUIRE) ? masked_root
        : root);
}

// Returns the table under top that holds the entry for gpa at level, creating
// the tables on the way, and splitting large leaves into tables of the same
// mappings.
static uint64_t *table_for(uint64_t *top, uintptr_t gpa, size_t level) {
    uint64_t *table = top;
    for (size_t l = 3; l > level; l--) {
        uint64_t *e = &table[(gpa >> (12 + 9 * l)) & 511];
        if (!entry_present(*e)) {
            uint64_t *next = page_alloc(1);
            *e = table_entry((uintptr_t)next);
        } else if (entry_large(*e, l)) {
            uint32_t prot;
            uint8_t memtype;
            leaf_decode(*e, l, &prot, &memtype);
            uintptr_t base = leaf_addr(*e, l);
            uint64_t *next = page_alloc(1);
            for (size_t i = 0; i < 512; i++) {
                next[i] = leaf_entry(
                    base + i * level_size(l - 1), prot, memtype, l - 1
                );
            }
            *e = table_entry((uintptr_t)next);
        }
        table = (uint64_t *)(uintptr_t)(*e & ENTRY_ADDR_MASK);
    }
    return table;
}

// The masked view's entry for a page of an even MiB, which also stands for the
// page a MiB above it.
static void mirror(uintptr_t gpa, uint64_t entry) {
    uint64_t *table = table_for(masked_root, gpa, 0);
    size_t i = (gpa >> 12) & 511;
    table[i] = entry;
    table[i + 256] = entry;
}

static void share_upper(void) {
    for (size_t i = 1; i < 512; i++) {
        masked_root[i] = root[i];
    }
    uint64_t *pdpt = (uint64_t *)(uintptr_t)(root[0] & ENTRY_ADDR_MASK);
    uint64_t *masked_pdpt = table_for(masked_root, 0, 2);
    for (size_t i = A20_LIMIT / level_size(2); i < 512; i++) {
        masked_pdpt[i] = pdpt[i];
    }
}

void gpt_map(
    uintptr_t gpa, uintptr_t hpa, size_t size, uint32_t prot, uint8_t memtype
) {
    dma_map(gpa, hpa, size, prot);

    spinlock_acquire(&gpt_lock);

    for (size_t at = 0; at < size; ) {
        size_t level = 0;
        for (size_t l = (prot & GPT_DIRTY) != 0 ? 0 : have_huge_pages ? 2 : 1;
             l > 0; l--) {
            uint64_t ls = level_size(l);
            if (((gpa + at) & (ls - 1)) == 0 && ((hpa + at) & (ls - 1)) == 0
             && size - at >= ls) {
                level = l;
                break;
            }
        }

        uint64_t *table = table_for(root, gpa + at, level);
        // A leaf that replaces a table leaks the table, as other CPUs may
        // still walk it until their next flush.
        table[((gpa + at) >> (12 + 9 * level)) & 511] = leaf_entry(
            hpa + at, prot, memtype, level
        );
        at += level_size(level);
    }

    if (masked_root != NULL) {
        for (size_t at = 0; at < size && gpa + at < A20_LIMIT;
             at += PAGE_SIZE) {
            if (((gpa + at) & A20_BIT) == 0) {
                mirror(gpa + at, leaf_entry(hpa + at, prot, memtype, 0));
            }
        }
        if (gpa + size > A20_LIMIT) {
            share_upper();
        }
    }

    spinlock_release(&gpt_lock);
}

void gpt_mask_a20(bool on) {
    spinlock_acquire(&gpt_lock);
    if (on && masked_root == NULL) {
        masked_root = page_alloc(1);
        for (uintptr_t mib = 0; mib < A20_LIMIT; mib += 2 * A20_BIT) {
            for (uintptr_t gpa = mib; gpa < mib + A20_BIT; gpa += PAGE_SIZE) {
                uintptr_t hpa;
                uint32_t prot;
                uint8_t memtype;
                if (gpt_translate(gpa, &hpa, &prot, &memtype)) {
                    mirror(gpa, leaf_entry(hpa, prot, memtype, 0));
                }
            }
        }
        share_upper();
    }
    __atomic_store_n(&masked, on, __ATOMIC_RELEASE);
    spinlock_release(&gpt_lock);
    gpt_flush();
}

uintptr_t gpt_cpu_gpa(uintptr_t gpa) {
    if (gpa < A20_LIMIT && __atomic_load_n(&masked, __ATOMIC_ACQUIRE)) {
        return gpa & ~A20_BIT;
    }
    return gpa;
}

bool gpt_translate_leaf(
    uintptr_t gpa, uint64_t *hpa, uint32_t *prot, uint8_t *memtype,
    size_t *level
) {
    uint64_t *table = root;
    for (size_t l = 4; l-- > 0; ) {
        uint64_t e = table[(gpa >> (12 + 9 * l)) & 511];
        if (!entry_present(e)) {
            return false;
        }
        if (entry_large(e, l)) {
            uint8_t type;
            leaf_decode(e, l, prot, &type);
            if (memtype != NULL) {
                *memtype = type;
            }
            if (level != NULL) {
                *level = l;
            }
            *hpa = leaf_addr(e, l) | (gpa & (level_size(l) - 1));
            return true;
        }
        table = (uint64_t *)(uintptr_t)(e & ENTRY_ADDR_MASK);
    }
    return false;
}

bool gpt_translate(
    uintptr_t gpa, uint64_t *hpa, uint32_t *prot, uint8_t *memtype
) {
    return gpt_translate_leaf(gpa, hpa, prot, memtype, NULL);
}

void gpt_flush(void) {
    __atomic_add_fetch(&gpt_generation, 1, __ATOMIC_SEQ_CST);
    for (size_t i = 0; i < smp_cpu_count(); i++) {
        vcpu_kick(smp_cpu(i));
    }
}

bool gpt_track_dirty(void) {
    track_dirty = can_track_dirty;
    return track_dirty;
}

bool gpt_dirty_tracked(void) {
    return track_dirty;
}

// The entry of the 4 KiB leaf under top that maps gpa, if one does.
static uint64_t *page_entry(uint64_t *top, uintptr_t gpa) {
    uint64_t *table = top;
    for (size_t l = 3; l > 0; l--) {
        uint64_t e = table[(gpa >> (12 + 9 * l)) & 511];
        if (!entry_present(e) || entry_large(e, l)) {
            return NULL;
        }
        table = (uint64_t *)(uintptr_t)(e & ENTRY_ADDR_MASK);
    }
    return &table[(gpa >> 12) & 511];
}

static bool take_dirty(uint64_t *e, bool clear) {
    uint64_t dirty = use_ept ? EPT_DIRTY : NPT_DIRTY;
    if (e == NULL || (__atomic_load_n(e, __ATOMIC_RELAXED) & dirty) == 0) {
        return false;
    }
    if (clear) {
        __atomic_fetch_and(e, ~dirty, __ATOMIC_RELAXED);
    }
    return true;
}

bool gpt_dirty(
    uintptr_t gpa, size_t size, uint64_t *bits, uintptr_t first, bool clear
) {
    if (!track_dirty) {
        return false;
    }
    bool any = false;
    spinlock_acquire(&gpt_lock);
    for (size_t i = 0; i < size / PAGE_SIZE; i++) {
        uintptr_t at = gpa + i * PAGE_SIZE;
        bool dirty = take_dirty(page_entry(root, at), clear);
        // While the A20 gate is closed, the processor reaches an even MiB's
        // pages through the masked view's entries for them and the MiB above.
        if (masked_root != NULL && at < A20_LIMIT && (at & A20_BIT) == 0) {
            dirty |= take_dirty(page_entry(masked_root, at), clear);
            dirty |= take_dirty(page_entry(masked_root, at | A20_BIT), clear);
        }
        if (!dirty) {
            continue;
        }
        any = true;
        if (bits != NULL) {
            bits[(first + i) / 64] |= (uint64_t)1 << ((first + i) % 64);
        } else if (!clear) {
            break;
        }
    }
    spinlock_release(&gpt_lock);
    return any;
}
