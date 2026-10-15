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
#include <efi.h>
#include <memory.h>
#include <core.h>
#include <handoff.h>
#include <lib/log.h>
#include <lib/time.h>
#include <sys/cpu.h>
#include <sys/mm.h>
#include <sys/x86.h>
#include <virt/gpt.h>
#include <virt/vcpu.h>
#include <virt/virt.h>

struct handoff *core_handoff;

extern const uint8_t test_guest_start[];
extern const uint8_t test_guest_hypercall[];
extern const uint8_t test_guest_end[];

#define TEST_GUEST_SEGMENT 0x0800

static bool efi_type_is_ram(uint32_t type) {
    switch (type) {
        case EfiLoaderCode:
        case EfiLoaderData:
        case EfiBootServicesCode:
        case EfiBootServicesData:
        case EfiRuntimeServicesCode:
        case EfiRuntimeServicesData:
        case EfiConventionalMemory:
        case EfiACPIReclaimMemory:
        case EfiACPIMemoryNVS:
        case EfiPersistentMemory: {
            return true;
        }
        default: {
            return false;
        }
    }
}

// Everything is mapped one to one, uncached unless the memory map says it is
// RAM, except the hypervisor's own memory.
static void guest_memory_setup(struct handoff *handoff) {
    gpt_map(0, 0, handoff->map_top, GPT_RWX, MT_UC);

    for (uint64_t off = 0; off + handoff->efi_mmap_desc_size <= handoff->efi_mmap_size;
         off += handoff->efi_mmap_desc_size) {
        EFI_MEMORY_DESCRIPTOR *desc = (EFI_MEMORY_DESCRIPTOR *)(uintptr_t)(handoff->efi_mmap + off);
        bool ram = efi_type_is_ram(desc->Type)
                   || (desc->Type == EfiReservedMemoryType && (desc->Attribute & EFI_MEMORY_WB));
        if (ram) {
            gpt_map(desc->PhysicalStart, desc->PhysicalStart, desc->NumberOfPages * PAGE_SIZE,
                    GPT_RWX, MT_WB);
        }
    }

    for (uint32_t i = 0; i < handoff->reserved_count; i++) {
        gpt_map(handoff->reserved[i].base, 0, handoff->reserved[i].size, 0, 0);
    }
}

static void bsp_main(void *arg) {
    struct handoff *handoff = arg;
    struct cpu *cpu = this_cpu();

    if (!virt_init()) {
        panic("Neither VMX nor SVM is usable on this processor\n");
    }
    cpu_tables_load(cpu, virt_vmx);
    virt_cpu_init();

    guest_memory_setup(handoff);

    struct vcpu *v = vcpu_create(cpu);

    uint8_t *program = (uint8_t *)(uintptr_t)(TEST_GUEST_SEGMENT << 4);
    memcpy(program, test_guest_start, test_guest_end - test_guest_start);
    if (!virt_vmx) {
        // VMMCALL in place of VMCALL.
        program[test_guest_hypercall - test_guest_start + 2] = 0xd9;
    }
    vcpu_set_real_mode(v, TEST_GUEST_SEGMENT, 0);

    vcpu_run(v);
}

void core_main(struct handoff *handoff) {
    core_handoff = handoff;

    printf("ReBIOS core running, heap %#llx-%#llx\n",
           (unsigned long long)handoff->heap_base,
           (unsigned long long)(handoff->heap_base + handoff->heap_size));

    mm_init(handoff->heap_base, handoff->heap_size);
    time_init(handoff->tsc_hz);
    printf("TSC at %llu MHz\n", (unsigned long long)(handoff->tsc_hz / 1000000));

    struct cpu *bsp = cpu_area_alloc(0, 0, true);
    switch_stack(cpu_stack_top(bsp), bsp_main, handoff);
}
