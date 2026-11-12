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
#include <virt/devmem.h>
#include <hw/pci_host.h>
#include <lib/heap.h>
#include <lib/log.h>
#include <lib/spinlock.h>
#include <lib/util.h>
#include <sys/x86.h>
#include <virt/gpt.h>
#include <virt/layout.h>

// The block ends below the I/O APICs' area, as a chipset's fixed ranges do,
// and keeps room for memory that the firmware's AML gives later.
#define BLOCK_TOP ((uintptr_t)0xfec00000)
#define BLOCK_ALIGN ONE_MIB
#define SPARE ONE_MIB
// The largest range a device gets: more is no device's registers.
#define RANGE_MAX ((size_t)0x4000000)
// TCG PC Client Platform Firmware Profile 1.06 r52, 9: a TPM's registers at
// FED40000h-FED44FFFh, which Linux 7.3's drivers/char/tpm/tpm_tis.c also takes
// them to be at.
#define TPM_BASE ((uintptr_t)0xfed40000)
#define TPM_SIZE ((size_t)0x5000)

// Host memory at a guest address.
struct placed {
    uintptr_t host;
    uintptr_t guest;
    size_t size;
};

static struct {
    struct placed *ranges;
    size_t count;
    size_t capacity;
    // How many of the ranges devmem_map() has mapped.
    size_t mapped;
    uintptr_t block_base;
    uintptr_t block_end;
    // Where the block's spare room starts, and where it started.
    uintptr_t spare;
    uintptr_t first_spare;
    uintptr_t tpm_base;
    uintptr_t tpm_end;
    struct spinlock lock;
} devmem;

static void place(uintptr_t host, uintptr_t guest, size_t size) {
    devmem.ranges = heap_grow(
        devmem.ranges, devmem.count, &devmem.capacity, sizeof(struct placed), 16
    );
    devmem.ranges[devmem.count++] = (struct placed){ host, guest, size };
}

static bool overlaps(
    const struct pci_host_range *list,
    size_t count,
    uintptr_t base,
    uintptr_t end
) {
    for (size_t i = 0; i < count; i++) {
        if (base < list[i].end && end > list[i].base) {
            return true;
        }
    }
    return false;
}

// Whether the guest may get the host's [base, end): none of the guest's legacy
// memory, the host's RAM, the configuration space windows, or the IOMMUs.
static bool usable(uintptr_t base, uintptr_t end) {
    const struct pci_host *host = pci_host_scan();
    if (base < ONE_MIB || end <= base || end - base > RANGE_MAX || !layout_host_kept(base, end)
     || overlaps(host->private, host->private_count, base, end)) {
        printf(
            "Device memory at 0x%llx-0x%llx is not the guest's\n", (unsigned long long)base,
            (unsigned long long)(end - 1)
        );
        return false;
    }
    return true;
}

// Whether a TPM's range is in the registers' span from the TPM's first.
static bool tpm_span(const struct pci_host *host, const struct pci_host_range *r) {
    return r->base >= host->tpms[0].base && r->end - host->tpms[0].base <= TPM_SIZE;
}

void devmem_plan(void) {
    const struct pci_host *host = pci_host_scan();
    size_t total = 0;
    for (size_t i = 0; i < host->device_count; i++) {
        total += host->devices[i].end - host->devices[i].base;
    }
    for (size_t i = 0; i < host->tpm_count; i++) {
        if (!tpm_span(host, &host->tpms[i])) {
            total += host->tpms[i].end - host->tpms[i].base;
        }
    }
    size_t size = align_up(total + SPARE, BLOCK_ALIGN);
    uintptr_t top = BLOCK_TOP;
    for (bool moved = true; moved; ) {
        moved = false;
        for (size_t i = 0; i < host->interrupt_count; i++) {
            if (host->interrupts[i].base < top && host->interrupts[i].end > top - size) {
                top = align_down(host->interrupts[i].base, BLOCK_ALIGN);
                moved = true;
            }
        }
    }
    devmem.block_base = top - size;
    devmem.block_end = top;

    uintptr_t at = devmem.block_base;
    for (size_t i = 0; i < host->device_count; i++) {
        const struct pci_host_range *r = &host->devices[i];
        if (usable(r->base, r->end)) {
            place(r->base, at, r->end - r->base);
            at += r->end - r->base;
        }
    }
    for (size_t i = 0; i < host->tpm_count; i++) {
        const struct pci_host_range *r = &host->tpms[i];
        if (!usable(r->base, r->end)) {
            continue;
        }
        if (tpm_span(host, r)) {
            uintptr_t guest = TPM_BASE + (r->base - host->tpms[0].base);
            place(r->base, guest, r->end - r->base);
            devmem.tpm_base = TPM_BASE;
            if (guest + (r->end - r->base) > devmem.tpm_end) {
                devmem.tpm_end = guest + (r->end - r->base);
            }
        } else {
            place(r->base, at, r->end - r->base);
            at += r->end - r->base;
        }
    }
    devmem.spare = at;
    devmem.first_spare = at;
    printf(
        "Guest device memory: %zu ranges at 0x%llx-0x%llx%s\n", devmem.count,
        (unsigned long long)devmem.block_base, (unsigned long long)(devmem.block_end - 1),
        devmem.tpm_end != 0 ? ", and a TPM's at its own address" : ""
    );
}

