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
#include <efi.h>
#include <sys/efi_runtime.h>
#include <core.h>
#include <lib/spinlock.h>
#include <lib/util.h>
#include <memory.h>
#include <sys/fixup.h>
#include <sys/mm.h>
#include <sys/x86.h>

#define FXSAVE_SIZE 512
// Every SSE exception masked, as the processor's reset leaves MXCSR.
#define MXCSR_RESET 0x1f80
// QueryVariableInfo() came with UEFI 2.0, and older firmware's table ends
// before it.
#define QUERY_VARIABLE_INFO_END \
    (offsetof(EFI_RUNTIME_SERVICES, QueryVariableInfo) + sizeof(EFI_QUERY_VARIABLE_INFO))

static struct spinlock lock;
static uint8_t *fpu_area;
static bool fpu_xsave;
// XCR0 as the guest has it, while a call runs.
static uint64_t fpu_xcr0;

// The core never asks for virtual addresses (UEFI 2.11, 8.4.1), so the
// services run at the physical addresses that its identity map gives them.
static EFI_RUNTIME_SERVICES *services(void) {
    return (EFI_RUNTIME_SERVICES *)(uintptr_t)core_handoff->efi_rt;
}

void efi_runtime_init(void) {
    fpu_xsave = (read_cr4() & CR4_OSXSAVE) != 0;
    // XSAVE's largest area, as the guest's XSETBV changes how much is in use.
    size_t size = fpu_xsave ? cpuid(0xd, 0).ecx : FXSAVE_SIZE;
    fpu_area = page_alloc(page_count(size));
    if (fpu_area != NULL) {
        memset(fpu_area, 0, page_count(size) * PAGE_SIZE);
    }
}

// The guest's x87, SSE, and AVX state stays in the processor while the
// hypervisor runs, and the firmware's code may use those registers, so they
// are saved around each call, and the firmware gets their reset values.
static bool begin(bool wait) {
    if (fpu_area == NULL || services() == NULL) {
        return false;
    }
    if (wait) {
        spinlock_acquire(&lock);
    } else if (!spinlock_try(&lock)) {
        return false;
    }
    if (fpu_xsave) {
        // XSAVE takes only the state XCR0 enables, which may leave out the SSE
        // state the firmware's code may change.
        fpu_xcr0 = xgetbv(0);
        if ((fpu_xcr0 & XCR0_SSE) == 0) {
            xsetbv_safe(0, fpu_xcr0 | XCR0_SSE);
        }
        xsave(fpu_area, ~(uint64_t)0);
    } else {
        fxsave(fpu_area);
    }
    fninit();
    ldmxcsr(MXCSR_RESET);
    return true;
}

static void end(void) {
    if (fpu_xsave) {
        xrstor(fpu_area, ~(uint64_t)0);
        if ((fpu_xcr0 & XCR0_SSE) == 0) {
            xsetbv_safe(0, fpu_xcr0);
        }
    } else {
        fxrstor(fpu_area);
    }
    spinlock_release(&lock);
}

EFI_STATUS efi_runtime_query_variable_info(
    EFI_UINT32 attributes, EFI_UINT64 *storage, EFI_UINT64 *remaining, EFI_UINT64 *max_size,
    bool wait
) {
    const EFI_RUNTIME_SERVICES *rt = services();
    if (rt == NULL || rt->Hdr.Revision < EFI_2_00_SYSTEM_TABLE_REVISION
     || rt->Hdr.HeaderSize < QUERY_VARIABLE_INFO_END) {
        return EFI_UNSUPPORTED;
    }
    if (!begin(wait)) {
        return EFI_NOT_READY;
    }
    EFI_STATUS status = rt->QueryVariableInfo(attributes, storage, remaining, max_size);
    end();
    return status;
}

EFI_STATUS efi_runtime_set_variable(
    EFI_CHAR16 *name, EFI_GUID *guid, EFI_UINT32 attributes, EFI_UINTN size, void *data, bool wait
) {
    if (!begin(wait)) {
        return EFI_NOT_READY;
    }
    EFI_STATUS status = services()->SetVariable(name, guid, attributes, size, data);
    end();
    return status;
}
