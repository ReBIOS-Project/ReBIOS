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
#include <virt/igfx.h>
#include <core.h>
#include <csm/e820.h>
#include <handoff.h>
#include <hw/pci_cfg.h>
#include <hw/pci_enum.h>
#include <iommu/dma.h>
#include <iommu/iommu_tables.h>
#include <lib/log.h>
#include <lib/time.h>
#include <memory.h>
#include <sys/mm.h>
#include <virt/gpt.h>
#include <virt/layout.h>
#include <virt/mmio.h>
#include <virt/pci_virt.h>
#include <virt/vcpu.h>

#define VENDOR_INTEL 0x8086
#define CLASS_DISPLAY 0x03
#define IGFX_DEVICE 2
#define IGFX_RID (IGFX_DEVICE << 3)

// Linux 7.3 include/drm/intel/pci_config.h ASLS, and
// drivers/gpu/drm/i915/display/intel_opregion.c: the header's version and
// mailboxes, mailbox 3's RVDA and RVDS, and the 8 KiB a driver maps at ASLS
// whatever the header's size says. RVDA is a physical address in OpRegion 2.0
// and an offset from the OpRegion from 2.1 on.
#define REG_ASLS 0xfc
#define OPREGION_SIZE ((uint32_t)0x2000)
#define OPREGION_SIGNATURE "IntelGraphicsMem"
#define SIGNATURE_SIZE 16
#define HEADER_OVER_MINOR 0x16
#define HEADER_OVER_MAJOR 0x17
#define HEADER_MBOXES 0x58
#define MBOX_ASLE ((uint32_t)1 << 2)
#define ASLE_RVDA 0x3ba
#define ASLE_RVDS 0x3c2
// A raw VBT larger than this is taken for garbage.
#define RAW_VBT_MAX ((uint32_t)0x100000)

// Linux 7.3 include/drm/intel/i915_drm.h: the GMCH control register's mirror
// with BDW_GMCH_GMS and BDW_GMCH_GGMS, decoded as arch/x86/kernel/
// early-quirks.c gen9_stolen_size() and gt/intel_ggtt.c
// gen8_get_total_gtt_size() do; the stolen memory's base, INTEL_BSM through
// Gen10 and INTEL_GEN11_BSM_DW0/DW1 for Gen11 and Gen12, none from Meteor Lake
// on. The host bridge's copies are at B0h and B4h (Intel Core Ultra Processors
// for H-series and U-series Platforms CFG and MEM Registers 795258 version
// 002, BDSM_0_0_0_PCI and BGSM_0_0_0_PCI).
#define REG_GGC 0x50
#define GGC_GGMS_SHIFT 6
#define GGC_GGMS_MASK ((uint32_t)0x3)
#define GGC_GMS_SHIFT 8
#define GGC_GMS_MASK ((uint32_t)0xff)
#define GMS_4M_FROM ((uint32_t)0xf0)
#define REG_BSM 0x5c
#define REG_BSM_GEN11 0xc0
#define BSM_MASK ((uint32_t)0xfff00000)
#define BSM_MASK_64 (~(uint64_t)0xfffff)
#define REG_HOST_BDSM 0xb0
#define REG_HOST_BGSM 0xb4
#define HOST_BRIDGE_DEVICE 0

// Gen6 and Gen7 have a 4 MiB GTTMMADR, Gen8 on 16 MiB (Linux 7.3
// gt/intel_ggtt.c gen6_gttmmadr_size()). Below Gen8 the stolen memory stays at
// its host address: Linux 7.3 gem/i915_gem_stolen.c uses none there with VT-d
// translating.
#define GTTMMADR_GEN8_SIZE ((uint64_t)0x1000000)

