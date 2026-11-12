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

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <efi.h>
#include <loader/drives.h>
#include <handoff.h>
#include <hw/pci_regs.h>
#include <lib/log.h>
#include <lib/printf.h>
#include <lib/util.h>
#include <loader/config.h>
#include <loader/efi_glue.h>
#include <loader/loader.h>
#include <memory.h>

#define MAX_DRIVES 32
#define MAX_PATH_NODES 256
#define TEXT_MAX 256
#define NAME_MAX 160
#define MODEL_MAX 64
#define PCI_DEPTH_MAX 8

// The Disk Info protocol, PI 1.10 Vol. V, 7.3.10 to 7.3.13, which the
// firmware's storage drivers install on the drives they find.
#define DISK_INFO_PROTOCOL_GUID \
    { \
        0xd432a67f, 0x14dc, 0x484b, \
        { 0xb3, 0xbb, 0x3f, 0x02, 0x91, 0x84, 0x93, 0x27 } \
    }
#define DISK_INFO_IDE_GUID \
    { \
        0x5e948fe3, 0x26d3, 0x42b5, \
        { 0xaf, 0x17, 0x61, 0x02, 0x87, 0x18, 0x8d, 0xec } \
    }
#define DISK_INFO_AHCI_GUID \
    { \
        0x9e498932, 0x4abc, 0x45af, \
        { 0xa3, 0x4d, 0x02, 0x47, 0x78, 0x7b, 0xe7, 0xc6 } \
    }
#define DISK_INFO_SCSI_GUID \
    { \
        0x08f74baa, 0xea36, 0x41d9, \
        { 0x95, 0x21, 0x21, 0xa7, 0x0f, 0x87, 0x80, 0xbc } \
    }
#define DISK_INFO_USB_GUID \
    { \
        0xcb871572, 0xc11a, 0x47b5, \
        { 0xb4, 0x92, 0x67, 0x5e, 0xaf, 0xa7, 0x77, 0x27 } \
    }

struct disk_info {
    struct efi_guid interface;
    efi_status(EFI_API *inquiry)(struct disk_info *self, void *data, uint32_t *size);
    efi_status(EFI_API *identify)(struct disk_info *self, void *data, uint32_t *size);
    efi_status(EFI_API *sense_data)(
        struct disk_info *self,
        void *data,
        uint32_t *size,
        uint8_t *count
    );
    efi_status(EFI_API *which_ide)(struct disk_info *self, uint32_t *channel, uint32_t *device);
};

// ATA8-ACS T13/1699-D revision 4a, 7.16.7.13: IDENTIFY DEVICE words 27-46, the
// first character of each pair in the word's upper byte.
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

// El Torito 1.0, figure 7: the Boot Record Volume Descriptor, in sector 17.
#define EL_TORITO_SECTOR 17
#define EL_TORITO_ID "EL TORITO SPECIFICATION"

#define MBR_SIGNATURE_OFFSET 510
#define MBR_PARTITIONS 446
#define MBR_PARTITION_SIZE 16
#define MBR_TYPE_OFFSET 4
#define MBR_TYPE_GPT_PROTECTIVE 0xee
#define MBR_STATUS_ACTIVE 0x80

#define ACPI_PNP0A03 0x0a0341d0
#define ACPI_PNP0A08 0x0a0841d0

struct drive {
    char text[TEXT_MAX];
    char name[NAME_MAX];
    char model[MODEL_MAX];
    size_t size;
    bool optical;
    bool external;
    bool bootable;
    bool self;
    unsigned int rank;
};

// The order drives are tried in; those of a rank go by their SeaBIOS names.
#define RANK_CHOSEN 0
#define RANK_SELF 1
#define RANK_INTERNAL_BOOTABLE 2
#define RANK_OPTICAL_BOOTABLE 3
#define RANK_EXTERNAL_BOOTABLE 4
#define RANK_OPTICAL 5
#define RANK_INTERNAL 6
#define RANK_EXTERNAL 7
#define RANK_SELF_LAST 8

static struct efi_guid loaded_image_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
static struct efi_guid device_path_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
static struct efi_guid block_io_guid = EFI_BLOCK_IO_PROTOCOL_GUID;
static struct efi_guid pci_io_guid = EFI_PCI_IO_PROTOCOL_GUID;
static struct efi_guid nvme_guid = EFI_NVM_EXPRESS_PASS_THRU_PROTOCOL_GUID;
static struct efi_guid path_to_text_guid = EFI_DEVICE_PATH_TO_TEXT_PROTOCOL_GUID;
static struct efi_guid disk_info_guid = DISK_INFO_PROTOCOL_GUID;
static struct efi_guid disk_info_ide = DISK_INFO_IDE_GUID;
static struct efi_guid disk_info_ahci = DISK_INFO_AHCI_GUID;
static struct efi_guid disk_info_scsi = DISK_INFO_SCSI_GUID;
static struct efi_guid disk_info_usb = DISK_INFO_USB_GUID;

