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
// You should have received a copy of the GNU General Public License along with
// ReBIOS. If not, see <https://www.gnu.org/licenses/>.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <virt/microcode.h>
#include <lib/heap.h>
#include <lib/spinlock.h>
#include <lib/util.h>
#include <sys/fixup.h>
#include <sys/mm.h>
#include <sys/x86.h>
#include <virt/guest_mem.h>
#include <virt/vcpu.h>

#define MSR_IA32_BIOS_UPDT_TRIG 0x79
#define MSR_AMD_PATCH_LOADER 0xc0010020

// An Intel update's header is the 48 bytes before the data that the trigger is
// given the address of, its Data Size and Total Size 0 for 2000 and 2048
// bytes; updates come in whole kilobytes (SDM 325462-093 Vol. 3A, 12.11.1 and
// 12.11.6).
#define INTEL_HEADER 48
#define INTEL_DATA_SIZE 28
#define INTEL_TOTAL_SIZE 32
#define INTEL_DEFAULT_TOTAL 2048
#define INTEL_GRANULE 1024
#define INTEL_UPDATE_MAX ((size_t)1 << 20)

// An AMD patch gives the loader no size, as the processor knows it: the
// guest's pages from the address on are copied, as far as they are mapped, up
// to this.
#define AMD_PATCH_MAX ((size_t)32 << 10)

// One load at a time, as threads that share a core share its update (SDM
// 325462-093 Vol. 3A, 12.11.6.3), its copy made under the lock, so that only
// one is ever held.
static struct spinlock lock;

// The size of the update whose header is at header, or 0 where its sizes are
// not an update's: its data has to fit in it, as Linux 7.3's
// intel_microcode_sanity_check() has it.
static size_t intel_total(const uint8_t *header) {
    uint32_t data = read_le32(header + INTEL_DATA_SIZE);
    if (data == 0) {
        return INTEL_DEFAULT_TOTAL;
    }
    size_t total = read_le32(header + INTEL_TOTAL_SIZE);
    if (total < INTEL_DEFAULT_TOTAL || total % INTEL_GRANULE != 0
     || (size_t)data + INTEL_HEADER > total) {
        return 0;
    }
    return total;
}

static void intel_load(struct vcpu *v, uint64_t data) {
    uint8_t header[INTEL_HEADER];
    if (data < INTEL_HEADER
     || !guest_mem_linear_read(v, data - INTEL_HEADER, header, sizeof(header), false)) {
        return;
    }
    size_t total = intel_total(header);
    if (total == 0 || total > INTEL_UPDATE_MAX) {
        return;
    }
    spinlock_acquire(&lock);
    // kmalloc()'s alignment keeps the data 16-byte aligned, as the trigger
    // needs. The copy's own header is the one the processor reads, so it is
    // the one that has to give the copy's size.
    uint8_t *copy = kmalloc(total);
    if (guest_mem_linear_read(v, data - INTEL_HEADER, copy, total, false)
     && intel_total(copy) == total) {
        wrmsr_safe(MSR_IA32_BIOS_UPDT_TRIG, (uintptr_t)copy + INTEL_HEADER);
    }
    kfree(copy);
    spinlock_release(&lock);
}

static void amd_load(struct vcpu *v, uint64_t patch) {
    spinlock_acquire(&lock);
    uint8_t *copy = kmalloc(AMD_PATCH_MAX);
    size_t size = 0;
    while (size < AMD_PATCH_MAX) {
        size_t chunk = PAGE_SIZE - ((patch + size) & (PAGE_SIZE - 1));
        if (chunk > AMD_PATCH_MAX - size) {
            chunk = AMD_PATCH_MAX - size;
        }
        if (!guest_mem_linear_read(v, patch + size, copy + size, chunk, false)) {
            break;
        }
        size += chunk;
    }
    if (size != 0) {
        wrmsr_safe(MSR_AMD_PATCH_LOADER, (uintptr_t)copy);
        // Family 17h then drops the patch's translations, as Linux 7.3's
        // __apply_microcode_amd() does.
        if (x86_cpu_signature().family == 0x17) {
            for (uintptr_t page = page_down((uintptr_t)copy); page < (uintptr_t)copy + size;
                 page += PAGE_SIZE) {
                invlpg(page);
            }
        }
    }
    kfree(copy);
    spinlock_release(&lock);
}

void microcode_load(struct vcpu *v, uint64_t address) {
    if (x86_cpu_vendor() == X86_CPU_VENDOR_INTEL) {
        intel_load(v, address);
    } else {
        amd_load(v, address);
    }
}