// GTTMMADR's registers that give the stolen memory's host addresses: HSW PRM
// Vol. 12 (12/18/2013) MBDSM and MBGSM, Linux 7.3 i915_reg.h GEN6_DSMBASE and
// GEN6_STOLEN_RESERVED, xe regs/xe_regs.h GSMBASE and GGC. STOLEN_RESERVED's
// base has GEN6_STOLEN_RESERVED_ADDR_MASK's bits through Gen10
// (i915_gem_stolen.c bdw_get_stolen_reserved() and chv_get_stolen_reserved()),
// and GEN11_STOLEN_RESERVED_ADDR_MASK's from Gen11.
#define MMIO_PAGE ((uint32_t)0x108000)
#define MMIO_GGC ((uint32_t)0x108040)
#define MMIO_DSMBASE ((uint32_t)0x1080c0)
#define MMIO_GSMBASE ((uint32_t)0x108100)
#define MMIO_STOLEN_RESERVED ((uint32_t)0x1082c0)

// The GGTT in GTTMMADR's second half from Gen8 on (Linux 7.3 gt/intel_ggtt.c
// gen6_gttadr_offset(), xe xe_ggtt.c), its entries as gt/intel_gtt.h has them:
// GEN8_PAGE_PRESENT, GEN12_GGTT_PTE_LM (an offset into the stolen memory from
// Meteor Lake on), and GEN12_GGTT_PTE_ADDR_MASK; gen8_ggtt_invalidate()'s
// flush through GFX_FLSH_CNTL_GEN6, which needs_wc_ggtt_mapping() asks for on
// Gen8 to Gen10 but Gen9 LP. Broxton's and Gemini Lake's device IDs are
// include/drm/intel/pciids.h INTEL_BXT_IDS and INTEL_GLK_IDS.
#define GGTT_OFFSET ((uint64_t)0x800000)
#define PTE_PRESENT ((uint64_t)1)
#define PTE_LM ((uint64_t)1 << 1)
#define PTE_ADDRESS ((uint64_t)0x3ffffffff000)
#define MMIO_FLSH_CNTL ((uint32_t)0x101008)
#define FLSH_CNTL_EN ((uint32_t)1)
// A few frames, for the display to fetch the GGTT's entries anew before their
// old addresses lead elsewhere.
#define SETTLE_US 50000

// Linux 7.3 display/intel_fbc.c WaFbcTurnOffFbcWhenHyperVisorIsUsed: Skylake's
// and Broxton's frame buffer compression stays off while VT-d translates,
// which it always does here. FBC turns on by DPFC_CTL_EN in ILK_DPFC_CONTROL
// (display/intel_fbc_regs.h; the one FBC of Gen9's at 43208h). The device IDs
// are include/drm/intel/pciids.h INTEL_SKL_IDS and INTEL_BXT_IDS.
#define FBC_PAGE ((uint32_t)0x43000)
#define FBC_CONTROL_TOP ((uint32_t)0x4320b)
#define FBC_CONTROL_EN_BYTE ((uint32_t)0x80)

#define STOLEN_NONE 0
// BDSM at 5Ch, the MMIO copies 32-bit: Gen9 and Gen10.
#define STOLEN_BSM 1
// BDSM at C0h and C4h, the MMIO copies 64-bit: Gen11 and Gen12.
#define STOLEN_BSM_64 2
// No BDSM in the function: from Meteor Lake on.
#define STOLEN_DSMBASE 3

static struct {
    struct pci_function *f;
    struct pci_function *host_bridge;
    // The host's OpRegion and the bytes from it the guest may read: a raw VBT
    // that follows it included.
    uint32_t asls;
    size_t size;
    uintptr_t raw_vbt_base;
    uintptr_t raw_vbt_end;
    // What the guest's ASLS holds, which never reaches the hardware: the
    // host's AML finds the OpRegion through the real one.
    uint32_t guest_asls;
    int kind;
    uint64_t gttmmadr;
    // The host's data stolen memory, the GGTT's memory below it, and the
    // guest's address for the former, 0 until layout_init() places it.
    uint64_t dsm;
    size_t dsm_size;
    size_t gsm_size;
    uint64_t alias;
    // Whether the stolen memory's RMRR is gone, so that the GGTT's entries
    // have to give the guest's addresses once the IOMMU translates.
    bool rmrr_dropped;
} igfx;

static uint64_t page_down(uint64_t x) {
    return x & ~(uint64_t)(PAGE_SIZE - 1);
}

