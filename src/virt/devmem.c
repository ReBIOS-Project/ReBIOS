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
#include <virt/devmem.h>
#include <hw/pci_host.h>
#include <lib/heap.h>
#include <lib/log.h>
#include <lib/range.h>
#include <lib/spinlock.h>
#include <lib/util.h>
#include <memory.h>
#include <sys/mm.h>
#include <sys/x86.h>
#include <virt/gpt.h>
#include <virt/layout.h>
#include <virt/mmio.h>
#include <virt/vcpu.h>

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

// How host memory is at a guest address: mapped there, trapped, or mapped for
// the guest to read.
#define PLACED_MAPPED 0
#define PLACED_TRAPPED 1
#define PLACED_READ_ONLY 2

struct placed {
    uintptr_t host;
    uintptr_t guest;
    size_t size;
    int placing;
};

// The most ranges that devmem_add_trapped() takes.
#define TRAPPED_MAX 4

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
    struct pci_host_range trapped[TRAPPED_MAX];
    size_t trapped_count;
    struct range_list read_only;
    struct spinlock lock;
} devmem;

static void place(uintptr_t host, uintptr_t guest, size_t size, int placing) {
    devmem.ranges = heap_grow(
        devmem.ranges, devmem.count, &devmem.capacity, sizeof(struct placed), 16
    );
    devmem.ranges[devmem.count++] = (struct placed){ host, guest, size, placing };
}

void devmem_add_trapped(uintptr_t base, uintptr_t end) {
    if (devmem.trapped_count < TRAPPED_MAX) {
        devmem.trapped[devmem.trapped_count++] = (struct pci_host_range){ base, end };
    }
}

void devmem_add_read_only(uintptr_t base, uintptr_t end) {
    range_list_add(&devmem.read_only, page_down(base), page_up(end));
}

// The power of two at or above size.
static size_t power_above(size_t size) {
    size_t power = 1;
    while (power < size) {
        power <<= 1;
    }
    return power;
}

static uint64_t read_only_read(struct vcpu *v, void *ctx, uint64_t offset, size_t size) {
    (void)v;
    uint64_t value = 0;
    memcpy(&value, (const uint8_t *)ctx + offset, size);
    return value;
}

static const struct mmio_ops read_only_ops = {
    .read = read_only_read,
    .write = mmio_absent_write,
    .reads_virtual = true,
    .writes_virtual = true,
    .writes_ignored = true,
    .reads_mapped = true
};

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
static bool allowed(uintptr_t base, uintptr_t end) {
    const struct pci_host *host = pci_host_scan();
    return base >= ONE_MIB && end > base && end - base <= RANGE_MAX && layout_host_kept(base, end)
        && !overlaps(host->private, host->private_count, base, end);
}

static bool usable(uintptr_t base, uintptr_t end) {
    if (!allowed(base, end)) {
        printf(
            "Device memory at 0x%llx-0x%llx is not the guest's\n",
            (unsigned long long)base, (unsigned long long)(end - 1)
        );
        return false;
    }
    return true;
}

