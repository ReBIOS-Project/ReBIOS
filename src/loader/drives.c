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

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>
#include <efi.h>
#include <memory.h>
#include <handoff.h>
#include <lib/log.h>
#include <lib/printf.h>
#include <loader/config.h>
#include <loader/drives.h>
#include <loader/efi_glue.h>
#include <loader/loader.h>

#define MAX_DRIVES 32
#define MAX_PATH_NODES 256
#define TEXT_MAX 256
#define NAME_MAX 160
#define MODEL_MAX 64
#define PCI_DEPTH_MAX 8

// The Disk Info protocol, PI 1.10 Vol. V, 7.3.10 to 7.3.13, which the
// firmware's storage drivers install on the drives they find.
#define DISK_INFO_PROTOCOL_GUID \
    {0xd432a67f, 0x14dc, 0x484b, {0xb3, 0xbb, 0x3f, 0x02, 0x91, 0x84, 0x93, 0x27}}
#define DISK_INFO_IDE_GUID \
    {0x5e948fe3, 0x26d3, 0x42b5, {0xaf, 0x17, 0x61, 0x02, 0x87, 0x18, 0x8d, 0xec}}
#define DISK_INFO_AHCI_GUID \
    {0x9e498932, 0x4abc, 0x45af, {0xa3, 0x4d, 0x02, 0x47, 0x78, 0x7b, 0xe7, 0xc6}}
#define DISK_INFO_SCSI_GUID \
    {0x08f74baa, 0xea36, 0x41d9, {0x95, 0x21, 0x21, 0xa7, 0x0f, 0x87, 0x80, 0xbc}}
#define DISK_INFO_USB_GUID \
    {0xcb871572, 0xc11a, 0x47b5, {0xb4, 0x92, 0x67, 0x5e, 0xaf, 0xa7, 0x77, 0x27}}

struct disk_info {
    EFI_GUID interface;
    EFI_STATUS (EFI_API *inquiry)(struct disk_info *self, void *data, uint32_t *size);
    EFI_STATUS (EFI_API *identify)(struct disk_info *self, void *data, uint32_t *size);
    EFI_STATUS (EFI_API *sense_data)(struct disk_info *self, void *data, uint32_t *size,
                                     uint8_t *count);
    EFI_STATUS (EFI_API *which_ide)(struct disk_info *self, uint32_t *channel, uint32_t *device);
};

// ATA8-ACS T13/1699-D revision 4a, 7.16.7.13: IDENTIFY DEVICE words 27-46,
// the first character of each pair in the word's upper byte.
#define ATA_MODEL_OFFSET 54
#define ATA_MODEL_LENGTH 40
// USB Mass Storage Class UFI 1.0, table 10: standard INQUIRY data.
#define INQUIRY_VENDOR_OFFSET 8
#define INQUIRY_VENDOR_LENGTH 8
#define INQUIRY_PRODUCT_OFFSET 16
#define INQUIRY_PRODUCT_LENGTH 16
#define INQUIRY_LENGTH 36
// NVM Express Base 2.4, 5.2.14 and 4.5.2: the Identify command, its Identify
// Controller data structure, and the Model Number in it.
#define NVME_ADMIN_IDENTIFY 0x06
#define NVME_CNS_CONTROLLER 0x01
#define NVME_IDENTIFY_SIZE 4096
#define NVME_MODEL_OFFSET 24
#define NVME_MODEL_LENGTH 40
#define NVME_TIMEOUT_100NS 10000000

// xHCI 1.2, 5.3.6 and 7.2: the extended capabilities' pointer in HCCPARAMS1,
// and the Supported Protocol capability's major revision and its range of
// root hub ports.
#define XHCI_HCCPARAMS1 0x10
#define XHCI_CAP_SUPPORTED_PROTOCOL 2
#define XHCI_CAP_MAX 64
#define PCI_CLASS_SERIAL_USB_XHCI 0x0c0330
#define PCI_BASE_CLASS_STORAGE 0x01
#define PCI_CLASS_SERIAL_USB 0x0c03

// El Torito 1.0, figure 7: the Boot Record Volume Descriptor, in sector 17.
#define EL_TORITO_SECTOR 17
#define EL_TORITO_ID "EL TORITO SPECIFICATION"

#define MBR_SIGNATURE_OFFSET 510
#define MBR_PARTITIONS 446
#define MBR_PARTITION_SIZE 16
#define MBR_TYPE_OFFSET 4
#define MBR_TYPE_GPT_PROTECTIVE 0xee

