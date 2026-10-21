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
#include <memory.h>
#include <csm/e820.h>
#include <hw/pci_cfg.h>
#include <hw/pci_enum.h>
#include <lib/log.h>
#include <sys/mm.h>
#include <virt/gpt.h>
#include <virt/igfx.h>
#include <virt/layout.h>
#include <virt/pci_virt.h>

#define VENDOR_INTEL 0x8086
#define CLASS_DISPLAY 0x03
#define IGFX_DEVICE 2

// Linux 7.3 include/drm/intel/pci_config.h ASLS, and
// drivers/gpu/drm/i915/display/intel_opregion.c: the header's version and
// mailboxes, mailbox 3's RVDA and RVDS, and the 8 KiB a driver maps at ASLS
// whatever the header's size says. RVDA is a physical address in OpRegion 2.0
// and an offset from the OpRegion from 2.1 on.
#define REG_ASLS 0xfc
#define OPREGION_SIZE 0x2000u
#define OPREGION_SIGNATURE "IntelGraphicsMem"
#define SIGNATURE_SIZE 16
#define HEADER_OVER_MINOR 0x16
#define HEADER_OVER_MAJOR 0x17
#define HEADER_MBOXES 0x58
#define MBOX_ASLE (1u << 2)
#define ASLE_RVDA 0x3ba
#define ASLE_RVDS 0x3c2
// A raw VBT larger than this is taken for garbage.
#define RAW_VBT_MAX 0x100000u

static struct {
    struct pci_function *f;
    // The host's OpRegion and the bytes from it the guest may read: a raw VBT
    // that follows it included.
    uint32_t asls;
    uint64_t size;
    uint64_t raw_vbt_base;
    uint64_t raw_vbt_end;
    // What the guest's ASLS holds, which never reaches the hardware: the
    // host's AML finds the OpRegion through the real one.
    uint32_t guest_asls;
} igfx;

static uint64_t page_down(uint64_t x) {
    return x & ~(uint64_t)(PAGE_SIZE - 1);
}

static uint64_t page_up(uint64_t x) {
    return page_down(x + PAGE_SIZE - 1);
}

void igfx_scan(void) {
    struct pci_function *f = pci_enum_find((struct pci_addr){0, 0, IGFX_DEVICE, 0});
    if (f == NULL || f->vendor_id != VENDOR_INTEL || f->base_class != CLASS_DISPLAY) {
        return;
    }
    igfx.f = f;
    uint32_t asls = pci_cfg_read(f->addr, REG_ASLS, 4);
    if (asls == 0 || asls == UINT32_MAX) {
        return;
    }
    const uint8_t *op = (const uint8_t *)(uintptr_t)asls;
    if (memcmp(op, OPREGION_SIGNATURE, SIGNATURE_SIZE) != 0) {
        printf("Intel graphics: no OpRegion at %#x, which ASLS gives\n", asls);
        return;
    }
    uint64_t end = (uint64_t)asls + OPREGION_SIZE;
    uint32_t mboxes;
    uint64_t rvda;
    uint32_t rvds;
    memcpy(&mboxes, op + HEADER_MBOXES, sizeof(mboxes));
    memcpy(&rvda, op + ASLE_RVDA, sizeof(rvda));
    memcpy(&rvds, op + ASLE_RVDS, sizeof(rvds));
    uint8_t major = op[HEADER_OVER_MAJOR];
    uint8_t minor = op[HEADER_OVER_MINOR];
    if (major >= 2 && (mboxes & MBOX_ASLE) != 0 && rvda != 0 && rvds != 0 && rvds <= RAW_VBT_MAX) {
        if (major > 2 || minor >= 1) {
            if (rvda >= OPREGION_SIZE && rvda <= RAW_VBT_MAX && asls + rvda + rvds > end) {
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

bool igfx_one_to_one(uint64_t *base, uint64_t *end) {
    if (igfx.raw_vbt_end == 0) {
        return false;
    }
    *base = igfx.raw_vbt_base;
    *end = igfx.raw_vbt_end;
    return true;
}

static bool config_read(uint32_t reg, uint32_t *value) {
    if (reg != REG_ASLS) {
        return false;
    }
    *value = igfx.guest_asls;
    return true;
}

static bool config_write(uint32_t reg, uint32_t value, uint32_t mask) {
    if (reg != REG_ASLS) {
        return false;
    }
    igfx.guest_asls = (igfx.guest_asls & ~mask) | (value & mask);
    return true;
}

static const struct pci_virt_hook config_hook = {config_read, config_write};

void igfx_init(void) {
    if (igfx.asls == 0) {
        return;
    }
    uint64_t offset = igfx.asls & (PAGE_SIZE - 1);
    uint64_t host = igfx.asls - offset;
    uint64_t size = page_up(offset + igfx.size);
    // Pages that back guest RAM elsewhere would be the guest's twice.
    if (!layout_host_kept(host, host + size)) {
        printf("Intel graphics: the OpRegion at %#x is in memory the guest may have as RAM\n",
               igfx.asls);
        return;
    }
    uint32_t alias = layout_take((uint32_t)size, PAGE_SIZE, E820_NVS);
    if (alias == 0) {
        printf("Intel graphics: no room in the guest's memory for the OpRegion\n");
        return;
    }
    gpt_map(alias, host, size, GPT_R | GPT_W, MT_WB);
    gpt_flush();
    igfx.guest_asls = alias + (uint32_t)offset;
    pci_virt_hook(igfx.f->addr, &config_hook);
    printf("Intel graphics: the OpRegion at %#x is the guest's at %#x\n", igfx.asls,
           igfx.guest_asls);
}
