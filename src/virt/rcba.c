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

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <virt/rcba.h>
#include <hw/pci_cfg.h>
#include <hw/pci_enum.h>
#include <hw/pci_host.h>
#include <hw/pci_regs.h>
#include <lib/log.h>
#include <lib/util.h>
#include <sys/mm.h>
#include <virt/devmem.h>
#include <virt/mmio.h>
#include <virt/pci_virt.h>
#include <virt/vcpu.h>

// Intel 6 Series Chipset and Intel C200 Series Chipset Datasheet 324645-006:
// the LPC bridge's RCBA register (13.1.39), the block's registers (Table
// 10-1), and its SPI registers (21.1).
#define LPC_DEVICE 31
#define REG_RCBA 0xf0
#define RCBA_ENABLE ((uint32_t)1)
#define RCBA_ADDRESS ((uint32_t)0xffffc000)
#define RCBA_SIZE ((size_t)0x4000)
// The page of the block that holds the registers it copies.
#define COPY_PAGE ((uint32_t)0x3000)

struct span {
    uint32_t base;
    uint32_t end;
};

// What reaches the chipset as the guest writes it: GCS, BUC, and the SPI
// controller's registers, which flashing tools drive.
static const struct span passed[] = {
    { 0x3410, 0x3418 },
    { 0x3800, 0x3a00 }
};

// What the guest has its own copy of, as the hypervisor relies on the
// chipset's: TCTL's TCO interrupt, the devices' interrupt pins and routes and
// OIC's I/O APIC enable, HPTC's choice of the HPET's address, and the function
// disable registers, FD, FDSW, and FD2.
static const struct span copied[] = {
    { 0x3000, 0x3001 },
    { 0x3100, 0x3200 },
    { 0x3404, 0x3408 },
    { 0x3418, 0x341c },
    { 0x3420, 0x3421 },
    { 0x3428, 0x342c }
};

static struct {
    bool found;
    struct pci_cfg_addr lpc;
    uintptr_t host;
    uint64_t guest;
    uint8_t copy[PAGE_SIZE];
    _Atomic(bool) dropped;
} rcba;

static bool in_span(const struct span *list, size_t count, uint64_t at) {
    for (size_t i = 0; i < count; i++) {
        if (at >= list[i].base && at < list[i].end) {
            return true;
        }
    }
    return false;
}

static uint64_t rcba_read(struct vcpu *v, void *ctx, uint64_t offset, size_t size) {
    (void)v;
    (void)ctx;
    uint64_t value = mmio_read_host(rcba.host + offset, size);
    for (size_t i = 0; i < size; i++) {
        if (in_span(copied, ARRAY_COUNT(copied), offset + i)) {
            uint64_t byte = rcba.copy[offset + i - COPY_PAGE];
            value = (value & ~((uint64_t)0xff << (8 * i))) | byte << (8 * i);
        }
    }
    return value;
}

static void rcba_write(struct vcpu *v, void *ctx, uint64_t offset, size_t size, uint64_t value) {
    (void)v;
    (void)ctx;
    bool passes = true;
    bool dropped = false;
    for (size_t i = 0; i < size; i++) {
        if (in_span(copied, ARRAY_COUNT(copied), offset + i)) {
            rcba.copy[offset + i - COPY_PAGE] = (uint8_t)(value >> (8 * i));
            passes = false;
        } else if (!in_span(passed, ARRAY_COUNT(passed), offset + i)) {
            passes = false;
            dropped = true;
        }
    }
    if (passes) {
        mmio_write_host(rcba.host + offset, size, value);
    }
    if (dropped && !atomic_exchange_explicit(&rcba.dropped, true, memory_order_relaxed)) {
        printf(
            "The guest's writes to the chipset configuration registers at +%#llx and the like "
            "do not reach the chipset\n",
            (unsigned long long)offset
        );
    }
}

static const struct mmio_ops rcba_ops = { .read = rcba_read, .write = rcba_write };

// The LPC bridge's RCBA register, as the guest sees it: the block where the
// guest finds it, which stays there.
static bool lpc_read(void *ctx, uint32_t reg, uint32_t *value) {
    (void)ctx;
    if (reg != REG_RCBA) {
        return false;
    }
    *value = (uint32_t)rcba.guest | (pci_cfg_read(rcba.lpc, REG_RCBA, 4) & ~RCBA_ADDRESS);
    return true;
}

static bool lpc_write(void *ctx, uint32_t reg, uint32_t value, uint32_t mask) {
    (void)ctx;
    (void)value;
    (void)mask;
    return reg == REG_RCBA;
}

static const struct pci_virt_hook lpc_hook = { lpc_read, lpc_write };

static bool within(const struct pci_host_range *list, size_t count, uintptr_t base, size_t size) {
    for (size_t i = 0; i < count; i++) {
        if (base >= list[i].base && base + size <= list[i].end) {
            return true;
        }
    }
    return false;
}

void rcba_find(void) {
    struct pci_cfg_addr lpc = { 0, 0, LPC_DEVICE, 0 };
    const struct pci_enum_function *f = pci_enum_find(lpc);
    if (f == NULL || f->vendor_id != PCI_VENDOR_ID_INTEL || f->base_class != 0x06
     || f->subclass != 0x01) {
        return;
    }
    uint32_t reg = pci_cfg_read(lpc, REG_RCBA, 4);
    uintptr_t base = reg & RCBA_ADDRESS;
    if ((reg & RCBA_ENABLE) == 0 || base == 0) {
        return;
    }
    const struct pci_host *host = pci_host_scan();
    if (!within(host->motherboard, host->motherboard_count, base, RCBA_SIZE)
     || within(host->devices, host->device_count, base, RCBA_SIZE)) {
        printf(
            "Chipset configuration registers at %#llx: no motherboard resource of the "
            "firmware's ACPI claims them, so the guest does not get them\n",
            (unsigned long long)base
        );
        return;
    }
    rcba.found = true;
    rcba.lpc = lpc;
    rcba.host = base;
    devmem_add_trapped(base, base + RCBA_SIZE);
}

void rcba_init(void) {
    uint64_t guest;
    if (!rcba.found || !devmem_trapped_guest(rcba.host, rcba.host + RCBA_SIZE, &guest)) {
        return;
    }
    mm_map_device(rcba.host, RCBA_SIZE);
    for (size_t i = 0; i < ARRAY_COUNT(copied); i++) {
        for (size_t at = copied[i].base; at < copied[i].end; at++) {
            rcba.copy[at - COPY_PAGE] = (uint8_t)mmio_read_host(rcba.host + at, 1);
        }
    }
    rcba.guest = guest;
    if (mmio_register(guest, RCBA_SIZE, &rcba_ops, NULL) < 0) {
        printf("No room to give the guest the chipset configuration registers\n");
        return;
    }
    pci_virt_hook(rcba.lpc, &lpc_hook, NULL);
    printf(
        "Chipset configuration registers at %#llx are the guest's at %#llx: GCS, BUC, and SPI "
        "reach the chipset, the guest has its own interrupt routing, HPET, and function disable "
        "registers, and its other writes are dropped\n",
        (unsigned long long)rcba.host, (unsigned long long)guest
    );
}