#define ACPI_PNP0A03 0x0a0341d0
#define ACPI_PNP0A08 0x0a0841d0

struct drive {
    char text[TEXT_MAX];
    char name[NAME_MAX];
    char model[MODEL_MAX];
    uint64_t size;
    bool optical;
    bool external;
    bool bootable;
    bool self;
    unsigned rank;
};

// The order drives are tried in; those of a rank go by their SeaBIOS names.
enum rank {
    RANK_CHOSEN,
    RANK_SELF,
    RANK_INTERNAL_BOOTABLE,
    RANK_OPTICAL_BOOTABLE,
    RANK_EXTERNAL_BOOTABLE,
    RANK_OPTICAL,
    RANK_INTERNAL,
    RANK_EXTERNAL,
    RANK_SELF_LAST,
};

static EFI_GUID loaded_image_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
static EFI_GUID device_path_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
static EFI_GUID block_io_guid = EFI_BLOCK_IO_PROTOCOL_GUID;
static EFI_GUID pci_io_guid = EFI_PCI_IO_PROTOCOL_GUID;
static EFI_GUID nvme_guid = EFI_NVM_EXPRESS_PASS_THRU_PROTOCOL_GUID;
static EFI_GUID path_to_text_guid = EFI_DEVICE_PATH_TO_TEXT_PROTOCOL_GUID;
static EFI_GUID disk_info_guid = DISK_INFO_PROTOCOL_GUID;
static EFI_GUID disk_info_ide = DISK_INFO_IDE_GUID;
static EFI_GUID disk_info_ahci = DISK_INFO_AHCI_GUID;
static EFI_GUID disk_info_scsi = DISK_INFO_SCSI_GUID;
static EFI_GUID disk_info_usb = DISK_INFO_USB_GUID;

static struct drive drives[MAX_DRIVES];
static unsigned drive_count;

static void *protocol_get(EFI_HANDLE handle, EFI_GUID *guid) {
    void *interface;
    if (efi_bs->OpenProtocol(handle, guid, &interface, efi_image_handle, NULL,
                             EFI_OPEN_PROTOCOL_GET_PROTOCOL) != EFI_SUCCESS) {
        return NULL;
    }
    return interface;
}

static bool guid_equal(const EFI_GUID *a, const EFI_GUID *b) {
    return memcmp(a, b, sizeof(*a)) == 0;
}

