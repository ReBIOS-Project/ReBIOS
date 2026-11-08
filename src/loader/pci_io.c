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
#include <efi.h>
#include <loader/pci_io.h>
#include <lib/log.h>
#include <loader/efi_glue.h>

#define PCI_COMMAND 0x04
#define PCI_COMMAND_IO 0x0001
#define PCI_COMMAND_MEMORY 0x0002
#define PCI_COMMAND_MASTER 0x0004
#define PCI_STATUS 0x06
#define PCI_HEADER_TYPE 0x0e
#define PCI_HEADER_TYPE_MASK 0x7f
#define PCI_BAR0 0x10
#define PCI_BARS 6
#define PCI_BAR_IO 0x01
#define PCI_BAR_IO_MASK 0xfffffffc
// A 16550 UART's registers.
#define UART_REGISTERS 8
#define PCI_STATUS_CAPABILITIES 0x10
#define PCI_CLASS_BRIDGE 0x06
#define PCI_SUBCLASS_PCI_BRIDGE 0x04
#define PCI_SECONDARY_BUS 0x19
#define PCI_CAPABILITIES 0x34
#define PCI_FIRST_CAPABILITY 0x40
#define PCI_CAPABILITIES_MAX 48
#define PCI_CAPABILITY_EXPRESS 0x10
#define PCI_BUSES 256

// Linux 7.3's include/uapi/linux/pci_regs.h, PCI_EXP_FLAGS.
#define PCI_EXPRESS_FLAGS 0x02
#define PCI_EXPRESS_FLAGS_TYPE 0x00f0
#define PCI_EXPRESS_TYPE_ROOT_PORT 0x0040
#define PCI_EXPRESS_TYPE_DOWNSTREAM 0x0060
#define PCI_EXPRESS_FLAGS_SLOT 0x0100

static EFI_GUID pci_io_guid = EFI_PCI_IO_PROTOCOL_GUID;

static bool pci_io_device_get(EFI_HANDLE handle, struct pci_io_device *dev) {
    EFI_PCI_IO_PROTOCOL *io;
    EFI_UINTN segment, bus, device, function;
    uint32_t header[4];

    if (efi_bs->HandleProtocol(handle, &pci_io_guid, (void **)&io) != EFI_SUCCESS || io == NULL
     || io->GetLocation(io, &segment, &bus, &device, &function) != EFI_SUCCESS
     || io->Pci.Read(io, EfiPciIoWidthUint32, 0, 4, header) != EFI_SUCCESS) {
        return false;
    }

    dev->handle = handle;
    dev->io = io;
    dev->address.segment = (uint16_t)segment;
    dev->address.bus = (uint8_t)bus;
    dev->address.devfn = (uint8_t)(device << 3 | function);
    dev->vendor_id = (uint16_t)header[0];
    dev->device_id = (uint16_t)(header[0] >> 16);
    dev->revision = (uint8_t)header[2];
    dev->prog_if = (uint8_t)(header[2] >> 8);
    dev->subclass = (uint8_t)(header[2] >> 16);
    dev->base_class = (uint8_t)(header[2] >> 24);
    return true;
}

struct pci_io_device *pci_io_devices(EFI_UINTN *count) {
    size_t handle_count;
    EFI_HANDLE *handles = efi_locate_handles(&pci_io_guid, &handle_count);
    struct pci_io_device *devices = NULL;

    *count = 0;
    if (handles == NULL) {
        return NULL;
    }

    EFI_STATUS status = efi_bs->AllocatePool(
        EfiLoaderData, handle_count * sizeof(struct pci_io_device), (void **)&devices
    );
    if (status != EFI_SUCCESS) {
        printf("Cannot allocate memory to list the PCI devices\n");
        devices = NULL;
    } else {
        for (size_t i = 0; i < handle_count; i++) {
            if (pci_io_device_get(handles[i], &devices[*count])) {
                (*count)++;
            }
        }
        if (*count == 0) {
            efi_bs->FreePool(devices);
            devices = NULL;
        }
    }

    efi_bs->FreePool(handles);
    return devices;
}

bool pci_io_device_on_path(EFI_DEVICE_PATH_PROTOCOL *path, struct pci_io_device *dev) {
    EFI_DEVICE_PATH_PROTOCOL *rest = path;
    EFI_HANDLE handle;

    if (efi_bs->LocateDevicePath(&pci_io_guid, &rest, &handle) != EFI_SUCCESS) {
        return false;
    }
    return pci_io_device_get(handle, dev);
}

static bool port_to_slot(EFI_PCI_IO_PROTOCOL *io) {
    uint16_t status = 0;
    uint8_t next = 0;
    io->Pci.Read(io, EfiPciIoWidthUint16, PCI_STATUS, 1, &status);
    if ((status & PCI_STATUS_CAPABILITIES) != 0) {
        io->Pci.Read(io, EfiPciIoWidthUint8, PCI_CAPABILITIES, 1, &next);
    }

    for (size_t i = 0; i < PCI_CAPABILITIES_MAX; i++) {
        if (next < PCI_FIRST_CAPABILITY) {
            return false;
        }
        uint8_t id = 0;
        io->Pci.Read(io, EfiPciIoWidthUint8, next, 1, &id);
        if (id == PCI_CAPABILITY_EXPRESS) {
            uint16_t flags = 0;
            io->Pci.Read(io, EfiPciIoWidthUint16, next + PCI_EXPRESS_FLAGS, 1, &flags);
            uint16_t type = flags & PCI_EXPRESS_FLAGS_TYPE;
            bool port = type == PCI_EXPRESS_TYPE_ROOT_PORT || type == PCI_EXPRESS_TYPE_DOWNSTREAM;
            return port && (flags & PCI_EXPRESS_FLAGS_SLOT) != 0;
        }
        uint32_t link = next + (uint32_t)1;
        next = 0;
        io->Pci.Read(io, EfiPciIoWidthUint8, link, 1, &next);
    }
    return false;
}

