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
#include <hw/usb_handoff.h>
#include <hw/pci_cfg.h>
#include <hw/pci_enum.h>
#include <hw/pci_regs.h>
#include <hw/usb_regs.h>
#include <iommu/iommu_tables.h>
#include <lib/log.h>
#include <lib/time.h>
#include <sys/mm.h>
#include <sys/x86.h>

#define USB_UHCI 0x00
#define USB_OHCI 0x10
#define USB_EHCI 0x20
#define USB_XHCI 0x30

// The firmware gets a second to let go and xHCI controllers 5 seconds to be
// ready, as Linux 7.3 gives them (drivers/usb/host/pci-quirks.c). Controllers
// halt within 16 ms (xHCI 1.2, 5.4.1; EHCI 1.0, 2.3.1 has 16 micro-frames), an
// OHCI reset takes up to 10 us (OHCI 1.0a, 5.1.1.4), allowed thrice as Linux
// 7.3 does, and a UHCI reset has no stated bound.
#define RELEASE_US 1000000
#define XHCI_READY_US 5000000
#define HALT_US 16000
#define OHCI_RESET_US 30
#define UHCI_RESET_US 10000
#define POLL_US 10
// Bounds on capability chains a broken controller might loop.
#define MAX_CAPABILITIES 64

#define MAX_TAKEN 64

static struct pci_cfg_addr taken[MAX_TAKEN];
static size_t taken_count;

static volatile uint32_t *reg(uintptr_t base, uint64_t offset) {
    return (volatile uint32_t *)(uintptr_t)(base + offset);
}

// Whether the bits in mask read as want within us microseconds.
static bool reg_wait(uintptr_t base, uint64_t offset, uint32_t mask, uint32_t want, uint64_t us) {
    for (size_t waited = 0; ; waited += POLL_US) {
        if ((*reg(base, offset) & mask) == want) {
            return true;
        }
        if (waited >= us) {
            return false;
        }
        time_udelay(POLL_US);
    }
}

static bool cfg_wait(
    struct pci_cfg_addr a, uint32_t offset, uint32_t mask, uint32_t want, uint64_t us
) {
    for (size_t waited = 0; ; waited += POLL_US) {
        if ((pci_cfg_read(a, offset, 4) & mask) == want) {
            return true;
        }
        if (waited >= us) {
            return false;
        }
        time_udelay(POLL_US);
    }
}

// What became of the firmware's ownership.
struct release {
    bool owned;
    bool forced;
};

uint32_t usb_xhci_legacy_cap(uintptr_t base, size_t size) {
    uint64_t at = (uint64_t)(*reg(base, XHCI_HCCPARAMS1) >> 16) * 4;
    for (size_t n = 0; at != 0 && at + 8 <= size && n < MAX_CAPABILITIES; n++) {
        uint32_t cap = *reg(base, at);
        if ((cap & 0xff) == XHCI_CAP_LEGACY) {
            return (uint32_t)at;
        }
        uint32_t next = cap >> 8 & 0xff;
        at = next != 0 ? at + next * 4 : 0;
    }
    return 0;
}

uint32_t usb_ehci_legacy_cap(struct pci_cfg_addr a, uintptr_t base) {
    uint32_t at = *reg(base, EHCI_HCCPARAMS) >> 8 & 0xff;
    for (size_t n = 0; at >= EHCI_EECP_MIN && n < MAX_CAPABILITIES; n++) {
        uint32_t cap = pci_cfg_read(a, at, 4);
        if ((cap & 0xff) == EHCI_CAP_LEGACY) {
            return at;
        }
        if ((cap & 0xff) == 0) {
            break;
        }
        at = cap >> 8 & 0xff;
    }
    return 0;
}