static char lower(char c) {
    return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

static bool word_is(const char *s, const char *word) {
    size_t i = 0;
    for (; word[i] != 0; i++) {
        if (lower(s[i]) != word[i]) {
            return false;
        }
    }
    return s[i] == 0;
}

static bool root_bridge(EFI_DEVICE_PATH_PROTOCOL *node) {
    if (node->Type != EFI_ACPI_DEVICE_PATH) {
        return false;
    }
    if (node->SubType == EFI_ACPI_DP
        && EfiDevicePathNodeLength(node) >= sizeof(EFI_ACPI_HID_DEVICE_PATH)) {
        uint32_t hid = ((EFI_ACPI_HID_DEVICE_PATH *)node)->HID;
        return hid == ACPI_PNP0A03 || hid == ACPI_PNP0A08;
    }
    if (node->SubType == EFI_ACPI_EXTENDED_DP
        && EfiDevicePathNodeLength(node) >= offsetof(EFI_ACPI_EXTENDED_HID_DEVICE_PATH, HidStr)) {
        EFI_ACPI_EXTENDED_HID_DEVICE_PATH *acpi = (EFI_ACPI_EXTENDED_HID_DEVICE_PATH *)node;
        return acpi->HID == ACPI_PNP0A03 || acpi->HID == ACPI_PNP0A08 || acpi->CID == ACPI_PNP0A03
               || acpi->CID == ACPI_PNP0A08;
    }
    return false;
}

static size_t text_length(const char *s) {
    size_t n = 0;
    while (s[n] != 0) {
        n++;
    }
    return n;
}

static void append(char *out, size_t size, const char *fmt, ...) {
    size_t used = text_length(out);
    if (used + 1 >= size) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    vsnprintf(out + used, size - used, fmt, args);
    va_end(args);
}

// Copies the printable characters of a fixed-length string, without the
// padding around them.
static void copy_text(char *out, size_t size, const uint8_t *s, size_t length) {
    while (length > 0 && (s[length - 1] == ' ' || s[length - 1] == 0)) {
        length--;
    }
    while (length > 0 && *s == ' ') {
        s++;
        length--;
    }
    size_t n = 0;
    for (size_t i = 0; i < length && n + 1 < size; i++) {
        out[n++] = s[i] >= 0x20 && s[i] < 0x7f ? (char)s[i] : '?';
    }
    out[n] = 0;
}

// A node shorter than its header would make the walks below loop for ever.
static bool path_valid(EFI_DEVICE_PATH_PROTOCOL *path) {
    EFI_DEVICE_PATH_PROTOCOL *node = path;
    for (unsigned i = 0; i < MAX_PATH_NODES; i++) {
        if (EfiDevicePathNodeLength(node) < sizeof(*node)) {
            return false;
        }
        if (EfiIsDevicePathEndType(node)) {
            return true;
        }
        node = EfiNextDevicePathNode(node);
    }
    return false;
}

// The bytes of the path's nodes, without its end node.
static size_t path_length(EFI_DEVICE_PATH_PROTOCOL *path) {
    size_t length = 0;
    for (EFI_DEVICE_PATH_PROTOCOL *node = path; !EfiIsDevicePathEndType(node);
         node = EfiNextDevicePathNode(node)) {
        length += EfiDevicePathNodeLength(node);
    }
    return length;
}

static bool path_starts_with(EFI_DEVICE_PATH_PROTOCOL *path, EFI_DEVICE_PATH_PROTOCOL *prefix) {
    size_t length = path_length(prefix);
    return path_length(path) >= length && memcmp(path, prefix, length) == 0;
}

static void path_text(EFI_DEVICE_PATH_PROTOCOL *path, char *out, size_t size) {
    static EFI_DEVICE_PATH_TO_TEXT_PROTOCOL *to_text;
    if (to_text == NULL
        && efi_bs->LocateProtocol(&path_to_text_guid, NULL, (void **)&to_text) != EFI_SUCCESS) {
        to_text = NULL;
    }
    out[0] = 0;
    EFI_CHAR16 *text = to_text != NULL ? to_text->ConvertDevicePathToText(path, false, false) : NULL;
    if (text == NULL) {
        // Without the firmware's rendering, its nodes' types and subtypes.
        for (EFI_DEVICE_PATH_PROTOCOL *node = path; !EfiIsDevicePathEndType(node);
             node = EfiNextDevicePathNode(node)) {
            append(out, size, "%sPath(%u,%u)", out[0] != 0 ? "/" : "", node->Type, node->SubType);
        }
        return;
    }
    size_t n = 0;
    for (size_t i = 0; text[i] != 0 && n + 1 < size; i++) {
        out[n++] = text[i] >= 0x20 && text[i] < 0x7f ? (char)text[i] : '?';
    }
    out[n] = 0;
    efi_bs->FreePool(text);
}

// The location of the PCI function at the end of the path's first nodes.
static bool pci_location(EFI_DEVICE_PATH_PROTOCOL *path, size_t length, EFI_PCI_IO_PROTOCOL **pci_io,
                         EFI_UINTN *segment, EFI_UINTN *bus) {
    uint8_t buffer[512];
    if (length + EFI_END_DEVICE_PATH_LENGTH > sizeof(buffer)) {
        return false;
    }
    memcpy(buffer, path, length);
    EfiSetDevicePathEndNode((EFI_DEVICE_PATH_PROTOCOL *)(buffer + length));
    EFI_DEVICE_PATH_PROTOCOL *remaining = (EFI_DEVICE_PATH_PROTOCOL *)buffer;
    EFI_HANDLE handle;
    if (efi_bs->LocateDevicePath(&pci_io_guid, &remaining, &handle) != EFI_SUCCESS
        || !EfiIsDevicePathEndType(remaining)) {
        return false;
    }
    *pci_io = protocol_get(handle, &pci_io_guid);
    EFI_UINTN device;
    EFI_UINTN function;
    return *pci_io != NULL
           && (*pci_io)->GetLocation(*pci_io, segment, bus, &device, &function) == EFI_SUCCESS;
}

static uint32_t pci_class(EFI_PCI_IO_PROTOCOL *pci_io) {
    uint32_t value = 0;
    if (pci_io->Pci.Read(pci_io, EfiPciIoWidthUint32, 0x08, 1, &value) != EFI_SUCCESS) {
        return 0;
    }
    return value >> 8;
}

static bool xhci_read(EFI_PCI_IO_PROTOCOL *pci_io, uint32_t offset, uint32_t *value) {
    return pci_io->Mem.Read(pci_io, EfiPciIoWidthUint32, 0, offset, 1, value) == EFI_SUCCESS;
}

// SeaBIOS numbers an xHCI root hub port by its place among the ports of its
// USB revision, as the Supported Protocol capabilities give them, the last
// capability of a revision counting (SeaBIOS src/hw/usb-xhci.c,
// xhci_hub_portmap()).
static uint32_t xhci_port(EFI_PCI_IO_PROTOCOL *pci_io, uint32_t port) {
    uint32_t hccparams1;
    if (!xhci_read(pci_io, XHCI_HCCPARAMS1, &hccparams1)) {
        return port + 1;
    }
    uint32_t start[4] = {0};
    uint32_t count[4] = {0};
    uint32_t offset = (hccparams1 >> 16) * 4;
    for (unsigned i = 0; offset != 0 && i < XHCI_CAP_MAX; i++) {
        uint32_t header;
        if (!xhci_read(pci_io, offset, &header)) {
            break;
        }
        uint32_t major = header >> 24;
        uint32_t ports;
        if ((header & 0xff) == XHCI_CAP_SUPPORTED_PROTOCOL && (major == 2 || major == 3)
            && xhci_read(pci_io, offset + 8, &ports)) {
            start[major] = ports & 0xff;
            count[major] = (ports >> 8) & 0xff;
        }
        uint32_t next = (header >> 8) & 0xff;
        offset = next != 0 ? offset + next * 4 : 0;
    }
    uint32_t mapped = port + 1;
    for (uint32_t major = 3; major >= 2; major--) {
        if (port + 1 >= start[major] && port + 1 < start[major] + count[major]) {
            mapped = port + 2 - start[major];
        }
    }
    return mapped;
}

// The drive's name as SeaBIOS's bootorder file has it (SeaBIOS src/boot.c,
// build_pci_path() and the bootprio_find_*() functions), or false where
// SeaBIOS could not boot it.
static bool seabios_name(EFI_DEVICE_PATH_PROTOCOL *path, char *out, size_t size) {
    EFI_PCI_DEVICE_PATH *pci[PCI_DEPTH_MAX];
    unsigned depth = 0;
    size_t first_pci_end = 0;
    size_t offset = 0;
    EFI_DEVICE_PATH_PROTOCOL *node = path;
    for (; !EfiIsDevicePathEndType(node); node = EfiNextDevicePathNode(node)) {
        if (node->Type == EFI_ACPI_DEVICE_PATH && depth == 0) {
            if (!root_bridge(node)) {
                return false;
            }
        } else if (node->Type == EFI_HARDWARE_DEVICE_PATH && node->SubType == EFI_HW_PCI_DP
                   && EfiDevicePathNodeLength(node) >= sizeof(EFI_PCI_DEVICE_PATH)) {
            if (depth == PCI_DEPTH_MAX) {
                return false;
            }
            pci[depth++] = (EFI_PCI_DEVICE_PATH *)node;
            if (depth == 1) {
                first_pci_end = offset + EfiDevicePathNodeLength(node);
            }
        } else {
            break;
        }
        offset += EfiDevicePathNodeLength(node);
    }
    if (depth == 0) {
        return false;
    }

    EFI_PCI_IO_PROTOCOL *root_io;
    EFI_UINTN segment;
    EFI_UINTN root_bus;
    EFI_PCI_IO_PROTOCOL *controller_io;
    EFI_UINTN controller_segment;
    EFI_UINTN controller_bus;
    if (!pci_location(path, first_pci_end, &root_io, &segment, &root_bus) || segment != 0
        || !pci_location(path, offset, &controller_io, &controller_segment, &controller_bus)) {
        return false;
    }

    bool usb = node->Type == EFI_MESSAGING_DEVICE_PATH && node->SubType == EFI_MSG_USB_DP;
    out[0] = 0;
    append(out, size, "/pci@i0cf8");
    if (root_bus != 0) {
        append(out, size, ",%x", (unsigned)root_bus);
    }
    for (unsigned i = 0; i < depth; i++) {
        const char *kind = i + 1 < depth ? "pci-bridge" : usb ? "usb" : "*";
        append(out, size, "/%s@%x", kind, pci[i]->Device);
        if (pci[i]->Function != 0) {
            append(out, size, ",%x", pci[i]->Function);
        }
    }

    if (EfiIsDevicePathEndType(node)) {
        return true;
    }
    if (node->Type != EFI_MESSAGING_DEVICE_PATH) {
        return false;
    }
    switch (node->SubType) {
        case EFI_MSG_SATA_DP: {
            EFI_SATA_DEVICE_PATH *sata = (EFI_SATA_DEVICE_PATH *)node;
            // SeaBIOS drives no port multipliers.
            if (sata->PortMultiplierPortNumber != 0xffff) {
                return false;
            }
            append(out, size, "/drive@%x/disk@0", sata->HBAPortNumber);
            node = EfiNextDevicePathNode(node);
            break;
        }
        case EFI_MSG_ATAPI_DP: {
            EFI_ATAPI_DEVICE_PATH *ata = (EFI_ATAPI_DEVICE_PATH *)node;
            append(out, size, "/drive@%x/disk@%x", ata->PrimarySecondary, ata->SlaveMaster);
            node = EfiNextDevicePathNode(node);
            break;
        }
        case EFI_MSG_SCSI_DP: {
            EFI_SCSI_DEVICE_PATH *scsi = (EFI_SCSI_DEVICE_PATH *)node;
            append(out, size, "/*@0/*@%x,%x", scsi->Pun, scsi->Lun);
            node = EfiNextDevicePathNode(node);
            break;
        }
        // SeaBIOS orders these by their controllers alone.
        case EFI_MSG_NVME_NAMESPACE_DP:
        case EFI_MSG_SD_DP:
        case EFI_MSG_EMMC_DP: {
            node = EfiNextDevicePathNode(node);
            break;
        }
        case EFI_MSG_USB_DP: {
            bool xhci = pci_class(controller_io) == PCI_CLASS_SERIAL_USB_XHCI;
            for (bool root = true; node->Type == EFI_MESSAGING_DEVICE_PATH
                                   && node->SubType == EFI_MSG_USB_DP; root = false) {
                uint32_t port = ((EFI_USB_DEVICE_PATH *)node)->ParentPortNumber;
                port = root && xhci ? xhci_port(controller_io, port) : port + 1;
                node = EfiNextDevicePathNode(node);
                bool last = node->Type != EFI_MESSAGING_DEVICE_PATH
                            || node->SubType != EFI_MSG_USB_DP;
                append(out, size, last ? "/storage@%x" : "/hub@%x", port);
            }
            uint32_t lun = 0;
            if (node->Type == EFI_MESSAGING_DEVICE_PATH
                && node->SubType == EFI_MSG_DEVICE_LOGICAL_UNIT_DP) {
                lun = ((EFI_DEVICE_LOGICAL_UNIT_DEVICE_PATH *)node)->Lun;
                node = EfiNextDevicePathNode(node);
            }
            append(out, size, "/*@0/*@0,%x", lun);
            break;
        }
        default: {
            return false;
        }
    }
    return EfiIsDevicePathEndType(node) && text_length(out) + 1 < size;
}

static void ata_model(struct disk_info *info, char *out, size_t size) {
    uint8_t identify[512];
    uint32_t length = sizeof(identify);
    if (info->identify(info, identify, &length) != EFI_SUCCESS
        || length < ATA_MODEL_OFFSET + ATA_MODEL_LENGTH) {
        return;
    }
    uint8_t model[ATA_MODEL_LENGTH];
    for (unsigned i = 0; i < ATA_MODEL_LENGTH; i += 2) {
        model[i] = identify[ATA_MODEL_OFFSET + i + 1];
        model[i + 1] = identify[ATA_MODEL_OFFSET + i];
    }
    copy_text(out, size, model, sizeof(model));
}

static void scsi_model(struct disk_info *info, char *out, size_t size) {
    uint8_t inquiry[96];
    uint32_t length = sizeof(inquiry);
    if (info->inquiry(info, inquiry, &length) != EFI_SUCCESS || length < INQUIRY_LENGTH) {
        return;
    }
    char vendor[INQUIRY_VENDOR_LENGTH + 1];
    char product[INQUIRY_PRODUCT_LENGTH + 1];
    copy_text(vendor, sizeof(vendor), inquiry + INQUIRY_VENDOR_OFFSET, INQUIRY_VENDOR_LENGTH);
    copy_text(product, sizeof(product), inquiry + INQUIRY_PRODUCT_OFFSET, INQUIRY_PRODUCT_LENGTH);
    out[0] = 0;
    append(out, size, "%s%s%s", vendor, vendor[0] != 0 && product[0] != 0 ? " " : "", product);
}

static void nvme_model(EFI_DEVICE_PATH_PROTOCOL *path, char *out, size_t size) {
    EFI_DEVICE_PATH_PROTOCOL *remaining = path;
    EFI_HANDLE handle;
    if (efi_bs->LocateDevicePath(&nvme_guid, &remaining, &handle) != EFI_SUCCESS) {
        return;
    }
    EFI_NVM_EXPRESS_PASS_THRU_PROTOCOL *nvme = protocol_get(handle, &nvme_guid);
    uint64_t buffer = efi_alloc_pages(1, 0);
    if (nvme == NULL || buffer == 0) {
        return;
    }
    EFI_NVM_EXPRESS_COMMAND command;
    EFI_NVM_EXPRESS_COMPLETION completion;
    EFI_NVM_EXPRESS_PASS_THRU_COMMAND_PACKET packet;
    memset(&command, 0, sizeof(command));
    memset(&completion, 0, sizeof(completion));
    memset(&packet, 0, sizeof(packet));
    command.Cdw0.OpCode = NVME_ADMIN_IDENTIFY;
    command.Cdw10 = NVME_CNS_CONTROLLER;
    command.Flags = EFI_CDW10_VALID;
    packet.CommandTimeout = NVME_TIMEOUT_100NS;
    packet.TransferBuffer = (void *)(uintptr_t)buffer;
    packet.TransferLength = NVME_IDENTIFY_SIZE;
    packet.NvmeCmd = &command;
    packet.NvmeCompletion = &completion;
    if (nvme->PassThru(nvme, 0, &packet, NULL) == EFI_SUCCESS) {
        copy_text(out, size, (uint8_t *)(uintptr_t)buffer + NVME_MODEL_OFFSET, NVME_MODEL_LENGTH);
    }
    efi_bs->FreePages(buffer, 1);
}

static void drive_model(EFI_HANDLE handle, EFI_DEVICE_PATH_PROTOCOL *path, char *out, size_t size) {
    out[0] = 0;
    struct disk_info *info = protocol_get(handle, &disk_info_guid);
    if (info != NULL && (guid_equal(&info->interface, &disk_info_ide)
                         || guid_equal(&info->interface, &disk_info_ahci))) {
        ata_model(info, out, size);
    } else if (info != NULL && (guid_equal(&info->interface, &disk_info_scsi)
                                || guid_equal(&info->interface, &disk_info_usb))) {
        scsi_model(info, out, size);
    }
    if (out[0] == 0) {
        nvme_model(path, out, size);
    }
}

// Whether the medium holds what the legacy BIOS boots: a master boot record
// with code, not GPT's protective one, or an El Torito boot record.
static bool legacy_bootable(EFI_BLOCK_IO_PROTOCOL *block_io, bool optical) {
    EFI_BLOCK_IO_MEDIA *media = block_io->Media;
    if (!media->MediaPresent || media->BlockSize < 512 || media->BlockSize > 4096) {
        return false;
    }
    uint64_t buffer = efi_alloc_pages(1, 0);
    if (buffer == 0) {
        return false;
    }
    uint8_t *sector = (uint8_t *)(uintptr_t)buffer;
    bool bootable = false;
    if (optical) {
        if (block_io->ReadBlocks(block_io, media->MediaId, EL_TORITO_SECTOR, media->BlockSize,
                                 sector) == EFI_SUCCESS) {
            bootable = sector[0] == 0 && memcmp(sector + 1, "CD001", 5) == 0 && sector[6] == 1
                       && memcmp(sector + 7, EL_TORITO_ID, sizeof(EL_TORITO_ID) - 1) == 0;
        }
    } else if (block_io->ReadBlocks(block_io, media->MediaId, 0, media->BlockSize, sector)
               == EFI_SUCCESS) {
        bootable = sector[MBR_SIGNATURE_OFFSET] == 0x55 && sector[MBR_SIGNATURE_OFFSET + 1] == 0xaa;
        for (unsigned i = 0; i < 4; i++) {
            if (sector[MBR_PARTITIONS + i * MBR_PARTITION_SIZE + MBR_TYPE_OFFSET]
                == MBR_TYPE_GPT_PROTECTIVE) {
                bootable = false;
            }
        }
        bool code = false;
        for (unsigned i = 0; i < MBR_PARTITIONS; i++) {
            code |= sector[i] != 0;
        }
        bootable = bootable && code;
    }
    efi_bs->FreePages(buffer, 1);
    return bootable;
}

static bool path_has_usb(EFI_DEVICE_PATH_PROTOCOL *path) {
    for (EFI_DEVICE_PATH_PROTOCOL *node = path; !EfiIsDevicePathEndType(node);
         node = EfiNextDevicePathNode(node)) {
        if (node->Type == EFI_MESSAGING_DEVICE_PATH && node->SubType == EFI_MSG_USB_DP) {
            return true;
        }
    }
    return false;
}

static EFI_DEVICE_PATH_PROTOCOL *own_path(void) {
    EFI_LOADED_IMAGE_PROTOCOL *image = protocol_get(efi_image_handle, &loaded_image_guid);
    if (image == NULL || image->DeviceHandle == NULL) {
        return NULL;
    }
    EFI_DEVICE_PATH_PROTOCOL *path = protocol_get(image->DeviceHandle, &device_path_guid);
    return path != NULL && path_valid(path) ? path : NULL;
}

static void drive_add(EFI_HANDLE handle, EFI_DEVICE_PATH_PROTOCOL *own) {
    EFI_BLOCK_IO_PROTOCOL *block_io = protocol_get(handle, &block_io_guid);
    EFI_DEVICE_PATH_PROTOCOL *path = protocol_get(handle, &device_path_guid);
    if (block_io == NULL || block_io->Media == NULL || block_io->Media->LogicalPartition
        || path == NULL || !path_valid(path)) {
        return;
    }
    struct drive d;
    memset(&d, 0, sizeof(d));
    path_text(path, d.text, sizeof(d.text));
    if (!seabios_name(path, d.name, sizeof(d.name))) {
        printf("Drive %s: not one the legacy BIOS can boot\n", d.text);
        return;
    }
    for (unsigned i = 0; i < drive_count; i++) {
        // NVMe namespaces share their controller's name.
        if (memcmp(drives[i].name, d.name, sizeof(d.name)) == 0) {
            return;
        }
    }
    if (drive_count == MAX_DRIVES) {
        printf("Drive %s: too many drives, left out\n", d.text);
        return;
    }
    // CD and DVD drives have 2048-byte blocks, as nothing else here does.
    d.optical = block_io->Media->BlockSize == 2048;
    d.external = path_has_usb(path) || (!d.optical && block_io->Media->RemovableMedia);
    d.size = block_io->Media->MediaPresent
                 ? (block_io->Media->LastBlock + 1) * block_io->Media->BlockSize
                 : 0;
    d.self = own != NULL && path_starts_with(own, path);
    drive_model(handle, path, d.model, sizeof(d.model));
    d.bootable = legacy_bootable(block_io, d.optical);
    drives[drive_count++] = d;
}

// A device path matches whatever the drive's path starts with, case and
// spaces aside, as a path with its partition does.
static bool path_matches(const char *wanted, const char *text) {
    size_t i = 0;
    size_t j = 0;
    for (;;) {
        while (wanted[i] == ' ' || wanted[i] == '\t') {
            i++;
        }
        while (text[j] == ' ') {
            j++;
        }
        if (text[j] == 0) {
            return wanted[i] == 0 || wanted[i] == '/';
        }
        if (lower(wanted[i]) != lower(text[j])) {
            return false;
        }
        i++;
        j++;
    }
}

static bool model_matches(const char *wanted, const char *model) {
    size_t length = text_length(wanted);
    if (length == 0) {
        return false;
    }
    for (size_t start = 0; model[start] != 0; start++) {
        size_t k = 0;
        while (k < length && model[start + k] != 0 && lower(model[start + k]) == lower(wanted[k])) {
            k++;
        }
        if (k == length) {
            return true;
        }
    }
    return false;
}

static bool wanted_is_path(const char *wanted) {
    for (size_t i = 0; wanted[i] != 0; i++) {
        if (wanted[i] == '(') {
            return true;
        }
    }
    return false;
}

static void rank_drives(void) {
    const char *wanted = config.boot_drive;
    bool automatic = word_is(wanted, "auto");
    bool named = wanted[0] != 0 && !automatic && !word_is(wanted, "self");
    bool chosen = false;
    for (unsigned i = 0; i < drive_count; i++) {
        struct drive *d = &drives[i];
        if (named && !chosen && (wanted_is_path(wanted) ? path_matches(wanted, d->text)
                                                        : model_matches(wanted, d->model))) {
            d->rank = RANK_CHOSEN;
            chosen = true;
        } else if (d->self && (automatic || named)) {
            d->rank = RANK_SELF_LAST;
        } else if (d->self && d->bootable) {
            d->rank = RANK_SELF;
        } else if (d->optical) {
            d->rank = d->bootable ? RANK_OPTICAL_BOOTABLE : RANK_OPTICAL;
        } else if (d->external) {
            d->rank = d->bootable ? RANK_EXTERNAL_BOOTABLE : RANK_EXTERNAL;
        } else {
            d->rank = d->bootable ? RANK_INTERNAL_BOOTABLE : RANK_INTERNAL;
        }
    }
    if (named && !chosen) {
        printf("boot_drive names no drive the firmware knows, ordering as with self\n");
        for (unsigned i = 0; i < drive_count; i++) {
            if (drives[i].self && drives[i].bootable) {
                drives[i].rank = RANK_SELF;
            }
        }
    }
}

static bool drive_before(const struct drive *a, const struct drive *b) {
    if (a->rank != b->rank) {
        return a->rank < b->rank;
    }
    for (size_t i = 0; i < sizeof(a->name); i++) {
        if (a->name[i] != b->name[i]) {
            return (uint8_t)a->name[i] < (uint8_t)b->name[i];
        }
        if (a->name[i] == 0) {
            break;
        }
    }
    return false;
}

static bool driver_managed(EFI_HANDLE handle, EFI_GUID *protocol) {
    EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *entries;
    EFI_UINTN count;
    if (efi_bs->OpenProtocolInformation(handle, protocol, &entries, &count) != EFI_SUCCESS) {
        return false;
    }
    bool managed = false;
    for (EFI_UINTN i = 0; i < count; i++) {
        if (entries[i].Attributes
            & (EFI_OPEN_PROTOCOL_BY_DRIVER | EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER)) {
            managed = true;
        }
    }
    efi_bs->FreePool(entries);
    return managed;
}

// Firmware set to boot fast may leave the controllers it does not boot from
// unconnected, their drives without Block I/O. Connecting again controllers
// that drivers manage has upset some firmware, so only the others are.
void drives_connect(void) {
    EFI_UINTN count;
    EFI_HANDLE *handles = efi_locate_handles(&pci_io_guid, &count);
    for (EFI_UINTN i = 0; handles != NULL && i < count; i++) {
        EFI_PCI_IO_PROTOCOL *pci_io = protocol_get(handles[i], &pci_io_guid);
        if (pci_io == NULL) {
            continue;
        }
        uint32_t class = pci_class(pci_io);
        if ((class >> 16 == PCI_BASE_CLASS_STORAGE || class >> 8 == PCI_CLASS_SERIAL_USB)
            && !driver_managed(handles[i], &pci_io_guid)) {
            efi_bs->ConnectController(handles[i], NULL, NULL, true);
        }
    }
    if (handles != NULL) {
        efi_bs->FreePool(handles);
    }
}

void drives_collect(void) {
    drive_count = 0;
    EFI_DEVICE_PATH_PROTOCOL *own = own_path();
    EFI_UINTN count;
    EFI_HANDLE *handles = efi_locate_handles(&block_io_guid, &count);
    for (EFI_UINTN i = 0; handles != NULL && i < count; i++) {
        drive_add(handles[i], own);
    }
    if (handles != NULL) {
        efi_bs->FreePool(handles);
    }

    rank_drives();
    for (unsigned i = 1; i < drive_count; i++) {
        struct drive d = drives[i];
        unsigned j = i;
        while (j > 0 && drive_before(&d, &drives[j - 1])) {
            drives[j] = drives[j - 1];
            j--;
        }
        drives[j] = d;
    }

    char *order = loader_handoff->boot_order;
    memset(order, 0, HANDOFF_MAX_BOOT_ORDER);
    for (unsigned i = 0; i < drive_count; i++) {
        const struct drive *d = &drives[i];
        printf("Boot drive %u: %s, %s, %s%s%s, %llu MiB, %s\n", i + 1, d->text,
               d->model[0] != 0 ? d->model : "no model name",
               d->optical ? "optical" : d->external ? "external" : "internal",
               d->bootable ? ", legacy bootable" : "", d->self ? ", ReBIOS's" : "",
               (unsigned long long)(d->size >> 20), d->name);
        size_t used = text_length(order);
        if (used + text_length(d->name) + 2 > HANDOFF_MAX_BOOT_ORDER) {
            printf("The boot order has no room for the rest of the drives\n");
            break;
        }
        append(order, HANDOFF_MAX_BOOT_ORDER, "%s%s", used != 0 ? "\n" : "", d->name);
    }
    if (drive_count == 0) {
        printf("No drives for the legacy BIOS to boot from\n");
    }
}