bool pci_io_device_in_slot(const struct pci_io_device *dev) {
    EFI_UINTN count;
    struct pci_io_device *devices = pci_io_devices(&count);
    bool slot = false;
    uint8_t bus = dev->address.bus;

    for (size_t depth = 0; depth < PCI_BUSES && !slot; depth++) {
        const struct pci_io_device *bridge = NULL;
        for (size_t i = 0; i < count && bridge == NULL; i++) {
            uint8_t secondary = 0;
            if (devices[i].base_class != PCI_CLASS_BRIDGE
             || devices[i].subclass != PCI_SUBCLASS_PCI_BRIDGE
             || devices[i].address.segment != dev->address.segment) {
                continue;
            }
            EFI_PCI_IO_PROTOCOL *io = devices[i].io;
            io->Pci.Read(io, EfiPciIoWidthUint8, PCI_SECONDARY_BUS, 1, &secondary);
            if (secondary != 0 && secondary == bus) {
                bridge = &devices[i];
            }
        }
        if (bridge == NULL) {
            break;
        }
        slot = port_to_slot(bridge->io);
        bus = bridge->address.bus;
    }

    if (devices != NULL) {
        efi_bs->FreePool(devices);
    }
    return slot;
}

// The upstream bridge of a function, or NULL.
static const struct pci_io_device *upstream_bridge(
    const struct pci_io_device *devices, EFI_UINTN count,
    const struct pci_io_device *dev
) {
    for (size_t i = 0; i < count; i++) {
        uint8_t secondary = 0;
        if (devices[i].base_class != PCI_CLASS_BRIDGE
         || devices[i].subclass != PCI_SUBCLASS_PCI_BRIDGE
         || devices[i].address.segment != dev->address.segment) {
            continue;
        }
        EFI_PCI_IO_PROTOCOL *io = devices[i].io;
        io->Pci.Read(io, EfiPciIoWidthUint8, PCI_SECONDARY_BUS, 1, &secondary);
        if (secondary != 0 && secondary == dev->address.bus) {
            return &devices[i];
        }
    }
    return NULL;
}

// Whether one of a function's I/O BARs holds a UART at the port.
static bool io_bar_holds(EFI_PCI_IO_PROTOCOL *io, uint16_t port) {
    uint8_t header_type = 0;
    io->Pci.Read(io, EfiPciIoWidthUint8, PCI_HEADER_TYPE, 1, &header_type);
    if ((header_type & PCI_HEADER_TYPE_MASK) != 0) {
        return false;
    }
    uint32_t bars[PCI_BARS];
    if (io->Pci.Read(io, EfiPciIoWidthUint32, PCI_BAR0, PCI_BARS, bars)
     != EFI_SUCCESS) {
        return false;
    }
    for (size_t i = 0; i < PCI_BARS; i++) {
        uint32_t base = bars[i] & PCI_BAR_IO_MASK;
        if ((bars[i] & PCI_BAR_IO) != 0 && base != 0
         && port >= base && port < base + UART_REGISTERS) {
            return true;
        }
    }
    return false;
}

static void enable_decoding(EFI_PCI_IO_PROTOCOL *io, bool memory) {
    EFI_UINT64 attributes = EFI_PCI_IO_ATTRIBUTE_IO;
    uint16_t bits = PCI_COMMAND_IO;
    if (memory) {
        attributes |= EFI_PCI_IO_ATTRIBUTE_MEMORY
            | EFI_PCI_IO_ATTRIBUTE_BUS_MASTER;
        bits |= PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER;
    }
    io->Attributes(io, EfiPciIoAttributeOperationEnable, attributes, NULL);
    // Some firmware leaves the command register as it was.
    uint16_t command = 0;
    io->Pci.Read(io, EfiPciIoWidthUint16, PCI_COMMAND, 1, &command);
    if ((command & bits) != bits) {
        command |= bits;
        io->Pci.Write(io, EfiPciIoWidthUint16, PCI_COMMAND, 1, &command);
    }
}

// Enables a function's decoding and that of the bridges above it.
static void enable_decoding_path(
    const struct pci_io_device *devices, EFI_UINTN count,
    const struct pci_io_device *dev, bool memory
) {
    const struct pci_io_device *bridge = dev;
    for (size_t depth = 0; depth < PCI_BUSES; depth++) {
        bridge = upstream_bridge(devices, count, bridge);
        if (bridge == NULL) {
            break;
        }
        enable_decoding(bridge->io, memory);
    }
    enable_decoding(dev->io, memory);
}

void pci_io_enable_device(const struct pci_io_device *dev) {
    EFI_UINTN count;
    struct pci_io_device *devices = pci_io_devices(&count);
    enable_decoding_path(devices, count, dev, true);
    if (devices != NULL) {
        efi_bs->FreePool(devices);
    }
}

bool pci_io_enable_io_port(uint16_t port, struct handoff_pci_address *found) {
    EFI_UINTN count;
    struct pci_io_device *devices = pci_io_devices(&count);
    const struct pci_io_device *dev = NULL;

    for (size_t i = 0; i < count && dev == NULL; i++) {
        if (io_bar_holds(devices[i].io, port)) {
            dev = &devices[i];
        }
    }
    if (dev != NULL) {
        *found = dev->address;
        enable_decoding_path(devices, count, dev, false);
    }

    if (devices != NULL) {
        efi_bs->FreePool(devices);
    }
    return dev != NULL;
}