// xHCI 1.2, 4.22.1.
static bool xhci_take(uintptr_t base, size_t size, struct release *r) {
    uint32_t at = usb_xhci_legacy_cap(base, size);
    if (at != 0) {
        uint32_t cap = *reg(base, at);
        if ((cap & USBLEGSUP_BIOS_OWNED) != 0) {
            r->owned = true;
            *reg(base, at) = cap | USBLEGSUP_OS_OWNED;
            if (!reg_wait(base, at, USBLEGSUP_BIOS_OWNED, 0, RELEASE_US)) {
                r->forced = true;
                *reg(base, at) = (cap | USBLEGSUP_OS_OWNED) & ~USBLEGSUP_BIOS_OWNED;
            }
        }
        uint32_t control = *reg(base, at + XHCI_LEGCTLSTS);
        *reg(base, at + XHCI_LEGCTLSTS) = (control & XHCI_LEGCTLSTS_RSVDP) | XHCI_LEGCTLSTS_EVENTS;
    }

    uintptr_t op = base + (*reg(base, XHCI_CAPLENGTH) & 0xff);
    reg_wait(op, XHCI_USBSTS, XHCI_USBSTS_CNR, 0, XHCI_READY_US);
    *reg(op, XHCI_USBCMD) &= ~(XHCI_USBCMD_RUN | XHCI_USBCMD_INTERRUPTS);
    return reg_wait(op, XHCI_USBSTS, XHCI_USBSTS_HCH, XHCI_USBSTS_HCH, HALT_US);
}

// EHCI 1.0, 5.1.
static bool ehci_take(struct pci_cfg_addr a, uintptr_t base, struct release *r) {
    uint32_t at = *reg(base, EHCI_HCCPARAMS) >> 8 & 0xff;
    for (size_t n = 0; at >= EHCI_EECP_MIN && n < MAX_CAPABILITIES; n++) {
        uint32_t cap = pci_cfg_read(a, at, 4);
        if ((cap & 0xff) == 0) {
            break;
        }
        if ((cap & 0xff) == EHCI_CAP_LEGACY) {
            if ((cap & USBLEGSUP_BIOS_OWNED) != 0) {
                // The semaphore's byte alone: Linux 7.3 notes firmware that
                // misbehaves when it is set any other way.
                r->owned = true;
                pci_cfg_write(a, at + EHCI_LEGSUP_OS_BYTE, 1, 1);
                if (!cfg_wait(a, at, USBLEGSUP_BIOS_OWNED, 0, RELEASE_US)) {
                    r->forced = true;
                    pci_cfg_write(a, at + EHCI_LEGSUP_BIOS_BYTE, 1, 0);
                }
            }
            pci_cfg_write(a, at + EHCI_LEGCTLSTS, 4, 0);
        }
        at = cap >> 8 & 0xff;
    }

    uintptr_t op = base + (*reg(base, EHCI_CAPLENGTH) & 0xff);
    if ((*reg(op, EHCI_USBSTS) & EHCI_USBSTS_HALTED) == 0) {
        *reg(op, EHCI_USBCMD) &= ~EHCI_USBCMD_RUN;
    }
    bool halted = reg_wait(op, EHCI_USBSTS, EHCI_USBSTS_HALTED, EHCI_USBSTS_HALTED, HALT_US);
    *reg(op, EHCI_USBINTR) = 0;
    *reg(op, EHCI_USBSTS) = EHCI_USBSTS_INTERRUPTS;
    // The firmware's port setup does not outlive its ownership: the ports go
    // back to the companion controllers.
    if (r->owned) {
        *reg(op, EHCI_CONFIGFLAG) = 0;
    }
    return halted;
}

// OHCI 1.0a, 5.1.1.3.3: an ownership change request makes the firmware's SMM
// driver let go, which it shows by clearing InterruptRouting.
static bool ohci_take(uintptr_t base, struct release *r) {
    uint32_t control = *reg(base, OHCI_CONTROL);
    if ((control & OHCI_CONTROL_IR) != 0) {
        r->owned = true;
        *reg(base, OHCI_INTERRUPT_ENABLE) = OHCI_INTERRUPT_OC;
        *reg(base, OHCI_COMMAND_STATUS) = OHCI_COMMAND_OCR;
        r->forced = !reg_wait(base, OHCI_CONTROL, OHCI_CONTROL_IR, 0, RELEASE_US);
    }
    *reg(base, OHCI_INTERRUPT_DISABLE) = UINT32_MAX;
    // UsbReset, keeping RemoteWakeupConnected, then a software reset, which
    // loses the frame interval.
    *reg(base, OHCI_CONTROL) = control & OHCI_CONTROL_RWC;
    (void)*reg(base, OHCI_CONTROL);
    uint32_t interval = *reg(base, OHCI_FM_INTERVAL);
    *reg(base, OHCI_COMMAND_STATUS) = OHCI_COMMAND_HCR;
    bool reset = reg_wait(base, OHCI_COMMAND_STATUS, OHCI_COMMAND_HCR, 0, OHCI_RESET_US);
    *reg(base, OHCI_FM_INTERVAL) = interval;
    return reset;
}

