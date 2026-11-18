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

#ifndef VIRT__VACPI_H
#define VIRT__VACPI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <hw/pci_cfg.h>

struct cpu;

#define VACPI_FADT_SIZE 116
#define VACPI_FACS_SIZE 64

// After irq_init(): starts the firmware's ACPI under the hypervisor and
// connects its events to the guest: the SCI, Notify(), and the power button,
// through PM1 and GPE registers the hypervisor emulates where the host has its
// own.
void vacpi_init(void);
// Before the machine sleeps, the GPEs the guest enabled set up to wake it, as
// its OS enables those of its devices' _PRW that may; after it woke, or did
// not sleep, back as they were, the guest's status showing what woke it.
void vacpi_sleep_arm(void);
void vacpi_sleep_woken(bool slept);
// After vacpi_sleep_arm(): whether the guest enabled the GPE that the PCI
// function's _PRW gives, to wake the machine.
bool vacpi_sleep_armed_for(struct pci_cfg_addr addr);
// The guest's virtual PM1 as a wake from a sleeping state leaves it: WAK_STS
// set (ACPI 6.6, 4.8.3.1.1, Table 4.11), SLP_TYP as the guest wrote it, as the
// chipsets that keep it in the RTC well have it (Intel 6 Series 324645-006,
// 13.8.3.3).
void vacpi_wake(void);
// Whether the guest has an ACPI OS that takes its events: the GPE of its
// notifications, or the power button.
bool vacpi_guest_ospm(void);
// Whether the CPU's guest waits for its call of a method of the firmware's,
// which the AML thread runs.
bool vacpi_call_waits(const struct cpu *cpu);

// The guest's DSDT, generated from the firmware's namespace in ACPI 1.0 AML,
// and the device its methods call the firmware's through, which this places in
// guest memory. Needs the guest's PCI interrupt routing, irq_route_pci().
// Returns the table, which the caller frees, and its length, or NULL.
uint8_t *vacpi_dsdt(size_t *length);

// A revision 1 FADT and a FACS for the guest's tables.
void vacpi_fadt(uint8_t fadt[static VACPI_FADT_SIZE], uint32_t facs, uint32_t dsdt);
void vacpi_facs(uint8_t facs[static VACPI_FACS_SIZE]);

// For HYPERCALL_PNP_NODES, whose struct is at gpa, after vacpi_dsdt(): the
// legacy devices and motherboard resources the guest's ACPI describes, as the
// system device nodes of the legacy BIOS's Plug and Play BIOS.
void vacpi_pnp_nodes(uint32_t gpa);

#endif