static struct drive drives[MAX_DRIVES];
static size_t drive_count;

static bool guid_equal(const struct efi_guid *a, const struct efi_guid *b) {
    return memcmp(a, b, sizeof(struct efi_guid)) == 0;
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

static bool root_bridge(struct efi_device_path_protocol *node) {
    if (node->type != EFI_ACPI_DEVICE_PATH) {
        return false;
    }
    if (node->sub_type == EFI_ACPI_DP
     && efi_device_path_node_length(node) >= sizeof(struct efi_acpi_hid_device_path)) {
        uint32_t hid = ((struct efi_acpi_hid_device_path *)node)->hid;
        return hid == ACPI_PNP0A03 || hid == ACPI_PNP0A08;
    }
    if (node->sub_type == EFI_ACPI_EXTENDED_DP
     && efi_device_path_node_length(node)
         >= offsetof(struct efi_acpi_extended_hid_device_path, hid_str)) {
        struct efi_acpi_extended_hid_device_path *acpi;
        acpi = (struct efi_acpi_extended_hid_device_path *)node;
        return acpi->hid == ACPI_PNP0A03 || acpi->hid == ACPI_PNP0A08 || acpi->cid == ACPI_PNP0A03
            || acpi->cid == ACPI_PNP0A08;
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
static bool path_valid(struct efi_device_path_protocol *path) {
    struct efi_device_path_protocol *node = path;
    for (size_t i = 0; i < MAX_PATH_NODES; i++) {
        if (efi_device_path_node_length(node) < sizeof(struct efi_device_path_protocol)) {
            return false;
        }
        if (efi_is_device_path_end_type(node)) {
            return true;
        }
        node = efi_next_device_path_node(node);
    }
    return false;
}

// The bytes of the path's nodes, without its end node.
static size_t path_length(struct efi_device_path_protocol *path) {
    size_t length = 0;
    for (struct efi_device_path_protocol *node = path; !efi_is_device_path_end_type(node);
         node = efi_next_device_path_node(node)) {
        length += efi_device_path_node_length(node);
    }
    return length;
}

static bool path_starts_with(
    struct efi_device_path_protocol *path,
    struct efi_device_path_protocol *prefix
) {
    size_t length = path_length(prefix);
    return path_length(path) >= length && memcmp(path, prefix, length) == 0;
}

static void path_text(struct efi_device_path_protocol *path, char *out, size_t size) {
    static struct efi_device_path_to_text_protocol *to_text;
    if (to_text == NULL
     && efi_bs->locate_protocol(&path_to_text_guid, NULL, (void **)&to_text) != EFI_SUCCESS) {
        to_text = NULL;
    }
    out[0] = 0;
    uint16_t *text = to_text != NULL
                   ? to_text->convert_device_path_to_text(path, false, false)
                   : NULL;
    if (text == NULL) {
        // Without the firmware's rendering, its nodes' types and subtypes.
        for (struct efi_device_path_protocol *node = path; !efi_is_device_path_end_type(node);
             node = efi_next_device_path_node(node)) {
            append(out, size, "%sPath(%u,%u)", out[0] != 0 ? "/" : "", node->type, node->sub_type);
        }
        return;
    }
    size_t n = 0;
    for (size_t i = 0; text[i] != 0 && n + 1 < size; i++) {
        out[n++] = text[i] >= 0x20 && text[i] < 0x7f ? (char)text[i] : '?';
    }
    out[n] = 0;
    efi_bs->free_pool(text);
}

// The location of the PCI function at the end of the path's first nodes.
static bool pci_location(
    struct efi_device_path_protocol *path,
    size_t length,
    struct efi_pci_io_protocol **pci_io,
    size_t *segment,
    size_t *bus
) {
    uint8_t buffer[512];
    if (length + EFI_END_DEVICE_PATH_LENGTH > sizeof(buffer)) {
        return false;
    }
    memcpy(buffer, path, length);
    efi_set_device_path_end_node((struct efi_device_path_protocol *)(buffer + length));
    struct efi_device_path_protocol *remaining = (struct efi_device_path_protocol *)buffer;
    efi_handle handle;
    if (efi_bs->locate_device_path(&pci_io_guid, &remaining, &handle) != EFI_SUCCESS
     || !efi_is_device_path_end_type(remaining)) {
        return false;
    }
    size_t device;
    size_t function;
    return efi_bs->handle_protocol(handle, &pci_io_guid, (void **)pci_io) == EFI_SUCCESS
        && *pci_io != NULL
        && (*pci_io)->get_location(*pci_io, segment, bus, &device, &function) == EFI_SUCCESS;
}

static uint32_t pci_class(struct efi_pci_io_protocol *pci_io) {
    uint32_t value = 0;
    if (pci_io->pci.read(pci_io, EFI_PCI_IO_WIDTH_UINT32, 0x08, 1, &value) != EFI_SUCCESS) {
        return 0;
    }
    return value >> 8;
}

// The drive's name as SeaBIOS's bootorder file has it (SeaBIOS src/boot.c,
// build_pci_path() and the bootprio_find_*() functions), or false where
// SeaBIOS could not boot it.
static bool seabios_name(struct efi_device_path_protocol *path, char *out, size_t size) {
    struct efi_pci_device_path *pci[PCI_DEPTH_MAX];
    size_t depth = 0;
    size_t first_pci_end = 0;
    size_t offset = 0;
    struct efi_device_path_protocol *node = path;
    for (; !efi_is_device_path_end_type(node); node = efi_next_device_path_node(node)) {
        if (node->type == EFI_ACPI_DEVICE_PATH && depth == 0) {
            if (!root_bridge(node)) {
                return false;
            }
        } else if (node->type == EFI_HARDWARE_DEVICE_PATH && node->sub_type == EFI_HW_PCI_DP
                && efi_device_path_node_length(node) >= sizeof(struct efi_pci_device_path)) {
            if (depth == PCI_DEPTH_MAX) {
                return false;
            }
            pci[depth++] = (struct efi_pci_device_path *)node;
            if (depth == 1) {
                first_pci_end = offset + efi_device_path_node_length(node);
            }
        } else {
            break;
        }
        offset += efi_device_path_node_length(node);
    }
    if (depth == 0) {
        return false;
    }

    struct efi_pci_io_protocol *root_io;
    size_t segment;
    size_t root_bus;
    struct efi_pci_io_protocol *controller_io;
    size_t controller_segment;
    size_t controller_bus;
    bool unlocated = !pci_location(path, first_pci_end, &root_io, &segment, &root_bus)
                  || !pci_location(
                         path, offset, &controller_io, &controller_segment, &controller_bus
                     );
    if (unlocated) {
        return false;
    }

    bool usb = node->type == EFI_MESSAGING_DEVICE_PATH && node->sub_type == EFI_MSG_USB_DP;
    out[0] = 0;
    append(out, size, "/pci@i0cf8");
    // The core has the legacy BIOS find another root bus's devices behind the
    // port it gives the root bus, and puts that port in the name instead.
    if (segment != 0 || root_bus != 0) {
        append(out, size, ",%x:%x", (unsigned int)segment, (unsigned int)root_bus);
    }
    for (size_t i = 0; i < depth; i++) {
        const char *kind = i + 1 < depth ? "pci-bridge" : usb ? "usb" : "*";
        append(out, size, "/%s@%x", kind, pci[i]->device);
        if (pci[i]->function != 0) {
            append(out, size, ",%x", pci[i]->function);
        }
    }

    if (efi_is_device_path_end_type(node)) {
        return true;
    }
    if (node->type != EFI_MESSAGING_DEVICE_PATH) {
        return false;
    }
    switch (node->sub_type) {
        case EFI_MSG_SATA_DP: {
            struct efi_sata_device_path *sata = (struct efi_sata_device_path *)node;
            // SeaBIOS drives no port multipliers.
            if (sata->port_multiplier_port_number != 0xffff) {
                return false;
            }
            append(out, size, "/drive@%x/disk@0", sata->hba_port_number);
            node = efi_next_device_path_node(node);
            break;
        }
        case EFI_MSG_ATAPI_DP: {
            struct efi_atapi_device_path *ata = (struct efi_atapi_device_path *)node;
            append(out, size, "/drive@%x/disk@%x", ata->primary_secondary, ata->slave_master);
            node = efi_next_device_path_node(node);
            break;
        }
        case EFI_MSG_SCSI_DP: {
            struct efi_scsi_device_path *scsi = (struct efi_scsi_device_path *)node;
            append(out, size, "/*@0/*@%x,%x", scsi->pun, scsi->lun);
            node = efi_next_device_path_node(node);
            break;
        }
        // SeaBIOS orders these by their controllers alone.
        case EFI_MSG_NVME_NAMESPACE_DP:
        case EFI_MSG_SD_DP:
        case EFI_MSG_EMMC_DP: {
            node = efi_next_device_path_node(node);
            break;
        }
        // The hypervisor's USB stack gives the same names (virt/usb_disk.c).
        case EFI_MSG_USB_DP: {
            while (node->type == EFI_MESSAGING_DEVICE_PATH && node->sub_type == EFI_MSG_USB_DP) {
                uint32_t port = ((struct efi_usb_device_path *)node)->parent_port_number + 1;
                node = efi_next_device_path_node(node);
                bool last = node->type != EFI_MESSAGING_DEVICE_PATH
                         || node->sub_type != EFI_MSG_USB_DP;
                append(out, size, last ? "/storage@%x" : "/hub@%x", port);
            }
            uint32_t lun = 0;
            if (node->type == EFI_MESSAGING_DEVICE_PATH
             && node->sub_type == EFI_MSG_DEVICE_LOGICAL_UNIT_DP) {
                lun = ((struct efi_device_logical_unit_device_path *)node)->lun;
                node = efi_next_device_path_node(node);
            }
            append(out, size, "/*@0/*@0,%x", lun);
            break;
        }
        default: {
            return false;
        }
    }
    return efi_is_device_path_end_type(node) && text_length(out) + 1 < size;
}

static void ata_model(struct disk_info *info, char *out, size_t size) {
    uint8_t identify[512];
    uint32_t length = sizeof(identify);
    if (info->identify(info, identify, &length) != EFI_SUCCESS
     || length < ATA_MODEL_OFFSET + ATA_MODEL_LENGTH) {
        return;
    }
    uint8_t model[ATA_MODEL_LENGTH];
    for (size_t i = 0; i < ATA_MODEL_LENGTH; i += 2) {
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

static void nvme_model(struct efi_device_path_protocol *path, char *out, size_t size) {
    struct efi_device_path_protocol *remaining = path;
    efi_handle handle;
    if (efi_bs->locate_device_path(&nvme_guid, &remaining, &handle) != EFI_SUCCESS) {
        return;
    }
    struct efi_nvm_express_pass_thru_protocol *nvme;
    if (efi_bs->handle_protocol(handle, &nvme_guid, (void **)&nvme) != EFI_SUCCESS
     || nvme == NULL) {
        return;
    }
    uintptr_t buffer = efi_glue_alloc_pages(1, 0);
    if (buffer == 0) {
        return;
    }
    struct efi_nvm_express_command command;
    struct efi_nvm_express_completion completion;
    struct efi_nvm_express_pass_thru_command_packet packet;
    memset(&command, 0, sizeof(struct efi_nvm_express_command));
    memset(&completion, 0, sizeof(struct efi_nvm_express_completion));
    memset(&packet, 0, sizeof(struct efi_nvm_express_pass_thru_command_packet));
    command.cdw0.cdw0 = (uint32_t)NVME_ADMIN_IDENTIFY << EFI_NVME_CDW0_OP_CODE_SHIFT;
    command.cdw10 = NVME_CNS_CONTROLLER;
    command.flags = EFI_CDW10_VALID;
    packet.command_timeout = NVME_TIMEOUT_100NS;
    packet.transfer_buffer = (void *)(uintptr_t)buffer;
    packet.transfer_length = NVME_IDENTIFY_SIZE;
    packet.nvme_cmd = &command;
    packet.nvme_completion = &completion;
    if (nvme->pass_thru(nvme, 0, &packet, NULL) == EFI_SUCCESS) {
        copy_text(out, size, (uint8_t *)(uintptr_t)buffer + NVME_MODEL_OFFSET, NVME_MODEL_LENGTH);
    }
    efi_bs->free_pages(buffer, 1);
}

static void drive_model(
    efi_handle handle,
    struct efi_device_path_protocol *path,
    char *out,
    size_t size
) {
    out[0] = 0;
    struct disk_info *info;
    if (efi_bs->handle_protocol(handle, &disk_info_guid, (void **)&info) != EFI_SUCCESS) {
        info = NULL;
    }
    if (info != NULL
     && (guid_equal(&info->interface, &disk_info_ide)
      || guid_equal(&info->interface, &disk_info_ahci))) {
        ata_model(info, out, size);
    } else if (info != NULL
            && (guid_equal(&info->interface, &disk_info_scsi)
             || guid_equal(&info->interface, &disk_info_usb))) {
        scsi_model(info, out, size);
    }
    if (out[0] == 0) {
        nvme_model(path, out, size);
    }
}

// Whether the medium holds what the legacy BIOS boots: a master boot record
// with code, or an El Torito boot record. GPT's protective MBR is no such
// record, but a hybrid one is where it also marks an MBR partition active for
// a legacy OS, as boot managers such as BootIt UEFI write.
static bool legacy_bootable(struct efi_block_io_protocol *block_io, bool optical) {
    struct efi_block_io_media *media = block_io->media;
    if (media->media_present == 0 || media->block_size < 512 || media->block_size > 4096) {
        return false;
    }
    // A transfer's buffer starts at a multiple of IoAlign, a power of two
    // where it is above 1 (UEFI 2.11, 13.9): pages start at a multiple of 4
    // KiB, and twice the pages of a larger alignment hold one that starts at a
    // multiple of it.
    size_t align = EFI_PAGE_SIZE;
    if (media->io_align > EFI_PAGE_SIZE && (media->io_align & (media->io_align - 1)) == 0) {
        align = media->io_align;
    }
    size_t pages = 2 * EFI_SIZE_TO_PAGES(align);
    uintptr_t buffer = efi_glue_alloc_pages(pages, 0);
    if (buffer == 0) {
        return false;
    }
    uint8_t *sector = (uint8_t *)(uintptr_t)align_up(buffer, align);
    bool bootable = false;
    if (optical) {
        efi_status status = block_io->read_blocks(
            block_io, media->media_id, EL_TORITO_SECTOR, media->block_size, sector
        );
        if (status == EFI_SUCCESS) {
            bootable = sector[0] == 0 && memcmp(sector + 1, "CD001", 5) == 0 && sector[6] == 1
                    && memcmp(sector + 7, EL_TORITO_ID, sizeof(EL_TORITO_ID) - 1) == 0;
        }
    } else {
        efi_status status = block_io->read_blocks(
            block_io, media->media_id, 0, media->block_size, sector
        );
        if (status == EFI_SUCCESS) {
            bool protective = false;
            bool active = false;
            for (size_t i = 0; i < 4; i++) {
                const uint8_t *entry = sector + MBR_PARTITIONS + i * MBR_PARTITION_SIZE;
                uint8_t type = entry[MBR_TYPE_OFFSET];
                if (type == MBR_TYPE_GPT_PROTECTIVE) {
                    protective = true;
                } else if (type != 0 && entry[0] == MBR_STATUS_ACTIVE) {
                    active = true;
                }
            }
            bool code = false;
            for (size_t i = 0; i < MBR_PARTITIONS; i++) {
                code |= sector[i] != 0;
            }
            bootable = sector[MBR_SIGNATURE_OFFSET] == 0x55
                    && sector[MBR_SIGNATURE_OFFSET + 1] == 0xaa && code && (!protective || active);
        }
    }
    efi_bs->free_pages(buffer, pages);
    return bootable;
}

static bool path_has_usb(struct efi_device_path_protocol *path) {
    for (struct efi_device_path_protocol *node = path; !efi_is_device_path_end_type(node);
         node = efi_next_device_path_node(node)) {
        if (node->type == EFI_MESSAGING_DEVICE_PATH && node->sub_type == EFI_MSG_USB_DP) {
            return true;
        }
    }
    return false;
}

static struct efi_device_path_protocol *own_path(void) {
    struct efi_loaded_image_protocol *image;
    efi_status status = efi_bs->handle_protocol(
        efi_image_handle, &loaded_image_guid, (void **)&image
    );
    if (status != EFI_SUCCESS || image == NULL || image->device_handle == NULL) {
        return NULL;
    }
    struct efi_device_path_protocol *path;
    status = efi_bs->handle_protocol(image->device_handle, &device_path_guid, (void **)&path);
    return status == EFI_SUCCESS && path != NULL && path_valid(path) ? path : NULL;
}

static void drive_add(efi_handle handle, struct efi_device_path_protocol *own) {
    struct efi_block_io_protocol *block_io;
    struct efi_device_path_protocol *path;
    if (efi_bs->handle_protocol(handle, &block_io_guid, (void **)&block_io) != EFI_SUCCESS
     || block_io == NULL || block_io->media == NULL || block_io->media->logical_partition != 0
     || efi_bs->handle_protocol(handle, &device_path_guid, (void **)&path) != EFI_SUCCESS
     || path == NULL || !path_valid(path)) {
        return;
    }
    struct drive d;
    memset(&d, 0, sizeof(struct drive));
    path_text(path, d.text, sizeof(d.text));
    if (!seabios_name(path, d.name, sizeof(d.name))) {
        printf("Drive %s: not one the legacy BIOS can boot\n", d.text);
        return;
    }
    for (size_t i = 0; i < drive_count; i++) {
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
    d.optical = block_io->media->block_size == 2048;
    d.external = path_has_usb(path) || (!d.optical && block_io->media->removable_media != 0);
    d.size = block_io->media->media_present != 0
           ? (block_io->media->last_block + 1) * block_io->media->block_size
           : 0;
    d.self = own != NULL && path_starts_with(own, path);
    drive_model(handle, path, d.model, sizeof(d.model));
    d.bootable = legacy_bootable(block_io, d.optical);
    drives[drive_count++] = d;
}

// A device path matches whatever the drive's path starts with, case and spaces
// aside, as a path with its partition does.
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
        while (k < length && model[start + k] != 0
            && lower(model[start + k]) == lower(wanted[k])) {
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
    for (size_t i = 0; i < drive_count; i++) {
        struct drive *d = &drives[i];
        if (named && !chosen
         && (wanted_is_path(wanted)
           ? path_matches(wanted, d->text)
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
        for (size_t i = 0; i < drive_count; i++) {
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

static bool driver_managed(efi_handle handle, struct efi_guid *protocol) {
    struct efi_open_protocol_information_entry *entries;
    size_t count;
    if (efi_bs->open_protocol_information(handle, protocol, &entries, &count) != EFI_SUCCESS) {
        return false;
    }
    bool managed = false;
    uint32_t by_driver = EFI_OPEN_PROTOCOL_BY_DRIVER | EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER;
    for (size_t i = 0; i < count; i++) {
        if ((entries[i].attributes & by_driver) != 0) {
            managed = true;
        }
    }
    efi_bs->free_pool(entries);
    return managed;
}

// Firmware set to boot fast may leave the controllers it does not boot from
// unconnected, their drives without Block I/O. Connecting again controllers
// that drivers manage has upset some firmware, so only the others are.
void drives_connect(void) {
    size_t count;
    efi_handle *handles = efi_glue_locate_handles(&pci_io_guid, &count);
    for (size_t i = 0; handles != NULL && i < count; i++) {
        struct efi_pci_io_protocol *pci_io;
        if (efi_bs->handle_protocol(handles[i], &pci_io_guid, (void **)&pci_io) != EFI_SUCCESS
         || pci_io == NULL) {
            continue;
        }
        uint32_t class = pci_class(pci_io);
        if ((class >> 16 == PCI_BASE_CLASS_STORAGE || class >> 8 == PCI_CLASS_SERIAL_USB)
         && !driver_managed(handles[i], &pci_io_guid)) {
            efi_bs->connect_controller(handles[i], NULL, NULL, true);
        }
    }
    if (handles != NULL) {
        efi_bs->free_pool(handles);
    }
}

void drives_collect(void) {
    drive_count = 0;
    struct efi_device_path_protocol *own = own_path();
    size_t count;
    efi_handle *handles = efi_glue_locate_handles(&block_io_guid, &count);
    for (size_t i = 0; handles != NULL && i < count; i++) {
        drive_add(handles[i], own);
    }
    if (handles != NULL) {
        efi_bs->free_pool(handles);
    }

    rank_drives();
    for (size_t i = 1; i < drive_count; i++) {
        struct drive d = drives[i];
        size_t j = i;
        while (j > 0 && drive_before(&d, &drives[j - 1])) {
            drives[j] = drives[j - 1];
            j--;
        }
        drives[j] = d;
    }

    char *order = loader_handoff->boot_order;
    memset(order, 0, HANDOFF_MAX_BOOT_ORDER);
    for (size_t i = 0; i < drive_count; i++) {
        const struct drive *d = &drives[i];
        printf(
            "Boot drive %zu: %s, %s, %s%s%s, %llu MiB, %s\n", i + 1, d->text,
            d->model[0] != 0 ? d->model : "no model name",
            d->optical ? "optical" : d->external ? "external" : "internal",
            d->bootable ? ", legacy bootable" : "", d->self ? ", ReBIOS's" : "",
            (unsigned long long)(d->size >> 20), d->name
        );
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
