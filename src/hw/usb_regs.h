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

#ifndef HW__USB_REGS_H
#define HW__USB_REGS_H

#include <stdint.h>

// EHCI 1.0, 2.1.7, and xHCI 1.2, 7.1.1: the semaphores of USBLEGSUP, which
// both have.
#define USBLEGSUP_BIOS_OWNED ((uint32_t)1 << 16)
#define USBLEGSUP_OS_OWNED ((uint32_t)1 << 24)

// UHCI 1.1, 5.2.1: the SMI enable that a BIOS's support takes, the enables of
// the firmware's keyboard emulation, the R/WC status bits, which writing ones
// clears, and the register's bits.
#define UHCI_LEGSUP 0xc0
#define UHCI_LEGSUP_SMI_ENABLE ((uint32_t)1 << 4)
#define UHCI_LEGSUP_ENABLES ((uint32_t)0xbf)
#define UHCI_LEGSUP_RWC ((uint32_t)0x8f00)
#define UHCI_LEGSUP_BITS ((uint32_t)0xffff)
// UHCI 1.1, 2.1.1 and 2.1.3.
#define UHCI_USBCMD 0x00
#define UHCI_USBCMD_HCRESET ((uint32_t)1 << 1)
#define UHCI_USBINTR 0x04

// OHCI 1.0a, 5.1.1.3.3, 7.1.2, 7.1.3, 7.1.5, 7.1.6, and 7.3.1.
#define OHCI_CONTROL 0x04
#define OHCI_CONTROL_IR ((uint32_t)1 << 8)
#define OHCI_CONTROL_RWC ((uint32_t)1 << 9)
#define OHCI_COMMAND_STATUS 0x08
#define OHCI_COMMAND_HCR ((uint32_t)1 << 0)
#define OHCI_COMMAND_OCR ((uint32_t)1 << 3)
#define OHCI_INTERRUPT_ENABLE 0x10
#define OHCI_INTERRUPT_DISABLE 0x14
#define OHCI_INTERRUPT_OC ((uint32_t)1 << 30)
#define OHCI_FM_INTERVAL 0x34

// EHCI 1.0: HCCPARAMS' pointer to the extended capabilities in configuration
// space, 40h or above (2.2.4); USBLEGSUP's semaphores, a byte each (2.1.7);
// USBLEGCTLSTS's SMI enables (2.1.8); the operational registers (2.3).
#define EHCI_CAPLENGTH 0x00
#define EHCI_HCCPARAMS 0x08
#define EHCI_EECP_MIN 0x40
#define EHCI_CAP_LEGACY 1
#define EHCI_LEGSUP_BIOS_BYTE 2
#define EHCI_LEGSUP_OS_BYTE 3
#define EHCI_LEGCTLSTS 4
#define EHCI_USBCMD 0x00
#define EHCI_USBCMD_RUN ((uint32_t)1 << 0)
#define EHCI_USBSTS 0x04
#define EHCI_USBSTS_HALTED ((uint32_t)1 << 12)
// The R/WC interrupt status bits.
#define EHCI_USBSTS_INTERRUPTS ((uint32_t)0x3f)
#define EHCI_USBINTR 0x08
#define EHCI_CONFIGFLAG 0x40

// xHCI 1.2: HCCPARAMS1's pointer to the extended capabilities, in dwords
// (5.3.6); USB Legacy Support's semaphores (7.1.1) and SMI controls (7.1.2),
// of which the RsvdP bits are written back, the enables cleared, and the RW1C
// events written to clear them; USBCMD's interrupt enables INTE, HSEE, and EWE
// (5.4.1); USBSTS (5.4.2).
#define XHCI_CAPLENGTH 0x00
#define XHCI_HCCPARAMS1 0x10
#define XHCI_CAP_LEGACY 1
#define XHCI_LEGCTLSTS 4
#define XHCI_LEGCTLSTS_RSVDP \
    (((uint32_t)7 << 1) | ((uint32_t)0xff << 5) | ((uint32_t)7 << 17))
#define XHCI_LEGCTLSTS_EVENTS ((uint32_t)7 << 29)
#define XHCI_USBCMD 0x00
#define XHCI_USBCMD_RUN ((uint32_t)1 << 0)
#define XHCI_USBCMD_INTERRUPTS \
    (((uint32_t)1 << 2) | ((uint32_t)1 << 3) | ((uint32_t)1 << 10))
#define XHCI_USBSTS 0x04
#define XHCI_USBSTS_HCH ((uint32_t)1 << 0)
#define XHCI_USBSTS_CNR ((uint32_t)1 << 11)

#endif
