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
#include <uacpi/acpi.h>
#include <iommu/dma.h>
#include <hw/lapic.h>
#include <iommu/amdvi.h>
#include <iommu/iommu_tables.h>
#include <iommu/vtd.h>
#include <lib/log.h>
#include <lib/spinlock.h>
#include <lib/time.h>
#include <lib/util.h>
#include <sys/mm.h>
#include <sys/ptable.h>
#include <sys/x86.h>
#include <virt/gpt.h>
#include <virt/hv_timer.h>

// What 4-level tables translate.
#define DMA_ADDRESS_LIMIT ((uint64_t)1 << 48)
// While the guest runs, faults are reported each second, and only a few, as a
// flood of them would hold the boot processor.
#define FAULT_LINES 4
#define LEGACY_END ((uint64_t)0x100000)

// VT-d second-stage paging entries.
#define VTD_R ((uint64_t)1 << 0)
#define VTD_W ((uint64_t)1 << 1)
#define VTD_PS ((uint64_t)1 << 7)

// AMD-Vi host page table entries, where a directory entry names the level of
// the table it points to, counting from 1, and a leaf names none.
#define AMD_PR ((uint64_t)1 << 0)
#define AMD_NEXT_LEVEL_SHIFT 9
#define AMD_NEXT_LEVEL_MASK ((uint64_t)7 << 9)
#define AMD_IR ((uint64_t)1 << 61)
#define AMD_IW ((uint64_t)1 << 62)

static uint32_t kind;
// The largest level a leaf may be at: 0 for 4KiB pages only, 1 for 2MiB, 2 for
// 1GiB, as every IOMMU takes.
static size_t leaf_level_max;
static uint64_t *root;
static bool enabled;
static struct spinlock dma_lock;
static struct hv_timer fault_timer;

static bool entry_present(uint64_t e) {
    if (kind == IOMMU_TABLES_KIND_AMD) {
        return (e & AMD_PR) != 0;
    }
    return (e & (VTD_R | VTD_W)) != 0;
}

static bool entry_leaf(uint64_t e) {
    if (kind == IOMMU_TABLES_KIND_AMD) {
        return (e & AMD_NEXT_LEVEL_MASK) == 0;
    }
    return (e & VTD_PS) != 0;
}

// The entry at level that points to the table below it.
static uint64_t table_entry(uintptr_t table, size_t level) {
    if (kind == IOMMU_TABLES_KIND_AMD) {
        return table | AMD_PR | (uint64_t)level << AMD_NEXT_LEVEL_SHIFT | AMD_IR | AMD_IW;
    }
    return table | VTD_R | VTD_W;
}

// As in EPT, write access comes with read access.
static uint64_t leaf_entry(uintptr_t hpa, uint64_t prot, size_t level) {
    if ((prot & (GPT_R | GPT_W)) == 0) {
        return 0;
    }
    bool write = (prot & GPT_W) != 0;
    if (kind == IOMMU_TABLES_KIND_AMD) {
        return hpa | AMD_PR | AMD_IR | (write ? AMD_IW : 0);
    }
    return hpa | VTD_R | (write ? VTD_W : 0) | (level > 0 ? VTD_PS : 0);
}

static uint64_t leaf_prot(uint64_t e, size_t level) {
    (void)level;
    bool write = (e & (kind == IOMMU_TABLES_KIND_AMD ? AMD_IW : VTD_W)) != 0;
    return write ? GPT_R | GPT_W : GPT_R;
}

static const struct ptable_format table_format = {
    entry_present, entry_leaf, table_entry, leaf_entry, leaf_prot
};

static void flush(void) {
    if (kind == IOMMU_TABLES_KIND_AMD) {
        amdvi_flush();
    } else {
        vtd_flush();
    }
}

