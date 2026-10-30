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
#include <loader/bootdev.h>
#include <handoff.h>
#include <lib/log.h>
#include <loader/efi_glue.h>
#include <loader/loader.h>
#include <memory.h>

#define PCI_SUBCLASS 0x0a
#define PCI_BASE_CLASS 0x0b

static EFI_GUID loaded_image_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
static EFI_GUID device_path_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
static EFI_GUID block_io_guid = EFI_BLOCK_IO_PROTOCOL_GUID;
static EFI_GUID pci_io_guid = EFI_PCI_IO_PROTOCOL_GUID;

static void *protocol_get(EFI_HANDLE handle, EFI_GUID *guid) {
    void *interface;
    EFI_STATUS status = efi_bs->OpenProtocol(
        handle, guid, &interface, efi_image_handle, NULL,
        EFI_OPEN_PROTOCOL_GET_PROTOCOL
    );
    if (status != EFI_SUCCESS) {
        return NULL;
    }
    return interface;
}

// Only the device path's first instance counts.
static uint8_t device_path_bbs_type(EFI_DEVICE_PATH_PROTOCOL *path) {
    bool cdrom = false;

    for (EFI_DEVICE_PATH_PROTOCOL *node = path; !EfiIsDevicePathEndType(node);
         node = EfiNextDevicePathNode(node)) {
        if (EfiDevicePathNodeLength(node) < sizeof(EFI_DEVICE_PATH_PROTOCOL)) {
            break;
        }
        if (node->Type == EFI_MESSAGING_DEVICE_PATH
         && (node->SubType == EFI_MSG_USB_DP
          || node->SubType == EFI_MSG_USB_WWID_DP
          || node->SubType == EFI_MSG_USB_CLASS_DP)) {
            return EFI_BBS_TYPE_USB;
        }
        if (node->Type == EFI_MEDIA_DEVICE_PATH
         && node->SubType == EFI_MEDIA_CDROM_DP) {
            cdrom = true;
        }
    }

    return cdrom ? EFI_BBS_TYPE_CDROM : EFI_BBS_TYPE_HARDDRIVE;
}

static const char *bbs_type_name(uint8_t type) {
    switch (type) {
        case EFI_BBS_TYPE_CDROM: {
            return "CD-ROM";
        }
        case EFI_BBS_TYPE_USB: {
            return "USB";
        }
        default: {
            return "hard disk";
        }
    }
}

static bool config_read8(
    EFI_PCI_IO_PROTOCOL *pci_io, uint32_t offset, uint8_t *value
) {
    return pci_io->Pci.Read(pci_io, EfiPciIoWidthUint8, offset, 1, value)
        == EFI_SUCCESS;
}

// Describes the PCI controller on the device path, and the device type the
// path implies, in *dev. Returns false if there is no such controller.
static bool controller_resolve(
    EFI_DEVICE_PATH_PROTOCOL *path, struct handoff_block_device *dev
) {
    EFI_DEVICE_PATH_PROTOCOL *remaining = path;
    EFI_HANDLE pci_handle;
    if (efi_bs->LocateDevicePath(&pci_io_guid, &remaining, &pci_handle)
     != EFI_SUCCESS) {
        return false;
    }
    EFI_PCI_IO_PROTOCOL *pci_io = protocol_get(pci_handle, &pci_io_guid);
    if (pci_io == NULL) {
        return false;
    }

    EFI_UINTN segment;
    EFI_UINTN bus;
    EFI_UINTN device;
    EFI_UINTN function;
    if (pci_io->GetLocation(pci_io, &segment, &bus, &device, &function)
     != EFI_SUCCESS
     || segment > 0xffff || bus > 0xff || device > 31 || function > 7) {
        return false;
    }

    uint8_t base_class;
    uint8_t subclass;
    if (!config_read8(pci_io, PCI_BASE_CLASS, &base_class)
     || !config_read8(pci_io, PCI_SUBCLASS, &subclass)) {
        return false;
    }

    memset(dev, 0, sizeof(struct handoff_block_device));
    dev->address.segment = segment;
    dev->address.bus = bus;
    dev->address.devfn = device << 3 | function;
    dev->bbs_type = device_path_bbs_type(path);
    dev->base_class = base_class;
    dev->subclass = subclass;
    return true;
}

static void device_log(
    const char *what, const struct handoff_block_device *dev
) {
    printf(
        "%s %04x:%02x:%02x.%x, %s, class %02x:%02x%s\n", what,
        dev->address.segment, dev->address.bus, dev->address.devfn >> 3,
        dev->address.devfn & 7, bbs_type_name(dev->bbs_type), dev->base_class,
        dev->subclass, dev->boot ? ", boot" : ""
    );
}

