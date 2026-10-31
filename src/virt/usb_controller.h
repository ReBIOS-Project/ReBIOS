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

#ifndef VIRT__USB_CONTROLLER_H
#define VIRT__USB_CONTROLLER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct pci_function;
struct tendril_hc;

// Who has it: the stack is yet to take it, has it, or the guest does.
#define USB_STATE_WAITING 0
#define USB_STATE_OWNED 1
#define USB_STATE_RELEASED 2

#define USB_LEGACY_RANGES 2

struct usb_controller;

// Registers of the controller the guest reaches through the hypervisor.
struct usb_legacy_range {
    struct usb_controller *c;
    size_t offset;
    size_t size;
};

struct usb_controller {
    const struct pci_function *f;
    int type;
    // Its registers: ports for UHCI, where mmio is NULL, else memory.
    uint16_t port;
    size_t port_count;
    volatile uint8_t *mmio;
    size_t mmio_size;
    struct tendril_hc *hc;
    int state;
    bool release_wanted;
    // The ports it shares with an EHCI controller go back to that one, when
    // the OS takes that one.
    bool ports_to_ehci;

    // What the guest sees of it meanwhile. Where USB legacy support is, in its
    // registers for xHCI and in configuration space for EHCI, and the OS's
    // semaphore there.
    uint32_t legacy_cap;
    bool os_owned;
    // The guest's command and power management control registers, once it has
    // written them.
    bool command_kept;
    uint16_t command;
    bool power_kept;
    uint16_t power;
    struct usb_legacy_range ranges[USB_LEGACY_RANGES];
    size_t range_count;
};

// The guest wants the controller: the stack lets go as soon as it can.
void usb_release(struct usb_controller *c);

// Whether the guest has to be kept from the controller, as the stack has it or
// is about to.
bool usb_legacy_own(struct usb_controller *c);
// The controller is the guest's from now on.
void usb_legacy_disown(struct usb_controller *c);

// The controller a host of Tendril's is, or NULL.
struct usb_controller *usb_controller_of(const struct tendril_hc *hc);

// Pages of the memory devices reach, or NULL, which usb_dma_free() takes back.
void *usb_dma_alloc(size_t size, size_t align, uintptr_t *bus);
void usb_dma_free(void *ptr, size_t size);

// Makes the stack run its next slice soon.
void usb_kick(void);

#endif
