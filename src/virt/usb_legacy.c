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
#include <tendril/tendril.h>
#include <hw/pci_cfg.h>
#include <hw/pci_enum.h>
#include <hw/usb_handoff.h>
#include <sys/mm.h>
#include <sys/x86.h>
#include <virt/io.h>
#include <virt/mmio.h>
#include <virt/pci_virt.h>
#include <virt/usb_controller.h>
#include <virt/vcpu.h>
#include <virt/virt.h>

// What the guest sees of a controller the hypervisor has is what it would of
// one a BIOS with legacy USB support has: registers it can read, and
// semaphores that say whose it is. An OS that asks for it gets it as such a
// BIOS gives it, a little later, and one that just starts writing gets it
// first.

#define REG_COMMAND 0x04
#define COMMAND_BITS ((uint32_t)0xffff)
#define COMMAND_MEMORY ((uint32_t)1 << 1)

// PCI Power Management Interface 1.2, 3.2.4.
#define PM_CONTROL 4

#define MSIX_CONTROL 2
#define MSIX_TABLE 4
#define MSIX_SIZE_MASK 0x7ff
#define MSIX_BIR_MASK ((uint32_t)7)
#define MSIX_ENTRY_SIZE 16

// xHCI 1.2, 7.1.1, and EHCI 1.0, 2.1.7.
#define LEGSUP_BIOS ((uint32_t)1 << 16)
#define LEGSUP_OS ((uint32_t)1 << 24)

// UHCI 1.1, 5.2.1: the enable of the SMI a BIOS's support takes.
#define UHCI_LEGSUP 0xc0
#define UHCI_LEGSUP_SMI_ENABLE ((uint32_t)1 << 4)
#define UHCI_LEGSUP_BITS ((uint32_t)0xffff)

// OHCI 1.0a, 5.1.1.3.3, 7.1.2, and 7.1.3.
#define OHCI_CONTROL 0x04
#define OHCI_CONTROL_IR ((uint32_t)1 << 8)
#define OHCI_COMMAND_STATUS 0x08
#define OHCI_COMMAND_OCR ((uint32_t)1 << 3)

static bool owned(const struct usb_controller *c) {
    return __atomic_load_n(&c->state, __ATOMIC_ACQUIRE) != USB_STATE_RELEASED;
}

static uint32_t legsup_shown(const struct usb_controller *c, uint32_t value) {
    return (value & ~(LEGSUP_BIOS | LEGSUP_OS)) | LEGSUP_BIOS
        | (c->os_owned ? LEGSUP_OS : 0);
}

// A read of size bytes at at, with what falls in the dword at reg from dword.
static uint64_t dword_into_access(
    uint64_t at, size_t size, uint64_t access, uint64_t reg, uint32_t dword
) {
    for (size_t i = 0; i < size; i++) {
        if (at + i >= reg && at + i < reg + 4) {
            uint64_t byte = dword >> (8 * (at + i - reg)) & 0xff;
            access = (access & ~((uint64_t)0xff << (8 * i))) | byte << (8 * i);
        }
    }
    return access;
}

// What of a write of size bytes at at falls in the dword at reg, in its place
// there, and the mask of those bytes.
static uint32_t access_into_dword(
    uint64_t at, size_t size, uint64_t access, uint64_t reg, uint32_t *mask
) {
    uint32_t dword = 0;
    *mask = 0;
    for (size_t i = 0; i < size; i++) {
        if (at + i >= reg && at + i < reg + 4) {
            size_t shift = 8 * (at + i - reg);
            dword |= (uint32_t)(access >> (8 * i) & 0xff) << shift;
            *mask |= (uint32_t)0xff << shift;
        }
    }
    return dword;
}

static uint64_t range_read(
    struct vcpu *v, void *ctx, uint64_t offset, size_t size
) {
    (void)v;
    struct usb_legacy_range *r = ctx;
    struct usb_controller *c = r->c;
    uint64_t at = r->offset + offset;
    uint64_t value = mmio_read_host((uintptr_t)c->mmio + at, size);
    if (!owned(c)) {
        return value;
    }
    if (c->type == TENDRIL_HC_XHCI && c->legacy_cap != 0
     && at < c->legacy_cap + 4 && at + size > c->legacy_cap) {
        uint32_t real = (uint32_t)mmio_read_host(
            (uintptr_t)c->mmio + c->legacy_cap, 4
        );
        value = dword_into_access(
            at, size, value, c->legacy_cap, legsup_shown(c, real)
        );
    }
    if (c->type == TENDRIL_HC_OHCI && at < OHCI_CONTROL + 4
     && at + size > OHCI_CONTROL) {
        uint32_t real = (uint32_t)mmio_read_host(
            (uintptr_t)c->mmio + OHCI_CONTROL, 4
        );
        value = dword_into_access(
            at, size, value, OHCI_CONTROL, real | OHCI_CONTROL_IR
        );
    }
    return value;
}