static uint64_t page_up(uint64_t x) {
    return page_down(x + PAGE_SIZE - 1);
}

static uint64_t read64(uintptr_t hpa) {
    return mmio_read_host(hpa, 4) | mmio_read_host(hpa + 4, 4) << 32;
}

static void scan_opregion(struct pci_function *f) {
    uint32_t asls = pci_cfg_read(f->addr, REG_ASLS, 4);
    if (asls == 0 || asls == UINT32_MAX) {
        return;
    }
    const uint8_t *op = (const uint8_t *)(uintptr_t)asls;
    if (memcmp(op, OPREGION_SIGNATURE, SIGNATURE_SIZE) != 0) {
        printf("Intel graphics: no OpRegion at %#x, which ASLS gives\n", asls);
        return;
    }
    uintptr_t end = (uint64_t)asls + OPREGION_SIZE;
    uint32_t mboxes;
    uint64_t rvda;
    uint32_t rvds;
    memcpy(&mboxes, op + HEADER_MBOXES, sizeof(uint32_t));
    memcpy(&rvda, op + ASLE_RVDA, sizeof(uint64_t));
    memcpy(&rvds, op + ASLE_RVDS, sizeof(uint32_t));
    uint8_t major = op[HEADER_OVER_MAJOR];
    uint8_t minor = op[HEADER_OVER_MINOR];
    if (major >= 2 && (mboxes & MBOX_ASLE) != 0 && rvda != 0 && rvds != 0
     && rvds <= RAW_VBT_MAX) {
        if (major > 2 || minor >= 1) {
            if (rvda >= OPREGION_SIZE && rvda <= RAW_VBT_MAX
             && asls + rvda + rvds > end) {
                end = asls + rvda + rvds;
            }
        } else {
            igfx.raw_vbt_base = page_down(rvda);
            igfx.raw_vbt_end = page_up(rvda + rvds);
        }
    }
    igfx.asls = asls;
    igfx.size = end - asls;
}

static size_t gms_size(uint32_t ggc) {
    uint32_t gms = (ggc >> GGC_GMS_SHIFT) & GGC_GMS_MASK;
    if (gms < GMS_4M_FROM) {
        return (uint64_t)gms << 25;
    }
    return (uint64_t)(gms - GMS_4M_FROM + 1) << 22;
}

static size_t ggms_size(uint32_t ggc) {
    uint32_t ggms = (ggc >> GGC_GGMS_SHIFT) & GGC_GGMS_MASK;
    return ggms != 0 ? (uint64_t)1 << (20 + ggms) : 0;
}

static void scan_stolen(struct pci_function *f) {
    const struct pci_bar *bar = &f->bars[0];
    if (f->bar_count == 0 || bar->kind != PCI_BAR_MEM64
     || bar->size < GTTMMADR_GEN8_SIZE) {
        return;
    }
    uint32_t ggc = pci_cfg_read(f->addr, REG_GGC, 2);
    uint64_t bsm = pci_cfg_read(f->addr, REG_BSM, 4) & BSM_MASK;
    uint64_t bsm_64 = (pci_cfg_read(f->addr, REG_BSM_GEN11, 4)
        | (uint64_t)pci_cfg_read(f->addr, REG_BSM_GEN11 + 4, 4) << 32)
        & BSM_MASK_64;
    igfx.gttmmadr = bar->base;
    if (core_handoff->config.igpu_stolen_remap == 0) {
        printf(
            "Intel graphics: the stolen memory stays at its host address "
            "(igpu_stolen_remap)\n"
        );
        return;
    }
    mm_map_device(bar->base + MMIO_PAGE, PAGE_SIZE);
    int kind;
    uint64_t dsm;
    if (bsm != 0) {
        kind = STOLEN_BSM;
        dsm = bsm;
    } else if (bsm_64 != 0) {
        kind = STOLEN_BSM_64;
        dsm = bsm_64;
    } else {
        kind = STOLEN_DSMBASE;
        dsm = read64(bar->base + MMIO_DSMBASE) & BSM_MASK_64;
        ggc = (uint32_t)mmio_read_host(bar->base + MMIO_GGC, 2);
    }
    size_t size = gms_size(ggc);
    uint64_t gsm = ggms_size(ggc);
    if (dsm == 0 || size == 0 || gsm > dsm) {
        return;
    }
    igfx.kind = kind;
    igfx.dsm = dsm;
    igfx.dsm_size = size;
    igfx.gsm_size = gsm;
}

