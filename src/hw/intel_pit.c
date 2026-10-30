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
#include <hw/intel_pit.h>
#include <core.h>
#include <hw/pci_cfg.h>
#include <lib/log.h>
#include <sys/x86.h>

#define INTEL_VENDOR_ID 0x8086

#define ITSSPRC 0x3300
#define ITSSPRC_CGE8254 ((uint32_t)1 << 2)

#define P2SB_SBREG_BAR 0x10
#define P2SB_SBREG_BARH 0x14
// Bits 15:8 of P2SBC, at E0h, of which bit 8 hides the device. The byte is
// written on its own, as a hidden P2SB's registers read as all ones.
#define P2SBC_HIDE_BYTE 0xe1
#define P2SBC_HIDE_BYTE_HIDE ((uint32_t)1 << 0)

struct pch_family {
    uint8_t model;
    uint32_t sideband_base;
    uint8_t itss_port_id;
    uint8_t p2sb_device;
    uint8_t p2sb_function;
};

// By processor model, the sideband register base to assume when the P2SB's BAR
// cannot be read, the ITSS's port ID, and where on bus 0 the P2SB is.
static const struct pch_family pch_families[] = {
    // Apollo Lake and Gemini Lake.
    { 0x5c, 0xd0000000, 0xd0, 13, 0 },
    { 0x7a, 0xd0000000, 0xd0, 13, 0 },
    // Elkhart Lake, Jasper Lake, and Alder Lake-N.
    { 0x96, 0xfd000000, 0xc4, 31, 1 },
    { 0x9c, 0xfd000000, 0xc4, 31, 1 },
    { 0xbe, 0xfd000000, 0xc4, 31, 1 },
    // Skylake through Tiger Lake, and mobile Alder Lake and Raptor Lake.
    { 0x4e, 0xfd000000, 0xc4, 31, 1 },
    { 0x5e, 0xfd000000, 0xc4, 31, 1 },
    { 0x55, 0xfd000000, 0xc4, 31, 1 },
    { 0x8e, 0xfd000000, 0xc4, 31, 1 },
    { 0x9e, 0xfd000000, 0xc4, 31, 1 },
    { 0xa5, 0xfd000000, 0xc4, 31, 1 },
    { 0xa6, 0xfd000000, 0xc4, 31, 1 },
    { 0x7e, 0xfd000000, 0xc4, 31, 1 },
    { 0x8c, 0xfd000000, 0xc4, 31, 1 },
    { 0x8d, 0xfd000000, 0xc4, 31, 1 },
    { 0x9a, 0xfd000000, 0xc4, 31, 1 },
    { 0xba, 0xfd000000, 0xc4, 31, 1 },
    // Desktop Alder Lake and Raptor Lake.
    { 0x97, 0xe0000000, 0xc4, 31, 1 },
    { 0xb7, 0xe0000000, 0xc4, 31, 1 },
    { 0xbf, 0xe0000000, 0xc4, 31, 1 },
    // Meteor Lake, Arrow Lake, and Lunar Lake.
    { 0xac, 0xe0000000, 0xca, 31, 1 },
    { 0xaa, 0xe0000000, 0xca, 31, 1 },
    { 0xc6, 0xe0000000, 0xca, 31, 1 },
    { 0xc5, 0xe0000000, 0xca, 31, 1 },
    { 0xb5, 0xe0000000, 0xca, 31, 1 },
    { 0xbd, 0xe0000000, 0xca, 31, 1 },
    // Panther Lake.
    { 0xcc, 0xe0000000, 0x69, 31, 1 },
};

static uintptr_t pcr_address(
    uintptr_t base, uint8_t port_id, uint32_t offset
) {
    return base | ((uint64_t)port_id << 16) | (offset & 0xffff)
        | ((uint64_t)((offset >> 16) & 0xf) << 24);
}

