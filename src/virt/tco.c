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
#include <stdint.h>
#include <uacpi/acpi.h>
#include <uacpi/status.h>
#include <uacpi/tables.h>
#include <uacpi/types.h>
#include <virt/tco.h>
#include <hw/acpi.h>
#include <hw/pci_cfg.h>
#include <hw/pci_regs.h>
#include <lib/log.h>
#include <virt/io.h>

// The TCO logic's 32 ports at TCOBASE, PMBASE + 60h, where the LPC bridge sets
// PMBASE and enables its decode in ACPI_CNTL (Intel 6 Series Chipset datasheet
// 324645-006, 13.1.12, 13.1.13, and 13.9), or at TCOBASE where the SMBus
// function sets it and enables it in TCOCTL (Intel 600 and 700 Series PCH
// datasheet volume 2, 680836-005, 6.1.17 and 6.1.18), as Linux 7.3's lpc_ich
// and i2c-i801 find them.
#define TCO_PORTS 32
#define LPC_DEVICE 31
#define LPC_FUNCTION 0
#define REG_PMBASE 0x40
#define PMBASE_ADDRESS ((uint32_t)0xff80)
#define REG_ACPI_CNTL 0x44
#define ACPI_CNTL_ACPI_EN ((uint32_t)1 << 7)
#define PMBASE_TCO 0x60
#define SMBUS_FUNCTION 4
#define PCI_CLASS_SERIAL_SMBUS 0x0c05
#define REG_TCOBASE 0x50
#define TCOBASE_ADDRESS ((uint32_t)0xffe0)
#define REG_TCOCTL 0x54
#define TCOCTL_TCO_BASE_EN ((uint32_t)1 << 8)

// Whether the chipset's function at device 31 is Intel's, of class.
static bool intel_function(struct pci_cfg_addr a, uint16_t class) {
    return pci_cfg_read(a, PCI_VENDOR_ID, 2) == PCI_VENDOR_ID_INTEL
        && pci_cfg_read(a, PCI_CLASS_DEVICE, 2) == class;
}

// TCOBASE where the LPC bridge's ACPI base is the FADT's PM1a event block, or
// 0.
static uint16_t lpc_tco(uint16_t pm1a) {
    struct pci_cfg_addr lpc = { 0, 0, LPC_DEVICE, LPC_FUNCTION };
    if (!intel_function(lpc, PCI_CLASS_BRIDGE_ISA)
     || (pci_cfg_read(lpc, REG_ACPI_CNTL, 1) & ACPI_CNTL_ACPI_EN) == 0) {
        return 0;
    }
    uint32_t pmbase = pci_cfg_read(lpc, REG_PMBASE, 4) & PMBASE_ADDRESS;
    return pmbase != 0 && pmbase == pm1a ? (uint16_t)(pmbase + PMBASE_TCO) : 0;
}

// TCOBASE where the SMBus function decodes it, or 0.
static uint16_t smbus_tco(void) {
    struct pci_cfg_addr smbus = { 0, 0, LPC_DEVICE, SMBUS_FUNCTION };
    if (!intel_function(smbus, PCI_CLASS_SERIAL_SMBUS)
     || (pci_cfg_read(smbus, REG_TCOCTL, 4) & TCOCTL_TCO_BASE_EN) == 0) {
        return 0;
    }
    return (uint16_t)(pci_cfg_read(smbus, REG_TCOBASE, 4) & TCOBASE_ADDRESS);
}

void tco_init(void) {
    struct acpi_fadt *fadt;
    if (!acpi_tables_ready() || uacpi_table_fadt(&fadt) != UACPI_STATUS_OK) {
        return;
    }
    const struct acpi_gas *pm1a = &fadt->x_pm1a_evt_blk;
    if (pm1a->address_space_id != UACPI_ADDRESS_SPACE_SYSTEM_IO || pm1a->address == 0
     || pm1a->address > UINT16_MAX) {
        return;
    }
    uint16_t tco = lpc_tco((uint16_t)pm1a->address);
    if (tco == 0) {
        tco = smbus_tco();
    }
    if (tco == 0) {
        return;
    }
    io_pass(tco, TCO_PORTS);
    printf(
        "The chipset's TCO logic at ports %#x-%#x is the guest's, its watchdog timer among it\n",
        tco, tco + TCO_PORTS - 1
    );
}