static bool gen9_lp(uint16_t id) {
    static const uint16_t ids[] = {
        0x0a84, 0x1a84, 0x1a85, 0x5a84, 0x5a85, 0x3184, 0x3185
    };
    for (size_t i = 0; i < sizeof(ids) / sizeof(uint16_t); i++) {
        if (id == ids[i]) {
            return true;
        }
    }
    return false;
}

static bool fbc_kept_off(uint16_t id) {
    static const uint16_t ids[] = {
        0x1902, 0x1906, 0x190a, 0x190b, 0x190e, 0x1912, 0x1913, 0x1915, 0x1916,
        0x1917, 0x191a, 0x191b, 0x191d, 0x191e, 0x1921, 0x1923, 0x1926, 0x1927,
        0x192a, 0x192b, 0x192d, 0x1932, 0x193a, 0x193b, 0x193d, 0x0a84, 0x1a84,
        0x1a85, 0x5a84, 0x5a85,
    };
    for (size_t i = 0; i < sizeof(ids) / sizeof(uint16_t); i++) {
        if (id == ids[i]) {
            return true;
        }
    }
    return false;
}

static bool rmrr_unneeded(uint16_t segment, uint16_t rid) {
    return segment == 0 && rid == IGFX_RID;
}

void igfx_scan(void) {
    struct pci_function *f = pci_enum_find(
        (struct pci_addr){ 0, 0, IGFX_DEVICE, 0 }
    );
    if (f == NULL || f->vendor_id != VENDOR_INTEL
     || f->base_class != CLASS_DISPLAY) {
        return;
    }
    igfx.f = f;
    struct pci_function *h = pci_enum_find(
        (struct pci_addr){ 0, 0, HOST_BRIDGE_DEVICE, 0 }
    );
    if (h != NULL && h->vendor_id == VENDOR_INTEL) {
        igfx.host_bridge = h;
    }
    scan_opregion(f);
    scan_stolen(f);
    // With MSI-X in a BAR, its page might be one the hypervisor takes over.
    if (igfx.kind != STOLEN_NONE && f->msix_cap == 0) {
        igfx.rmrr_dropped = true;
        iommu_tables_forget_reserved(rmrr_unneeded);
    }
}

bool igfx_one_to_one(uint64_t *base, uint64_t *end) {
    if (igfx.raw_vbt_end == 0) {
        return false;
    }
    *base = igfx.raw_vbt_base;
    *end = igfx.raw_vbt_end;
    return true;
}

bool igfx_stolen(
    uint64_t *host, uint64_t *size, uint64_t *clear_base, uint64_t *clear_end
) {
    if (igfx.kind == STOLEN_NONE) {
        return false;
    }
    *host = igfx.dsm;
    *size = igfx.dsm_size;
    *clear_base = igfx.dsm - igfx.gsm_size;
    *clear_end = igfx.dsm + igfx.dsm_size;
    return true;
}

void igfx_stolen_placed(uintptr_t gpa) {
    igfx.alias = gpa;
}

bool igfx_stolen_rmrr_dropped(void) {
    return igfx.rmrr_dropped;
}

// A host address in the data stolen memory as the guest has it: the rest of
// value, the register's other bits, as they are.
static uint64_t translate(uint64_t value, uint64_t mask) {
    uintptr_t address = value & mask;
    if (address < igfx.dsm || address - igfx.dsm >= igfx.dsm_size) {
        return value;
    }
    return (value & ~mask) | (igfx.alias + (address - igfx.dsm));
}

