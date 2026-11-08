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

#ifndef HW__PCI_REGS_H
#define HW__PCI_REGS_H

#include <stdint.h>

// The configuration space of a conventional function, and that of a PCI
// Express function, which ECAM maps whole.
#define PCI_CFG_SPACE_SIZE 256
#define PCI_CFG_SPACE_EXP_SIZE 4096

// A function's configuration header (PCI Local Bus Specification 3.0, 6.1 and
// 6.2) and a bridge's (PCI-to-PCI Bridge Architecture Specification 1.1, 3.2),
// by the names that Linux 7.3's include/uapi/linux/pci_regs.h gives them.
#define PCI_VENDOR_ID 0x00
#define PCI_DEVICE_ID 0x02
#define PCI_COMMAND 0x04
#define PCI_COMMAND_IO ((uint32_t)1 << 0)
#define PCI_COMMAND_MEMORY ((uint32_t)1 << 1)
#define PCI_COMMAND_MASTER ((uint32_t)1 << 2)
#define PCI_STATUS 0x06
#define PCI_STATUS_CAP_LIST ((uint32_t)1 << 4)
#define PCI_CLASS_REVISION 0x08
#define PCI_CLASS_DEVICE 0x0a
#define PCI_CACHE_LINE_SIZE 0x0c
#define PCI_HEADER_TYPE 0x0e
#define PCI_HEADER_TYPE_MASK 0x7f
#define PCI_HEADER_TYPE_NORMAL 0
#define PCI_HEADER_TYPE_BRIDGE 1
#define PCI_HEADER_TYPE_CARDBUS 2
#define PCI_HEADER_TYPE_MFD 0x80
#define PCI_BASE_ADDRESS_0 0x10
#define PCI_BASE_ADDRESS_SPACE_IO ((uint32_t)0x01)
#define PCI_BASE_ADDRESS_IO_MASK ((uint32_t)0xfffffffc)
#define PCI_STD_NUM_BARS 6
#define PCI_ROM_ADDRESS 0x30
#define PCI_ROM_ADDRESS_ENABLE ((uint32_t)0x1)
#define PCI_ROM_ADDRESS_MASK ((uint32_t)0xfffff800)
#define PCI_CAPABILITY_LIST 0x34
#define PCI_INTERRUPT_LINE 0x3c
// Where capabilities start, and the most that a list holds.
#define PCI_STD_HEADER_SIZEOF 0x40
#define PCI_FIND_CAP_TTL 48

#define PCI_PRIMARY_BUS 0x18
#define PCI_SECONDARY_BUS 0x19
#define PCI_SUBORDINATE_BUS 0x1a
#define PCI_IO_BASE 0x1c
#define PCI_IO_LIMIT 0x1d
#define PCI_MEMORY_BASE 0x20
#define PCI_MEMORY_LIMIT 0x22
#define PCI_PREF_MEMORY_BASE 0x24
#define PCI_PREF_MEMORY_LIMIT 0x26
#define PCI_PREF_RANGE_TYPE_64 ((uint32_t)0x1)
#define PCI_PREF_BASE_UPPER32 0x28
#define PCI_PREF_LIMIT_UPPER32 0x2c
#define PCI_IO_BASE_UPPER16 0x30
#define PCI_ROM_ADDRESS1 0x38
#define PCI_BRIDGE_CONTROL 0x3e
#define PCI_BRIDGE_CTL_VGA 0x0008
#define PCI_BRIDGE_CTL_VGA_16BIT 0x0010

// A bridge's windows come in 4 KiB granules of I/O space and 1 MiB granules of
// memory (PCI-to-PCI Bridge Architecture Specification 1.1, 3.2.5.6, 3.2.5.8,
// and 3.2.5.9).
#define PCI_IO_GRANULE ((uint32_t)0x1000)
#define PCI_MEMORY_GRANULE ((uint64_t)0x100000)

// Capabilities (PCI Local Bus Specification 3.0, 6.7, 6.8.1, and 6.8.2): MSI's
// Message Control register and the registers after it, and MSI-X's, where its
// table is, and a table entry's fields.
#define PCI_CAP_ID_PM 0x01
#define PCI_CAP_ID_MSI 0x05
#define PCI_CAP_ID_EXP 0x10
#define PCI_CAP_ID_MSIX 0x11
#define PCI_MSI_FLAGS 2
#define PCI_MSI_FLAGS_ENABLE ((uint32_t)0x0001)
#define PCI_MSI_FLAGS_QSIZE ((uint32_t)0x0070)
#define PCI_MSI_FLAGS_64BIT ((uint32_t)0x0080)
#define PCI_MSI_ADDRESS_LO 4
#define PCI_MSI_ADDRESS_HI 8
#define PCI_MSI_DATA_32 8
#define PCI_MSI_DATA_64 12
#define PCI_MSIX_FLAGS 2
#define PCI_MSIX_FLAGS_QSIZE ((uint32_t)0x07ff)
#define PCI_MSIX_FLAGS_ENABLE ((uint32_t)0x8000)
#define PCI_MSIX_TABLE 4
#define PCI_MSIX_TABLE_BIR ((uint32_t)7)
#define PCI_MSIX_ENTRY_SIZE 16
#define PCI_MSIX_ENTRY_LOWER_ADDR 0
#define PCI_MSIX_ENTRY_UPPER_ADDR 4
#define PCI_MSIX_ENTRY_DATA 8
#define PCI_MSIX_ENTRY_VECTOR_CTRL 12
#define PCI_MSIX_ENTRY_CTRL_MASKBIT ((uint32_t)1)
#define PCI_EXP_FLAGS 2

// Class codes, by the names that Linux 7.3's include/linux/pci_ids.h gives
// them, and the subclasses of base classes, after them.
#define PCI_BASE_CLASS_STORAGE 0x01
#define PCI_SUBCLASS_STORAGE_IDE 0x01
#define PCI_BASE_CLASS_DISPLAY 0x03
#define PCI_BASE_CLASS_BRIDGE 0x06
#define PCI_SUBCLASS_BRIDGE_HOST 0x00
#define PCI_SUBCLASS_BRIDGE_ISA 0x01
#define PCI_SUBCLASS_BRIDGE_PCI 0x04
#define PCI_CLASS_BRIDGE_HOST 0x0600
#define PCI_CLASS_BRIDGE_ISA 0x0601
#define PCI_BASE_CLASS_SYSTEM 0x08
#define PCI_SUBCLASS_SYSTEM_IOMMU 0x06
#define PCI_BASE_CLASS_SERIAL 0x0c
#define PCI_SUBCLASS_SERIAL_USB 0x03
#define PCI_CLASS_SERIAL_USB 0x0c03
#define PCI_VENDOR_ID_REALTEK 0x10ec
#define PCI_VENDOR_ID_ATI 0x1002
#define PCI_VENDOR_ID_NVIDIA 0x10de
#define PCI_VENDOR_ID_INTEL 0x8086

// The buses of a segment, which bus numbers' eight bits give.
#define PCI_BUSES 256

#endif