bool devmem_range(size_t i, uintptr_t *base, uintptr_t *end) {
    if (i == 0 && devmem.block_end != 0) {
        *base = devmem.block_base;
        *end = devmem.block_end;
        return true;
    }
    if (i == 1 && devmem.tpm_end != 0) {
        *base = devmem.tpm_base;
        *end = devmem.tpm_end;
        return true;
    }
    return false;
}

bool devmem_spare(uintptr_t *base, uintptr_t *end) {
    *base = devmem.first_spare;
    *end = devmem.block_end;
    return devmem.block_end != 0;
}

void devmem_map(void) {
    for (; devmem.mapped < devmem.count; devmem.mapped++) {
        const struct placed *p = &devmem.ranges[devmem.mapped];
        gpt_map(p->guest, p->host, p->size, GPT_RWX, MT_UC);
    }
}

bool devmem_interrupts(uint64_t base, uint64_t end) {
    const struct pci_host *host = pci_host_scan();
    for (size_t i = 0; i < host->interrupt_count; i++) {
        if (base >= host->interrupts[i].base && end <= host->interrupts[i].end) {
            return true;
        }
    }
    return false;
}

// Gives the host's pages of [base, end) an address in the block's spare room.
static bool place_late(uint64_t base, uint64_t end, uint64_t *guest) {
    uintptr_t first = page_down(base);
    uintptr_t last = page_up(end);
    if (!usable(first, last)) {
        return false;
    }
    if (last - first > devmem.block_end - devmem.spare) {
        printf(
            "No room in the guest for device memory at 0x%llx-0x%llx\n",
            (unsigned long long)base, (unsigned long long)(end - 1)
        );
        return false;
    }
    place(first, devmem.spare, last - first);
    devmem_map();
    gpt_flush();
    printf(
        "Device memory at 0x%llx-0x%llx is the guest's at 0x%llx\n", (unsigned long long)first,
        (unsigned long long)(last - 1), (unsigned long long)devmem.spare
    );
    *guest = devmem.spare + (base - first);
    devmem.spare += last - first;
    return true;
}

bool devmem_guest(uint64_t base, uint64_t end, bool late, uint64_t *guest) {
    if (devmem_interrupts(base, end)) {
        *guest = base;
        return true;
    }
    bool found = false;
    spinlock_acquire(&devmem.lock);
    for (size_t i = 0; i < devmem.count && !found; i++) {
        const struct placed *p = &devmem.ranges[i];
        if (base >= p->host && end <= p->host + p->size) {
            *guest = p->guest + (base - p->host);
            found = true;
        }
    }
    if (!found && late && devmem.block_end != 0) {
        found = place_late(base, end, guest);
    }
    spinlock_release(&devmem.lock);
    return found;
}

bool devmem_host(uint64_t base, uint64_t end, uint64_t *host) {
    if (devmem_interrupts(base, end)) {
        *host = base;
        return true;
    }
    bool found = false;
    spinlock_acquire(&devmem.lock);
    for (size_t i = 0; i < devmem.count && !found; i++) {
        const struct placed *p = &devmem.ranges[i];
        if (base >= p->guest && end <= p->guest + p->size) {
            *host = p->host + (base - p->guest);
            found = true;
        }
    }
    spinlock_release(&devmem.lock);
    return found;
}
