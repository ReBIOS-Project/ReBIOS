// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of ReBIOS.
// Copyright (C) 2026 Mintsuki and contributors
//
// ReBIOS is free software: you can redistribute it and/or modify it under the
// terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version.
//
// ReBIOS is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
// FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
// details.
//
// You should have received a copy of the GNU General Public License along
// with ReBIOS. If not, see <https://www.gnu.org/licenses/>.

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <efi.h>
#include <lib/log.h>
#include <loader/efi_glue.h>
#include <loader/pci_io.h>

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
    dev->prog_if = (uint8_t)(header[2] >> 8);
    dev->subclass = (uint8_t)(header[2] >> 16);
    dev->base_class = (uint8_t)(header[2] >> 24);
    return true;
}

struct pci_io_device *pci_io_devices(EFI_UINTN *count) {
    EFI_UINTN handle_count;
    EFI_HANDLE *handles = efi_locate_handles(&pci_io_guid, &handle_count);
    struct pci_io_device *devices = NULL;

    *count = 0;
    if (handles == NULL) {
        return NULL;
    }

    if (efi_bs->AllocatePool(EfiLoaderData, handle_count * sizeof(*devices),
                             (void **)&devices) != EFI_SUCCESS) {
        printf("Cannot allocate memory to list the PCI devices\n");
        devices = NULL;
    } else {
        for (EFI_UINTN i = 0; i < handle_count; i++) {
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