void dma_init(void) {
    const struct iommu_tables *tables = iommu_tables_parse();
    kind = tables->kind;

    // The local APICs' mode, chosen before the units remap interrupts in its
    // format: x2APIC mode where every unit can remap in it, unless the DMAR
    // asks system software not to turn it on (VT-d D51397-019 rev. 5.20, 8.1).
    bool usable = false;
    if (kind == IOMMU_TABLES_KIND_VTD) {
        usable = vtd_probe(tables, &leaf_level_max);
        if (usable) {
            uint32_t opt_out = ACPI_DMAR_INTR_REMAP | ACPI_DMAR_X2APIC_OPT_OUT;
            bool opted_out = (tables->vtd_flags & opt_out) == opt_out;
            lapic_host_choose(vtd_remap_x2apic_capable() && !opted_out);
            vtd_start(lapic_host_x2apic());
        }
    } else if (kind == IOMMU_TABLES_KIND_AMD) {
        usable = amdvi_probe(tables, &leaf_level_max);
        if (usable) {
            lapic_host_choose(amdvi_x2apic_capable());
            amdvi_start();
        }
    }
    if (!usable) {
        panic("No IOMMU can remap DMA\n");
    }
    if (tables->truncated) {
        printf("Devices of the IOMMUs the tables leave out keep untranslated DMA\n");
    }
    root = page_alloc(1);
}

void dma_map(uintptr_t gpa, uintptr_t hpa, size_t size, uint32_t prot) {
    if (root == NULL) {
        panic("Guest memory mapped before the IOMMU tables\n");
    }
    spinlock_acquire(&dma_lock);
    ptable_map(&table_format, root, gpa, hpa, size, prot, leaf_level_max);
    if (enabled) {
        flush();
    }
    spinlock_release(&dma_lock);
}

// One domain serves every device, so memory the firmware needs mapped for some
// devices is mapped for all, one to one, as the guest has it too, but for the
// first MiB, the guest's own memory.
static void reserved_regions_apply(const struct iommu_tables *tables) {
    for (size_t i = 0; i < tables->reserved_count; i++) {
        const struct iommu_tables_reserved_region *r = &tables->reserved[i];
        if (r->last >= DMA_ADDRESS_LIMIT) {
            printf(
                "IOMMU region %#llx-%#llx is beyond the tables' reach\n",
                (unsigned long long)r->base, (unsigned long long)r->last
            );
            continue;
        }
        uintptr_t base = page_down(r->base);
        uintptr_t last = r->last | (PAGE_SIZE - 1);
        if (last < LEGACY_END) {
            continue;
        }
        if (base < LEGACY_END) {
            base = LEGACY_END;
        }
        uint32_t prot = 0;
        if (r->exclusion || r->writable) {
            prot = GPT_R | GPT_W;
        } else if (r->readable) {
            prot = GPT_R;
        }
        ptable_map(&table_format, root, base, base, last - base + 1, prot, leaf_level_max);
    }
}

static size_t report_faults(size_t limit) {
    if (!enabled) {
        return 0;
    }
    return kind == IOMMU_TABLES_KIND_AMD ? amdvi_report_faults(limit) : vtd_report_faults(limit);
}

static void fault_timer_fired(void *ctx) {
    (void)ctx;
    size_t unshown = report_faults(FAULT_LINES);
    if (unshown != 0) {
        printf("%zu more IOMMU faults\n", unshown);
    }
    hv_timer_set(&fault_timer, rdtsc() + time_tsc_hz());
}

void dma_enable(void) {
    const struct iommu_tables *tables = iommu_tables_parse();

    spinlock_acquire(&dma_lock);
    reserved_regions_apply(tables);
    if (kind == IOMMU_TABLES_KIND_AMD) {
        amdvi_enable((uintptr_t)root);
    } else {
        vtd_enable((uintptr_t)root);
    }
    enabled = true;
    spinlock_release(&dma_lock);
    printf("Device DMA remapped\n");
    fault_timer.fired = fault_timer_fired;
    hv_timer_add(&fault_timer);
    hv_timer_set(&fault_timer, rdtsc() + time_tsc_hz());
}

void dma_report_faults(void) {
    report_faults(SIZE_MAX);
}