static void range_write(
    struct vcpu *v, void *ctx, uint64_t offset, size_t size, uint64_t value
) {
    struct usb_legacy_range *r = ctx;
    struct usb_controller *c = r->c;
    uint64_t at = r->offset + offset;
    if (!owned(c)) {
        mmio_write_host((uintptr_t)c->mmio + at, size, value);
        return;
    }
    uint32_t mask;
    if (c->type == TENDRIL_HC_XHCI && c->legacy_cap != 0) {
        uint32_t dword = access_into_dword(
            at, size, value, c->legacy_cap, &mask
        );
        if ((mask & LEGSUP_OS) != 0) {
            c->os_owned = (dword & LEGSUP_OS) != 0;
            if (c->os_owned) {
                usb_release(c);
            }
        }
        if (mask != 0) {
            return;
        }
    }
    if (c->type == TENDRIL_HC_OHCI) {
        uint32_t dword = access_into_dword(
            at, size, value, OHCI_COMMAND_STATUS, &mask
        );
        if ((dword & OHCI_COMMAND_OCR) != 0) {
            usb_release(c);
            return;
        }
    }
    usb_release(c);
    v->mmio_retry = true;
}

static const struct mmio_ops range_ops = { range_read, range_write, false };

// The low half of the register at reg, as the guest wrote it.
static void low_half_keep(
    struct pci_addr a, uint32_t reg, uint32_t value, uint32_t mask,
    bool *kept, uint16_t *low
) {
    uint32_t real = pci_cfg_read(a, reg, 4);
    uint32_t shown = *kept ? *low : real & COMMAND_BITS;
    *low = (uint16_t)((shown & ~mask) | (value & mask));
    *kept = true;
    // The other half has bits that clear where ones are written.
    pci_cfg_write(
        a, reg, 4, (real & COMMAND_BITS) | (value & mask & ~COMMAND_BITS)
    );
}

static bool hook_read(void *ctx, uint32_t reg, uint32_t *value) {
    struct usb_controller *c = ctx;
    struct pci_addr a = c->f->addr;
    if (reg == REG_COMMAND && c->command_kept) {
        *value = (pci_cfg_read(a, reg, 4) & ~COMMAND_BITS) | c->command;
        return true;
    }
    if (c->f->pm_cap != 0 && reg == c->f->pm_cap + (uint32_t)PM_CONTROL
     && c->power_kept) {
        *value = (pci_cfg_read(a, reg, 4) & ~COMMAND_BITS) | c->power;
        return true;
    }
    if (c->type == TENDRIL_HC_EHCI && c->legacy_cap != 0
     && reg == c->legacy_cap) {
        *value = legsup_shown(c, pci_cfg_read(a, reg, 4));
        return true;
    }
    if (c->type == TENDRIL_HC_UHCI && reg == UHCI_LEGSUP) {
        *value = pci_cfg_read(a, reg, 4) | UHCI_LEGSUP_SMI_ENABLE;
        return true;
    }
    return false;
}

// Enumeration turns decoding off to size the BARs, and an OS with no driver
// for the controller may power it down, neither of which must reach it under
// the stack: those registers wait until the OS has it.
static bool hook_write(
    void *ctx, uint32_t reg, uint32_t value, uint32_t mask
) {
    struct usb_controller *c = ctx;
    struct pci_addr a = c->f->addr;
    if (reg == REG_COMMAND) {
        low_half_keep(a, reg, value, mask, &c->command_kept, &c->command);
        return true;
    }
    if (c->f->pm_cap != 0 && reg == c->f->pm_cap + (uint32_t)PM_CONTROL) {
        low_half_keep(a, reg, value, mask, &c->power_kept, &c->power);
        return true;
    }
    if (c->type == TENDRIL_HC_EHCI && c->legacy_cap != 0
     && reg == c->legacy_cap) {
        if ((mask & LEGSUP_OS) != 0) {
            c->os_owned = (value & LEGSUP_OS) != 0;
            if (c->os_owned) {
                usb_release(c);
            }
        }
        return true;
    }
    if (c->type == TENDRIL_HC_UHCI && reg == UHCI_LEGSUP
     && (mask & UHCI_LEGSUP_BITS) != 0) {
        usb_release(c);
    }
    return false;
}