static uintptr_t sideband_base(const struct pch_family *pch) {
    struct pci_addr p2sb = {
        .segment = 0,
        .bus = 0,
        .device = pch->p2sb_device,
        .function = pch->p2sb_function,
    };
    bool hidden = pci_cfg_read(p2sb, 0, 2) == 0xffff;
    if (hidden) {
        pci_cfg_write(p2sb, P2SBC_HIDE_BYTE, 1, 0);
    }

    uintptr_t base = 0;
    if (pci_cfg_read(p2sb, 0, 2) != 0xffff) {
        uint32_t low = pci_cfg_read(p2sb, P2SB_SBREG_BAR, 4);
        uint32_t high = pci_cfg_read(p2sb, P2SB_SBREG_BARH, 4);
        base = ((uint64_t)high << 32) | (low & ~(uint32_t)0xf);
    }

    if (hidden) {
        pci_cfg_write(p2sb, P2SBC_HIDE_BYTE, 1, P2SBC_HIDE_BYTE_HIDE);
    }

    if (base == 0) {
        printf(
            "8254 clock gate: P2SB BAR unavailable, assuming sideband "
            "registers at %#x\n",
            pch->sideband_base
        );
        return pch->sideband_base;
    }
    printf(
        "8254 clock gate: sideband registers at %#llx\n",
        (unsigned long long)base
    );
    return base;
}

void intel_pit_ungate(void) {
    struct cpuid_regs vendor = cpuid(0, 0);
    bool intel = vendor.ebx == 0x756e6547 && vendor.edx == 0x49656e69
        && vendor.ecx == 0x6c65746e;
    struct cpuid_regs leaf1 = cpuid(1, 0);
    unsigned int family = (leaf1.eax >> 8) & 0xf;
    unsigned int model = (leaf1.eax >> 4) & 0xf;
    if (family == 0xf) {
        family += (leaf1.eax >> 20) & 0xff;
    }
    if (family == 6 || family >= 0xf) {
        model |= (leaf1.eax >> 12) & 0xf0;
    }
    struct pci_addr host_bridge = {
        .segment = 0, .bus = 0, .device = 0, .function = 0
    };
    uint32_t host_vendor = pci_cfg_read(host_bridge, 0, 2);

    const struct pch_family *pch = NULL;
    if (intel && family == 6 && host_vendor == INTEL_VENDOR_ID) {
        for (size_t i = 0;
             i < sizeof(pch_families) / sizeof(struct pch_family); i++) {
            if (pch_families[i].model == model) {
                pch = &pch_families[i];
                break;
            }
        }
    }
    if (pch == NULL) {
        printf(
            "8254 clock gate: not a chipset known to have one (%s processor "
            "family %#x model %#x, host bridge vendor %#x), doing nothing\n",
            intel ? "Intel" : "non-Intel", family, model, host_vendor
        );
        return;
    }
    // A virtual machine's chipset has no sideband registers to find.
    if ((leaf1.ecx & ((uint32_t)1 << 31)) != 0) {
        printf("8254 clock gate: running under a hypervisor, doing nothing\n");
        return;
    }

    uintptr_t address = pcr_address(
        sideband_base(pch), pch->itss_port_id, ITSSPRC
    );
    if (address > core_handoff->map_top - 4) {
        printf(
            "8254 clock gate: ITSSPRC at %#llx is out of reach, doing "
            "nothing\n",
            (unsigned long long)address
        );
        return;
    }

    volatile uint32_t *itssprc = (volatile uint32_t *)(uintptr_t)address;
    uint32_t before = *itssprc;
    if (before == 0xffffffff) {
        printf(
            "8254 clock gate: ITSSPRC at %#llx reads all ones, doing "
            "nothing\n",
            (unsigned long long)address
        );
        return;
    }
    if ((before & ITSSPRC_CGE8254) != 0) {
        *itssprc = before & ~ITSSPRC_CGE8254;
    }
    printf(
        "8254 clock gate: ITSSPRC at %#llx was %#x, now %#x\n",
        (unsigned long long)address, before, *itssprc
    );
}
