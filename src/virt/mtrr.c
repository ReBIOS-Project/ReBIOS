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
#include <stddef.h>
#include <stdbool.h>
#include <sys/x86.h>
#include <virt/mtrr.h>
#include <virt/pci_plan.h>
#include <virt/vcpu.h>
#include <virt/virt.h>

// SDM 325462-093 Vol. 4, Table 2-2.
#define MSR_MTRRCAP 0xfe
#define MSR_PHYSBASE0 0x200
#define MSR_VARIABLE_END 0x220
#define MSR_FIX64K_00000 0x250
#define MSR_FIX16K_80000 0x258
#define MSR_FIX16K_A0000 0x259
#define MSR_FIX4K_C0000 0x268
#define MSR_FIX4K_F8000 0x26f
#define MSR_DEF_TYPE 0x2ff

// SDM 325462-093 Vol. 3A, 14.11.1 and 14.11.2: the variable ranges' count,
// fixed ranges, and write-combining; the default type's register.
#define MTRRCAP_FIX (1u << 8)
#define MTRRCAP_WC (1u << 10)
#define DEF_TYPE_FE (1u << 10)
#define DEF_TYPE_E (1u << 11)
#define DEF_TYPE_RESERVED (~(uint64_t)0xcff)
#define PHYSBASE_RESERVED_LOW 0xf00ull
#define PHYSMASK_VALID (1ull << 11)
#define PHYSMASK_RESERVED_LOW 0x7ffull

#define TYPE_UC 0
#define TYPE_WB 6
#define FIXED_WB 0x0606060606060606ull
#define FIXED_UC 0

#define FIXED_VGA 2
#define FOUR_GIB 0x100000000ull

// Bits MAXPHYADDR-1:12.
static uint64_t address_mask;

// SDM 325462-093 Vol. 3A, 14.11.2.1: 0, 1, 4, 5, and 6.
static bool type_valid(uint8_t type) {
    return type == 0 || type == 1 || type == 4 || type == 5 || type == 6;
}

static int fixed_index(uint32_t msr) {
    switch (msr) {
        case MSR_FIX64K_00000: {
            return 0;
        }
        case MSR_FIX16K_80000: {
            return 1;
        }
        case MSR_FIX16K_A0000: {
            return 2;
        }
        default: {
            if (msr >= MSR_FIX4K_C0000 && msr <= MSR_FIX4K_F8000) {
                return 3 + (int)(msr - MSR_FIX4K_C0000);
            }
            return -1;
        }
    }
}

void mtrr_init(void) {
    unsigned bits = cpuid(0x80000000, 0).eax >= 0x80000008 ? cpuid(0x80000008, 0).eax & 0xff : 36;
    address_mask = ((1ull << bits) - 1) & ~0xfffull;
    virt_intercept_msr(MSR_MTRRCAP, true, true);
    virt_intercept_msr(MSR_DEF_TYPE, true, true);
    // Those beyond the guest's count too, which the host may have.
    for (uint32_t msr = MSR_PHYSBASE0; msr < MSR_VARIABLE_END; msr++) {
        virt_intercept_msr(msr, true, true);
    }
    virt_intercept_msr(MSR_FIX64K_00000, true, true);
    virt_intercept_msr(MSR_FIX16K_80000, true, true);
    virt_intercept_msr(MSR_FIX16K_A0000, true, true);
    for (uint32_t msr = MSR_FIX4K_C0000; msr <= MSR_FIX4K_F8000; msr++) {
        virt_intercept_msr(msr, true, true);
    }
}

void mtrr_vcpu_init(struct vcpu *v) {
    v->mtrr_def_type = DEF_TYPE_E | DEF_TYPE_FE | TYPE_WB;
    for (unsigned i = 0; i < MTRR_FIXED_COUNT; i++) {
        v->mtrr_fixed[i] = FIXED_WB;
    }
    v->mtrr_fixed[FIXED_VGA] = FIXED_UC;
    for (unsigned i = 0; i < 2 * MTRR_VARIABLE_COUNT; i++) {
        v->mtrr_variable[i] = 0;
    }
    // The PCI hole in the largest aligned pieces that fit.
    const struct pci_plan *plan = pci_plan();
    uint64_t base = plan != NULL ? plan->tolud : FOUR_GIB;
    for (unsigned n = 0; base < FOUR_GIB && n < MTRR_VARIABLE_COUNT; n++) {
        uint64_t size = base & -base;
        while (base + size > FOUR_GIB) {
            size >>= 1;
        }
        v->mtrr_variable[2 * n] = base | TYPE_UC;
        v->mtrr_variable[2 * n + 1] = (~(size - 1) & address_mask) | PHYSMASK_VALID;
        base += size;
    }
}

bool mtrr_rdmsr(struct vcpu *v, uint32_t msr, uint64_t *value, bool *fault) {
    *fault = false;
    int fixed = fixed_index(msr);
    if (msr == MSR_MTRRCAP) {
        *value = MTRR_VARIABLE_COUNT | MTRRCAP_FIX | MTRRCAP_WC;
    } else if (msr == MSR_DEF_TYPE) {
        *value = v->mtrr_def_type;
    } else if (fixed >= 0) {
        *value = v->mtrr_fixed[fixed];
    } else if (msr >= MSR_PHYSBASE0 && msr < MSR_VARIABLE_END) {
        uint32_t i = msr - MSR_PHYSBASE0;
        if (i < 2 * MTRR_VARIABLE_COUNT) {
            *value = v->mtrr_variable[i];
        } else {
            *fault = true;
        }
    } else {
        return false;
    }
    return true;
}

bool mtrr_wrmsr(struct vcpu *v, uint32_t msr, uint64_t value, bool *fault) {
    *fault = false;
    int fixed = fixed_index(msr);
    if (msr == MSR_MTRRCAP) {
        *fault = true;
    } else if (msr == MSR_DEF_TYPE) {
        if ((value & DEF_TYPE_RESERVED) || !type_valid((uint8_t)value)) {
            *fault = true;
        } else {
            v->mtrr_def_type = value;
        }
    } else if (fixed >= 0) {
        for (unsigned b = 0; b < 8; b++) {
            if (!type_valid((uint8_t)(value >> (8 * b)))) {
                *fault = true;
            }
        }
        if (!*fault) {
            v->mtrr_fixed[fixed] = value;
        }
    } else if (msr >= MSR_PHYSBASE0 && msr < MSR_VARIABLE_END) {
        uint32_t i = msr - MSR_PHYSBASE0;
        bool is_mask = i % 2 != 0;
        uint64_t reserved = is_mask ? PHYSMASK_RESERVED_LOW | ~(address_mask | PHYSMASK_VALID)
                                    : PHYSBASE_RESERVED_LOW | ~(address_mask | 0xff);
        if (i >= 2 * MTRR_VARIABLE_COUNT || (value & reserved)
            || (!is_mask && !type_valid((uint8_t)value))) {
            *fault = true;
        } else {
            v->mtrr_variable[i] = value;
        }
    } else {
        return false;
    }
    return true;
}