static const struct pci_virt_hook hook = { hook_read, hook_write };

static void port_access(
    struct vcpu *v, void *ctx, uint16_t offset, size_t size, bool in,
    uint32_t *value
) {
    struct usb_controller *c = ctx;
    uint16_t port = (uint16_t)(c->port + offset);
    if (!in && owned(c)) {
        usb_release(c);
        v->io_retry = true;
        return;
    }
    switch (size) {
        case 1: {
            if (in) {
                *value = (*value & ~(uint32_t)0xff) | inb(port);
            } else {
                outb(port, (uint8_t)*value);
            }
            break;
        }
        case 2: {
            if (in) {
                *value = (*value & ~(uint32_t)0xffff) | inw(port);
            } else {
                outw(port, (uint16_t)*value);
            }
            break;
        }
        default: {
            if (in) {
                *value = inl(port);
            } else {
                outl(port, *value);
            }
            break;
        }
    }
}

static bool trap_range(struct usb_controller *c, size_t offset, size_t size) {
    if (size == 0) {
        return true;
    }
    struct usb_legacy_range *r = &c->ranges[c->range_count];
    *r = (struct usb_legacy_range){ c, offset, size };
    if (!pci_virt_trap(c->f->addr, 0, offset, size, &range_ops, r)) {
        return false;
    }
    c->range_count++;
    return true;
}

// All of the BAR but the pages of an MSI-X table, which are the guest's
// anyway.
static bool trap_registers(struct usb_controller *c) {
    const struct pci_function *f = c->f;
    size_t first = c->mmio_size;
    size_t end = c->mmio_size;
    if (f->msix_cap != 0) {
        uint32_t table = pci_cfg_read(f->addr, f->msix_cap + MSIX_TABLE, 4);
        size_t count = (pci_cfg_read(f->addr, f->msix_cap + MSIX_CONTROL, 2)
            & MSIX_SIZE_MASK) + 1;
        if ((table & MSIX_BIR_MASK) == 0) {
            size_t offset = table & ~MSIX_BIR_MASK;
            first = offset & ~(size_t)(PAGE_SIZE - 1);
            end = (offset + count * MSIX_ENTRY_SIZE + PAGE_SIZE - 1)
                & ~(size_t)(PAGE_SIZE - 1);
        }
    }
    return trap_range(c, 0, first)
        && trap_range(c, end, c->mmio_size > end ? c->mmio_size - end : 0);
}

bool usb_legacy_own(struct usb_controller *c) {
    const struct pci_function *f = c->f;
    if (c->type == TENDRIL_HC_UHCI) {
        struct io_handler ports = {
            c->port, c->port_count, port_access, c
        };
        io_register(&ports);
    } else {
        // Firmware that used no USB may have left it off.
        uint32_t command = pci_cfg_read(f->addr, REG_COMMAND, 2);
        pci_cfg_write(f->addr, REG_COMMAND, 2, command | COMMAND_MEMORY);
        if (c->type == TENDRIL_HC_XHCI) {
            c->legacy_cap = usb_xhci_legacy_cap(
                (uintptr_t)c->mmio, c->mmio_size
            );
        } else if (c->type == TENDRIL_HC_EHCI) {
            c->legacy_cap = usb_ehci_legacy_cap(f->addr, (uintptr_t)c->mmio);
        }
        if (!trap_registers(c)) {
            for (size_t i = 0; i < c->range_count; i++) {
                pci_virt_untrap(f->addr, 0, c->ranges[i].offset);
            }
            return false;
        }
    }
    pci_virt_hook(f->addr, &hook, c);
    return true;
}

void usb_legacy_disown(struct usb_controller *c) {
    const struct pci_function *f = c->f;
    pci_virt_hook(f->addr, NULL, NULL);
    if (c->power_kept) {
        pci_cfg_write(f->addr, f->pm_cap + PM_CONTROL, 2, c->power);
    }
    if (c->command_kept) {
        pci_cfg_write(f->addr, REG_COMMAND, 2, c->command);
    }
    for (size_t i = 0; i < c->range_count; i++) {
        pci_virt_untrap(f->addr, 0, c->ranges[i].offset);
    }
    if (c->type == TENDRIL_HC_UHCI) {
        vcpu_pause_others();
        virt_intercept_io(c->port, c->port_count, false);
        vcpu_resume_others();
    }
}