// The GGTT's memory, as far below the guest's stolen memory as the host's is.
static uint64_t translate_gsm(uint64_t value, uint64_t mask) {
    if ((value & mask) != igfx.dsm - igfx.gsm_size) {
        return value;
    }
    return (value & ~mask) | (igfx.alias - igfx.gsm_size);
}

static bool config_read(uint32_t reg, uint32_t *value) {
    if (reg == REG_ASLS && igfx.guest_asls != 0) {
        *value = igfx.guest_asls;
        return true;
    }
    if (igfx.alias == 0) {
        return false;
    }
    if (igfx.kind == STOLEN_BSM && reg == REG_BSM) {
        *value = (uint32_t)translate(
            pci_cfg_read(igfx.f->addr, reg, 4), BSM_MASK
        );
        return true;
    }
    if (igfx.kind == STOLEN_BSM_64
     && (reg == REG_BSM_GEN11 || reg == REG_BSM_GEN11 + 4)) {
        uint64_t bsm = pci_cfg_read(igfx.f->addr, REG_BSM_GEN11, 4)
            | (uint64_t)pci_cfg_read(igfx.f->addr, REG_BSM_GEN11 + 4, 4) << 32;
        bsm = translate(bsm, BSM_MASK_64);
        *value = (uint32_t)(reg == REG_BSM_GEN11 ? bsm : bsm >> 32);
        return true;
    }
    return false;
}

// The firmware locks the stolen memory's registers: guest writes go nowhere.
static bool config_write(uint32_t reg, uint32_t value, uint32_t mask) {
    if (reg == REG_ASLS && igfx.guest_asls != 0) {
        igfx.guest_asls = (igfx.guest_asls & ~mask) | (value & mask);
        return true;
    }
    if (igfx.alias == 0) {
        return false;
    }
    return (igfx.kind == STOLEN_BSM && reg == REG_BSM)
        || (igfx.kind == STOLEN_BSM_64
            && (reg == REG_BSM_GEN11 || reg == REG_BSM_GEN11 + 4));
}

static const struct pci_virt_hook config_hook = { config_read, config_write };

static bool host_bridge_read(uint32_t reg, uint32_t *value) {
    if (reg == REG_HOST_BDSM) {
        *value = (uint32_t)translate(
            pci_cfg_read(igfx.host_bridge->addr, reg, 4), BSM_MASK
        );
        return true;
    }
    if (reg == REG_HOST_BGSM) {
        *value = (uint32_t)translate_gsm(
            pci_cfg_read(igfx.host_bridge->addr, reg, 4), BSM_MASK
        );
        return true;
    }
    return false;
}

static bool host_bridge_write(uint32_t reg, uint32_t value, uint32_t mask) {
    (void)value;
    (void)mask;
    return reg == REG_HOST_BDSM || reg == REG_HOST_BGSM;
}

static const struct pci_virt_hook host_bridge_hook = {
    host_bridge_read, host_bridge_write
};

// GTTMMADR's register at reg, which gives a host address, as the guest has it.
static uint64_t mmio_translated(uint32_t reg) {
    bool wide = igfx.kind != STOLEN_BSM;
    uint64_t mask = wide ? BSM_MASK_64 : BSM_MASK;
    uint64_t value = read64(igfx.gttmmadr + reg);
    uint64_t low = wide ? value : (uint32_t)value;
    switch (reg) {
        case MMIO_DSMBASE: {
            low = translate(low, mask);
            break;
        }
        case MMIO_GSMBASE: {
            low = translate_gsm(low, mask);
            break;
        }
        default: {
            low = translate(low, mask);
            break;
        }
    }
    return wide ? low : (value & ~(uint64_t)0xffffffff) | low;
}

static bool mmio_own(uint32_t reg, uint32_t *base) {
    static const uint32_t regs[] = {
        MMIO_DSMBASE, MMIO_GSMBASE, MMIO_STOLEN_RESERVED
    };
    for (size_t i = 0; i < sizeof(regs) / sizeof(uint32_t); i++) {
        if (reg >= regs[i] && reg < regs[i] + 8) {
            *base = regs[i];
            return true;
        }
    }
    return false;
}