// Says why if there is no boot device. *block is whether the device itself has
// a block I/O interface.
static bool boot_device_find(struct handoff_block_device *boot, bool *block) {
    EFI_LOADED_IMAGE_PROTOCOL *image = protocol_get(
        efi_image_handle, &loaded_image_guid
    );
    if (image == NULL || image->DeviceHandle == NULL) {
        printf("No boot device: the loader's image names no device\n");
        return false;
    }
    EFI_DEVICE_PATH_PROTOCOL *path = protocol_get(
        image->DeviceHandle, &device_path_guid
    );
    if (path == NULL) {
        printf("No boot device: the loader's device has no device path\n");
        return false;
    }
    if (!controller_resolve(path, boot)) {
        printf(
            "No boot device: the loader's device is not on a PCI controller\n"
        );
        return false;
    }
    *block = protocol_get(image->DeviceHandle, &block_io_guid) != NULL;
    return true;
}

static uint32_t address_key(const struct handoff_pci_address *address) {
    return (uint32_t)address->segment << 16 | (uint32_t)address->bus << 8
        | address->devfn;
}

// The boot controller's records go first, the boot device's own type leading
// them, then the others by controller and type.
static bool record_before(
    const struct handoff_block_device *a, const struct handoff_block_device *b,
    uint8_t boot_type
) {
    if (a->boot != b->boot) {
        return a->boot;
    }
    if (a->boot && (a->bbs_type == boot_type) != (b->bbs_type == boot_type)) {
        return a->bbs_type == boot_type;
    }
    if (address_key(&a->address) != address_key(&b->address)) {
        return address_key(&a->address) < address_key(&b->address);
    }
    return a->bbs_type < b->bbs_type;
}

// The records are kept in order, so that, once they are full, what would go
// last is what is dropped, never the boot controller's.
static void record(const struct handoff_block_device *dev, uint8_t boot_type) {
    struct handoff_block_device *records = loader_handoff->block_devices;
    size_t count = loader_handoff->block_device_count;

    for (size_t i = 0; i < count; i++) {
        if (address_key(&records[i].address) == address_key(&dev->address)
         && records[i].bbs_type == dev->bbs_type) {
            return;
        }
    }

    size_t pos = count;
    while (pos > 0 && record_before(dev, &records[pos - 1], boot_type)) {
        pos--;
    }

    if (count == HANDOFF_MAX_BLOCK_DEVICES) {
        if (pos == count) {
            device_log("Too many block devices, skipping", dev);
            return;
        }
        device_log("Too many block devices, skipping", &records[count - 1]);
        count--;
    }

    memmove(
        &records[pos + 1], &records[pos],
        (count - pos) * sizeof(struct handoff_block_device)
    );
    records[pos] = *dev;
    loader_handoff->block_device_count = count + 1;
}

void bootdev_collect(void) {
    struct handoff_block_device boot;
    bool boot_block = false;
    memset(&boot, 0, sizeof(struct handoff_block_device));
    loader_handoff->block_device_count = 0;

    bool have_boot = boot_device_find(&boot, &boot_block);
    if (have_boot) {
        device_log("Boot device", &boot);
        boot.boot = true;
        // Partitions are skipped below, which would lose the type of a boot
        // device that is one, such as the El Torito image of a CD-ROM.
        if (boot_block) {
            record(&boot, boot.bbs_type);
        }
    }

    size_t handle_count = 0;
    EFI_HANDLE *handles = NULL;
    EFI_STATUS status = efi_bs->LocateHandleBuffer(
        EfiByProtocol, &block_io_guid, NULL, &handle_count, &handles
    );
    if (status != EFI_SUCCESS) {
        handle_count = 0;
        handles = NULL;
    }

    for (size_t i = 0; i < handle_count; i++) {
        EFI_BLOCK_IO_PROTOCOL *block_io = protocol_get(
            handles[i], &block_io_guid
        );
        if (block_io == NULL || block_io->Media == NULL
         || block_io->Media->LogicalPartition) {
            continue;
        }
        EFI_DEVICE_PATH_PROTOCOL *path = protocol_get(
            handles[i], &device_path_guid
        );
        struct handoff_block_device dev;
        if (path == NULL || !controller_resolve(path, &dev)) {
            continue;
        }
        dev.boot = have_boot
            && address_key(&dev.address) == address_key(&boot.address);
        record(&dev, boot.bbs_type);
    }

    if (handles != NULL) {
        efi_bs->FreePool(handles);
    }

    if (loader_handoff->block_device_count == 0) {
        printf("No block devices on PCI controllers\n");
    }
    for (size_t i = 0; i < loader_handoff->block_device_count; i++) {
        device_log("Block device", &loader_handoff->block_devices[i]);
    }
}