static bool uhci_take(struct pci_cfg_addr a, uint16_t port, struct release *r) {
    r->owned = (pci_cfg_read(a, UHCI_LEGSUP, 2) & UHCI_LEGSUP_ENABLES) != 0;
    pci_cfg_write(a, UHCI_LEGSUP, 2, UHCI_LEGSUP_RWC);
    outw(port + UHCI_USBCMD, UHCI_USBCMD_HCRESET);
    bool reset = false;
    for (size_t waited = 0; !reset && waited <= UHCI_RESET_US; waited += POLL_US) {
        time_udelay(POLL_US);
        reset = (inw(port + UHCI_USBCMD) & UHCI_USBCMD_HCRESET) == 0;
    }
    outw(port + UHCI_USBINTR, 0);
    outw(port + UHCI_USBCMD, 0);
    return reset;
}

static const struct pci_bar *first_bar(const struct pci_function *f, bool io) {
    for (size_t i = 0; i < f->bar_count; i++) {
        const struct pci_bar *b = &f->bars[i];
        bool is_io = b->kind == PCI_BAR_IO;
        bool is_memory = b->kind == PCI_BAR_MEM32 || b->kind == PCI_BAR_MEM64;
        if ((io ? is_io : is_memory) && b->base != 0 && b->size != 0) {
            return b;
        }
    }
    return NULL;
}

static void take(const struct pci_function *f) {
    const char *name;
    switch (f->prog_if) {
        case USB_UHCI: {
            name = "UHCI";
            break;
        }
        case USB_OHCI: {
            name = "OHCI";
            break;
        }
        case USB_EHCI: {
            name = "EHCI";
            break;
        }
        case USB_XHCI: {
            name = "xHCI";
            break;
        }
        default: {
            return;
        }
    }
    struct pci_cfg_addr a = f->addr;
    uint32_t command = pci_cfg_read(a, PCI_COMMAND, 2);
    bool io = f->prog_if == USB_UHCI;
    const struct pci_bar *bar = first_bar(f, io);
    if (bar == NULL || (command & (io ? PCI_COMMAND_IO : PCI_COMMAND_MEMORY)) == 0) {
        printf(
            "USB %04x:%02x:%02x.%x: %s left alone, as its registers are not decoded\n", a.segment,
            a.bus, a.device, a.function, name
        );
        return;
    }

    struct release r = { false, false };
    bool stopped;
    if (io) {
        stopped = uhci_take(a, (uint16_t)bar->base, &r);
    } else {
        mm_map_device(bar->base, bar->size);
        if (f->prog_if == USB_OHCI) {
            stopped = ohci_take(bar->base, &r);
        } else if (f->prog_if == USB_EHCI) {
            stopped = ehci_take(a, bar->base, &r);
        } else {
            stopped = xhci_take(bar->base, bar->size, &r);
        }
    }

    const char *from = !r.owned
                     ? ""
                     : r.forced
                         ? " taken from the firmware, which did not let go, and"
                         : " taken from the firmware and";
    printf(
        "USB %04x:%02x:%02x.%x: %s%s %s\n", a.segment, a.bus, a.device, a.function, name, from,
        stopped ? "stopped" : "not stopped"
    );
    if (stopped && taken_count < MAX_TAKEN) {
        taken[taken_count++] = a;
    }
}

static bool unneeded(uint16_t segment, uint16_t rid) {
    for (size_t i = 0; i < taken_count; i++) {
        const struct pci_cfg_addr *a = &taken[i];
        if (a->segment == segment && a->bus == rid >> 8 && a->device == (rid >> 3 & 0x1f)
         && a->function == (rid & 7)) {
            return true;
        }
    }
    return false;
}

void usb_handoff(void) {
    const struct pci_topology *t = pci_enum_topology();
    for (size_t i = 0; t != NULL && i < t->function_count; i++) {
        const struct pci_function *f = &t->functions[i];
        if (f->base_class == PCI_BASE_CLASS_SERIAL && f->subclass == PCI_SUBCLASS_SERIAL_USB) {
            take(f);
        }
    }
    iommu_tables_forget_reserved(unneeded);
}