static uint64_t mmio_read(
    struct vcpu *v, void *ctx, uint64_t offset, size_t size
) {
    (void)v;
    (void)ctx;
    uint32_t reg = MMIO_PAGE + (uint32_t)offset;
    uint32_t base;
    if (!mmio_own(reg, &base)) {
        return mmio_read_host(igfx.gttmmadr + reg, size);
    }
    uint64_t value = mmio_translated(base) >> (8 * (reg - base));
    return size == 8 ? value : value & (((uint64_t)1 << (8 * size)) - 1);
}

static void mmio_write(
    struct vcpu *v, void *ctx, uint64_t offset, size_t size, uint64_t value
) {
    (void)v;
    (void)ctx;
    uint32_t reg = MMIO_PAGE + (uint32_t)offset;
    uint32_t base;
    if (!mmio_own(reg, &base)) {
        mmio_write_host(igfx.gttmmadr + reg, size, value);
    }
}

static const struct mmio_ops mmio_ops = { mmio_read, mmio_write, false };

static uint64_t fbc_read(
    struct vcpu *v, void *ctx, uint64_t offset, size_t size
) {
    (void)v;
    (void)ctx;
    return mmio_read_host(igfx.gttmmadr + FBC_PAGE + offset, size);
}

static void fbc_write(
    struct vcpu *v, void *ctx, uint64_t offset, size_t size, uint64_t value
) {
    (void)v;
    (void)ctx;
    uint32_t reg = FBC_PAGE + (uint32_t)offset;
    if (reg <= FBC_CONTROL_TOP && FBC_CONTROL_TOP < reg + size) {
        value &= ~((uint64_t)FBC_CONTROL_EN_BYTE
            << (8 * (FBC_CONTROL_TOP - reg)));
    }
    mmio_write_host(igfx.gttmmadr + FBC_PAGE + offset, size, value);
}

static const struct mmio_ops fbc_ops = { fbc_read, fbc_write, false };

static void init_fbc(void) {
    mm_map_device(igfx.gttmmadr + FBC_PAGE, PAGE_SIZE);
    if (!pci_virt_trap(igfx.f->addr, 0, FBC_PAGE, PAGE_SIZE, &fbc_ops, NULL)) {
        printf("Intel graphics: FBC cannot be kept off\n");
        return;
    }
    uintptr_t top = igfx.gttmmadr + FBC_CONTROL_TOP;
    mmio_write_host(top, 1, mmio_read_host(top, 1) & ~FBC_CONTROL_EN_BYTE);
    printf("Intel graphics: FBC kept off, as VT-d translates\n");
}

static void init_opregion(void) {
    uint64_t offset = igfx.asls & (PAGE_SIZE - 1);
    uintptr_t host = igfx.asls - offset;
    size_t size = page_up(offset + igfx.size);
    // Pages that back guest RAM elsewhere would be the guest's twice.
    if (!layout_host_kept(host, host + size)) {
        printf(
            "Intel graphics: the OpRegion at %#x is in memory the guest may "
            "have as RAM\n",
            igfx.asls
        );
        return;
    }
    uint32_t alias = layout_take((uint32_t)size, PAGE_SIZE, E820_NVS);
    if (alias == 0) {
        printf(
            "Intel graphics: no room in the guest's memory for the OpRegion\n"
        );
        return;
    }
    gpt_map(alias, host, size, GPT_R | GPT_W, MT_WB);
    gpt_flush();
    igfx.guest_asls = alias + (uint32_t)offset;
    printf(
        "Intel graphics: the OpRegion at %#x is the guest's at %#x\n",
        igfx.asls, igfx.guest_asls
    );
}

static void init_stolen(void) {
    if (igfx.host_bridge != NULL) {
        pci_virt_hook(igfx.host_bridge->addr, &host_bridge_hook);
    }
    bool trapped = pci_virt_trap(
        igfx.f->addr, 0, MMIO_PAGE, PAGE_SIZE, &mmio_ops, NULL
    );
    if (!trapped) {
        printf(
            "Intel graphics: GTTMMADR's registers of the stolen memory cannot "
            "be trapped\n"
        );
    }
    printf(
        "Intel graphics: the stolen memory at %#llx-%#llx is the guest's at "
        "%#llx\n",
        (unsigned long long)igfx.dsm,
        (unsigned long long)(igfx.dsm + igfx.dsm_size - 1),
        (unsigned long long)igfx.alias
    );
}