// Whether size bytes fit in the block from at, and says so where not.
static bool fits(uintptr_t at, size_t size, uintptr_t base) {
    if (size > devmem.block_end - at) {
        printf(
            "Device memory at 0x%llx-0x%llx does not fit in the guest's block, so it is not "
            "the guest's\n",
            (unsigned long long)base, (unsigned long long)(base + size - 1)
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
    // Firmware data that the guest only reads is mapped for it to write
    // nowhere, whatever device claims it too.
    struct range_list devices = { 0 };
    for (size_t i = 0; i < host->device_count; i++) {
        range_list_add(&devices, host->devices[i].base, host->devices[i].end);
    }
    for (size_t i = 0; i < devmem.read_only.count; i++) {
        range_list_remove(&devices, devmem.read_only.r[i].base, devmem.read_only.r[i].end);
    }
    // Room for the ranges the guest gets alone.
    size_t total = 0;
    for (size_t i = 0; i < devices.count; i++) {
        if (allowed(devices.r[i].base, devices.r[i].end)) {
            total += devices.r[i].end - devices.r[i].base;
        }
    }
    for (size_t i = 0; i < host->tpm_count; i++) {
        const struct pci_host_range *r = &host->tpms[i];
        if (!tpm_span(host, r) && allowed(r->base, r->end)) {
            total += r->end - r->base;
        }
    }
    // Room for each, and for its alignment.
    for (size_t i = 0; i < devmem.trapped_count; i++) {
        const struct pci_host_range *r = &devmem.trapped[i];
        if (allowed(r->base, r->end)) {
            total += power_above(r->end - r->base) + (r->end - r->base);
        }
    }
    for (size_t i = 0; i < devmem.read_only.count; i++) {
        const struct range *r = &devmem.read_only.r[i];
        if (allowed(r->base, r->end)) {
            total += r->end - r->base;
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
    // Above the guest's legacy memory whatever the ranges add up to, those
    // that do not fit left out.
    if (size > top - ONE_MIB) {
        size = top - ONE_MIB;
    }
    devmem.block_base = top - size;
    devmem.block_end = top;

    uintptr_t at = devmem.block_base;
    for (size_t i = 0; i < devices.count; i++) {
        const struct range *r = &devices.r[i];
        if (usable(r->base, r->end) && fits(at, r->end - r->base, r->base)) {
            place(r->base, at, r->end - r->base, PLACED_MAPPED);
            at += r->end - r->base;
        }
    }
    range_list_free(&devices);
    for (size_t i = 0; i < devmem.trapped_count; i++) {
        const struct pci_host_range *r = &devmem.trapped[i];
        uintptr_t aligned = align_up(at, power_above(r->end - r->base));
        if (usable(r->base, r->end) && fits(aligned, r->end - r->base, r->base)) {
            place(r->base, aligned, r->end - r->base, PLACED_TRAPPED);
            at = aligned + (r->end - r->base);
        }
    }
    for (size_t i = 0; i < devmem.read_only.count; i++) {
        const struct range *r = &devmem.read_only.r[i];
        if (usable(r->base, r->end) && fits(at, r->end - r->base, r->base)) {
            place(r->base, at, r->end - r->base, PLACED_READ_ONLY);
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
            place(r->base, guest, r->end - r->base, PLACED_MAPPED);
            devmem.tpm_base = TPM_BASE;
            if (guest + (r->end - r->base) > devmem.tpm_end) {
                devmem.tpm_end = guest + (r->end - r->base);
            }
        } else if (fits(at, r->end - r->base, r->base)) {
            place(r->base, at, r->end - r->base, PLACED_MAPPED);
            at += r->end - r->base;
        }
    }
    devmem.spare = at;
    devmem.first_spare = at;
    printf(
        "Guest device memory: %zu ranges at 0x%llx-0x%llx%s\n",
        devmem.count,
        (unsigned long long)devmem.block_base,
        (unsigned long long)(devmem.block_end - 1),
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
        if (p->placing == PLACED_MAPPED) {
            gpt_map(p->guest, p->host, p->size, GPT_RWX, MT_UC);
        } else if (p->placing == PLACED_READ_ONLY) {
            // Firmware data, in the RAM that the firmware keeps.
            mm_map_device(p->host, p->size);
            if (mmio_register(p->guest, p->size, &read_only_ops, (void *)p->host) < 0) {
                printf(
                    "No room to give the guest the firmware data at 0x%llx-0x%llx\n",
                    (unsigned long long)p->host, (unsigned long long)(p->host + p->size - 1)
                );
                continue;
            }
            gpt_map(p->guest, p->host, p->size, GPT_R, MT_WB);
        }
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

// Gives the host's pages of [base, end) an address in the block's spare room,
// but where the hypervisor answers for any of them, or maps them for the guest
// to read alone.
static bool place_late(uint64_t base, uint64_t end, uint64_t *guest) {
    uintptr_t first = page_down(base);
    uintptr_t last = page_up(end);
    if (!usable(first, last)) {
        return false;
    }
    for (size_t i = 0; i < devmem.count; i++) {
        const struct placed *p = &devmem.ranges[i];
        if (p->placing != PLACED_MAPPED && first < p->host + p->size && last > p->host) {
            printf(
                "Device memory at 0x%llx-0x%llx holds what the guest gets otherwise: it does "
                "not get it there\n",
                (unsigned long long)base, (unsigned long long)(end - 1)
            );
            return false;
        }
    }
    if (last - first > devmem.block_end - devmem.spare) {
        printf(
            "No room in the guest for device memory at 0x%llx-0x%llx\n",
            (unsigned long long)base, (unsigned long long)(end - 1)
        );
        return false;
    }
    place(first, devmem.spare, last - first, PLACED_MAPPED);
    devmem_map();
    gpt_flush();
    printf(
        "Device memory at 0x%llx-0x%llx is the guest's at 0x%llx\n",
        (unsigned long long)first, (unsigned long long)(last - 1), (unsigned long long)devmem.spare
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

bool devmem_trapped_guest(uint64_t base, uint64_t end, uint64_t *guest) {
    bool found = false;
    spinlock_acquire(&devmem.lock);
    for (size_t i = 0; i < devmem.count && !found; i++) {
        const struct placed *p = &devmem.ranges[i];
        if (p->placing == PLACED_TRAPPED && base >= p->host && end <= p->host + p->size) {
            *guest = p->guest + (base - p->host);
            found = true;
        }
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