void igfx_init(void) {
    if (igfx.f == NULL) {
        return;
    }
    if (igfx.asls != 0) {
        init_opregion();
    }
    if (igfx.alias != 0) {
        init_stolen();
    }
    if (igfx.gttmmadr != 0 && fbc_kept_off(igfx.f->device_id)) {
        init_fbc();
    }
    if (igfx.guest_asls != 0 || igfx.alias != 0) {
        pci_virt_hook(igfx.f->addr, &config_hook);
    }
}

void igfx_switch_begin(void) {
    if (!igfx.rmrr_dropped || igfx.alias == 0) {
        return;
    }
    // The firmware's entries give host addresses until igfx_switch_end().
    uintptr_t base = igfx.dsm - igfx.gsm_size;
    dma_map(base, base, igfx.gsm_size + igfx.dsm_size, GPT_R | GPT_W);
}

// The guest's own mappings of [base, end) in the IOMMU again, in runs.
static void dma_restore(uintptr_t base, uintptr_t end) {
    uintptr_t run = base;
    uintptr_t run_hpa = 0;
    uint32_t run_prot = 0;
    for (uintptr_t gpa = base; gpa <= end; gpa += PAGE_SIZE) {
        uintptr_t hpa = 0;
        uint32_t prot = 0;
        uint8_t memtype;
        if (gpa < end && !gpt_translate(gpa, &hpa, &prot, &memtype)) {
            hpa = 0;
            prot = 0;
        }
        bool same = gpa < end && prot == run_prot
            && (prot == 0 || hpa == run_hpa + (gpa - run));
        if (same) {
            continue;
        }
        if (gpa > run) {
            dma_map(run, run_prot != 0 ? run_hpa : 0, gpa - run, run_prot);
        }
        run = gpa;
        run_hpa = hpa;
        run_prot = prot;
    }
}

void igfx_switch_end(void) {
    if (!igfx.rmrr_dropped || igfx.alias == 0) {
        return;
    }
    uintptr_t ggtt = igfx.gttmmadr + GGTT_OFFSET;
    mm_map_device(ggtt, igfx.gsm_size);
    unsigned int moved = 0;
    unsigned int elsewhere = 0;
    for (uintptr_t at = ggtt; at < ggtt + igfx.gsm_size; at += 8) {
        uint64_t pte = mmio_read_host(at, 8);
        if ((pte & PTE_PRESENT) == 0
         || (igfx.kind == STOLEN_DSMBASE && (pte & PTE_LM) != 0)) {
            continue;
        }
        uintptr_t address = pte & PTE_ADDRESS;
        if (address < igfx.dsm || address - igfx.dsm >= igfx.dsm_size) {
            elsewhere++;
            continue;
        }
        mmio_write_host(
            at, 8, (pte & ~PTE_ADDRESS) | (igfx.alias + (address - igfx.dsm))
        );
        moved++;
    }
    (void)mmio_read_host(ggtt, 8);
    if (igfx.kind == STOLEN_BSM && !gen9_lp(igfx.f->device_id)) {
        mm_map_device(
            igfx.gttmmadr + MMIO_FLSH_CNTL - MMIO_FLSH_CNTL % PAGE_SIZE,
            PAGE_SIZE
        );
        mmio_write_host(igfx.gttmmadr + MMIO_FLSH_CNTL, 4, FLSH_CNTL_EN);
    }
    time_udelay(SETTLE_US);
    uintptr_t base = igfx.dsm - igfx.gsm_size;
    dma_restore(base, igfx.dsm + igfx.dsm_size);
    printf(
        "Intel graphics: %u GGTT entries moved to the guest's stolen memory, "
        "%u present elsewhere left\n",
        moved, elsewhere
    );
}
