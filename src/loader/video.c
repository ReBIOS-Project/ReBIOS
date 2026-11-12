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
#include <loader/video.h>
#include <handoff.h>
#include <hw/pci_regs.h>
#include <lib/log.h>
#include <lib/printf.h>
#include <lib/util.h>
#include <loader/cbfs.h>
#include <loader/config.h>
#include <loader/display.h>
#include <loader/efi_glue.h>
#include <loader/fwtables.h>
#include <loader/gpu_lists.h>
#include <loader/loader.h>
#include <loader/panel.h>
#include <loader/pci_io.h>
#include <loader/pci_rom.h>
#include <memory.h>

#define VBIOS_FILE_MAX ((uint32_t)256 * 1024)
#define CBFS_SETTING "cbfs"
#define CBFS_SETTING_PREFIX "cbfs:"
#define CBFS_DEFAULT_NAME "vgabios.bin"

// SMBIOS DSP0134 3.9.0, Tables 16 and 90.
#define SMBIOS_CHASSIS 3
#define SMBIOS_CHASSIS_TYPE 0x05
#define SMBIOS_CHASSIS_TYPE_MASK 0x7f
#define SMBIOS_PORTABLE_BATTERY 22

// ACPI 6.6, 5.2.9.
#define FADT_PROFILE_MOBILE 2
#define FADT_PROFILE_APPLIANCE 6
#define FADT_PROFILE_TABLET 8

// What a stopped graphics driver may turn off, and the VBIOS needs.
#define VGA_DECODE_ATTRIBUTES \
    (EFI_PCI_IO_ATTRIBUTE_IO | EFI_PCI_IO_ATTRIBUTE_MEMORY | EFI_PCI_IO_ATTRIBUTE_BUS_MASTER \
   | EFI_PCI_IO_ATTRIBUTE_VGA_MEMORY | EFI_PCI_IO_ATTRIBUTE_VGA_IO \
   | EFI_PCI_IO_ATTRIBUTE_VGA_IO_16)

#define VGA_ATTRIBUTES \
    (EFI_PCI_IO_ATTRIBUTE_VGA_MEMORY | EFI_PCI_IO_ATTRIBUTE_VGA_IO \
   | EFI_PCI_IO_ATTRIBUTE_VGA_IO_16)

static struct efi_guid gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
static struct efi_guid device_path_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
static struct efi_guid loaded_image_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
static struct efi_guid file_system_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
static struct efi_guid file_info_guid = EFI_FILE_INFO_ID;

// SMBIOS DSP0134 3.9.0, Table 17: Portable, Laptop, Notebook, Hand Held, All
// in One, Sub Notebook, Tablet, Convertible, Detachable, IoT Gateway, Embedded
// PC, Mini PC, and Stick PC.
static const uint8_t slotless_chassis[] = {
    0x08, 0x09, 0x0a, 0x0b, 0x0d, 0x0e, 0x1e, 0x1f, 0x20, 0x21, 0x22, 0x23, 0x24
};

// A VBIOS image the configuration names.
struct user_vbios {
    const void *image;
    size_t size;
    // Pool memory holding the image, if it was read from a file.
    void *buffer;
    char source[CONFIG_VGABIOS_MAX + 16];
};

struct vbios_choice {
    struct pci_io_device dev;
    const void *image;
    size_t size;
    const char *source;
};

static bool setting_starts_with(const uint16_t *setting, const char *prefix) {
    for (size_t i = 0; prefix[i] != 0; i++) {
        if (setting[i] != (uint8_t)prefix[i]) {
            return false;
        }
    }
    return true;
}

static void setting_to_ascii(const uint16_t *setting, char *out, size_t out_size) {
    size_t i = 0;
    for (; i + 1 < out_size && setting[i] != 0; i++) {
        out[i] = setting[i] < 0x80 ? (char)setting[i] : '?';
    }
    out[i] = 0;
}

static bool gop_mode_usable(struct efi_graphics_output_protocol *gop) {
    struct efi_graphics_output_protocol_mode *mode = gop->mode;
    if (mode == NULL || mode->info == NULL || mode->frame_buffer_base == 0) {
        return false;
    }

    struct efi_graphics_output_mode_information *info = mode->info;
    struct efi_pixel_bitmask *masks = &info->pixel_information;
    uint32_t bits = masks->red_mask | masks->green_mask | masks->blue_mask | masks->reserved_mask;
    bool format_usable = info->pixel_format == EFI_PIXEL_RED_GREEN_BLUE_RESERVED_8_BIT_PER_COLOR
                      || info->pixel_format == EFI_PIXEL_BLUE_GREEN_RED_RESERVED_8_BIT_PER_COLOR
                      || (info->pixel_format == EFI_PIXEL_BIT_MASK && bits != 0);

    return format_usable && info->horizontal_resolution != 0 && info->vertical_resolution != 0
        && info->pixels_per_scan_line >= info->horizontal_resolution;
}

// Leaves the graphics output in a usable mode if it has one, preferring the
// current mode so as not to disturb the display.
static bool gop_prepare(struct efi_graphics_output_protocol *gop) {
    struct efi_graphics_output_mode_information *info = NULL;
    size_t info_size;

    if (gop->mode == NULL) {
        return false;
    }

    // Some firmware leaves the protocol unstarted until a mode is set.
    efi_status status = gop->query_mode(gop, gop->mode->mode, &info_size, &info);
    if (status == EFI_NOT_STARTED) {
        gop->set_mode(gop, 0);
        status = gop->query_mode(gop, gop->mode->mode, &info_size, &info);
    }
    // A GPU with no display attached can give a current mode of 0x0, and its
    // firmware can hang when set to another: there is nothing to draw on.
    bool blank = status == EFI_SUCCESS && info != NULL
              && (info->horizontal_resolution == 0 || info->vertical_resolution == 0);
    // The buffer is the caller's, unless the firmware handed out its own.
    if (status == EFI_SUCCESS && info != NULL && info != gop->mode->info) {
        efi_bs->free_pool(info);
    }
    if (blank) {
        return false;
    }

    if (gop_mode_usable(gop)) {
        return true;
    }
    uint32_t original = gop->mode->mode;
    for (size_t mode = 0; mode < gop->mode->max_mode; mode++) {
        if (gop->set_mode(gop, (uint32_t)mode) == EFI_SUCCESS && gop_mode_usable(gop)) {
            return true;
        }
    }
    if (original < gop->mode->max_mode) {
        gop->set_mode(gop, original);
    }
    return false;
}

static bool gop_pci_device(efi_handle handle, struct pci_io_device *dev) {
    struct efi_device_path_protocol *path;
    if (efi_bs->handle_protocol(handle, &device_path_guid, (void **)&path) != EFI_SUCCESS
     || path == NULL) {
        return false;
    }
    return pci_io_device_on_path(path, dev);
}

static bool gop_of_configured_device(efi_handle handle) {
    struct pci_io_device dev;
    return config.vga_specified && gop_pci_device(handle, &dev)
        && handoff_pci_address_equal(dev.address, config.vga_device);
}

// Returns the first usable graphics output, preferring those of the configured
// VGA device, or NULL if there is none.
static efi_handle choose_gop(struct efi_graphics_output_protocol **chosen) {
    size_t count;
    efi_handle *handles = efi_glue_locate_handles(&gop_guid, &count);
    efi_handle found = NULL;

    for (size_t pass = 0; pass < 2 && found == NULL; pass++) {
        for (size_t i = 0; i < count && found == NULL; i++) {
            struct efi_graphics_output_protocol *gop;
            if (gop_of_configured_device(handles[i]) != (pass == 0)) {
                continue;
            }
            if (efi_bs->handle_protocol(handles[i], &gop_guid, (void **)&gop) == EFI_SUCCESS
             && gop != NULL && gop_prepare(gop)) {
                found = handles[i];
                *chosen = gop;
            }
        }
    }

    if (handles != NULL) {
        efi_bs->free_pool(handles);
    }
    return found;
}

static uint8_t mask_shift(uint32_t mask) {
    return mask == 0 ? 0 : (uint8_t)__builtin_ctz(mask);
}

static size_t mask_size(uint32_t mask) {
    return (uint8_t)__builtin_popcount(mask);
}

// Takes the current mode from Mode->Info rather than from QueryMode(), as some
// firmware reports a wrong PixelsPerScanLine through the latter.
static void describe_framebuffer(
    struct efi_graphics_output_protocol *gop,
    struct handoff_framebuffer *fb
) {
    struct efi_graphics_output_protocol_mode *mode = gop->mode;
    struct efi_graphics_output_mode_information *info = mode->info;
    struct efi_pixel_bitmask masks = info->pixel_information;

    if (info->pixel_format == EFI_PIXEL_RED_GREEN_BLUE_RESERVED_8_BIT_PER_COLOR) {
        masks = (struct efi_pixel_bitmask){ 0x000000ff, 0x0000ff00, 0x00ff0000, 0xff000000 };
    } else if (info->pixel_format == EFI_PIXEL_BLUE_GREEN_RED_RESERVED_8_BIT_PER_COLOR) {
        masks = (struct efi_pixel_bitmask){ 0x00ff0000, 0x0000ff00, 0x000000ff, 0xff000000 };
    }

    uint32_t all = masks.red_mask | masks.green_mask | masks.blue_mask | masks.reserved_mask;
    uint32_t bpp = (32 - (uint32_t)__builtin_clz(all) + 7) / 8 * 8;

    fb->base = mode->frame_buffer_base;
    fb->size = mode->frame_buffer_size;
    fb->width = info->horizontal_resolution;
    fb->height = info->vertical_resolution;
    fb->pitch = info->pixels_per_scan_line * (bpp / 8);
    fb->bpp = (uint8_t)bpp;
    fb->red_size = mask_size(masks.red_mask);
    fb->red_shift = mask_shift(masks.red_mask);
    fb->green_size = mask_size(masks.green_mask);
    fb->green_shift = mask_shift(masks.green_mask);
    fb->blue_size = mask_size(masks.blue_mask);
    fb->blue_shift = mask_shift(masks.blue_mask);
    fb->reserved_size = mask_size(masks.reserved_mask);
    fb->reserved_shift = mask_shift(masks.reserved_mask);
    fb->present = true;

    printf(
        "Framebuffer at %#llx, %ux%u, %u bpp, pitch %u\n", (unsigned long long)fb->base, fb->width,
        fb->height, bpp, fb->pitch
    );
}

static bool load_cbfs_vbios(struct user_vbios *vbios) {
    const uint16_t *setting = config.vgabios;
    char name[CONFIG_VGABIOS_MAX];
    size_t prefix = sizeof(CBFS_SETTING_PREFIX) - 1;

    if (setting[sizeof(CBFS_SETTING) - 1] == 0) {
        snprintf(name, sizeof(name), "%s", CBFS_DEFAULT_NAME);
    } else {
        size_t i = 0;
        for (; prefix + i < CONFIG_VGABIOS_MAX && setting[prefix + i] != 0; i++) {
            if (setting[prefix + i] >= 0x80) {
                printf("The VBIOS CBFS file name is not ASCII\n");
                return false;
            }
            name[i] = (char)setting[prefix + i];
        }
        name[i] = 0;
        if (i == 0) {
            printf("The VBIOS CBFS file name is empty\n");
            return false;
        }
    }

    if (!cbfs_find_file(name, &vbios->image, &vbios->size)) {
        printf("Cannot find a usable CBFS file %s for the VBIOS\n", name);
        return false;
    }
    snprintf(vbios->source, sizeof(vbios->source), "the CBFS file %s", name);
    return true;
}

static bool file_size(struct efi_file_protocol *file, uint64_t *size) {
    size_t info_size = 0;
    struct efi_file_info *info;
    bool ok = false;

    if (file->get_info(file, &file_info_guid, &info_size, NULL) != EFI_BUFFER_TOO_SMALL
     || efi_bs->allocate_pool(EFI_LOADER_DATA, info_size, (void **)&info) != EFI_SUCCESS) {
        return false;
    }
    if (file->get_info(file, &file_info_guid, &info_size, info) == EFI_SUCCESS
     && (info->attribute & EFI_FILE_DIRECTORY) == 0) {
        *size = info->file_size;
        ok = true;
    }
    efi_bs->free_pool(info);
    return ok;
}

// Returns the contents in pool memory, or NULL.
static void *read_file(struct efi_file_protocol *file, size_t size) {
    uint8_t *buffer;

    if (efi_bs->allocate_pool(EFI_LOADER_DATA, size, (void **)&buffer) != EFI_SUCCESS) {
        return NULL;
    }
    for (size_t done = 0; done < size; ) {
        size_t chunk = size - done;
        if (file->read(file, &chunk, buffer + done) != EFI_SUCCESS || chunk == 0
         || chunk > size - done) {
            efi_bs->free_pool(buffer);
            return NULL;
        }
        done += (uint32_t)chunk;
    }
    return buffer;
}

static bool load_file_vbios(struct user_vbios *vbios) {
    struct efi_loaded_image_protocol *loaded_image;
    struct efi_simple_file_system_protocol *fs;
    struct efi_file_protocol *root;
    struct efi_file_protocol *file;
    uint16_t path[CONFIG_VGABIOS_MAX];
    char name[CONFIG_VGABIOS_MAX];
    size_t size = 0;

    // FAT names cannot hold forward slashes, so these can only be separators.
    size_t i = 0;
    for (; i + 1 < CONFIG_VGABIOS_MAX && config.vgabios[i] != 0; i++) {
        path[i] = config.vgabios[i] == u'/' ? u'\\' : config.vgabios[i];
    }
    path[i] = 0;
    setting_to_ascii(path, name, sizeof(name));

    efi_status status = efi_bs->handle_protocol(
        efi_image_handle, &loaded_image_guid, (void **)&loaded_image
    );
    if (status == EFI_SUCCESS) {
        status = efi_bs->handle_protocol(
            loaded_image->device_handle, &file_system_guid, (void **)&fs
        );
    }
    if (status == EFI_SUCCESS) {
        status = fs->open_volume(fs, &root);
    }
    if (status != EFI_SUCCESS) {
        printf("Cannot open the volume ReBIOS was loaded from to read the VBIOS file %s\n", name);
        return false;
    }

    if (root->open(root, &file, path, EFI_FILE_MODE_READ, 0) != EFI_SUCCESS) {
        printf("Cannot open the VBIOS file %s\n", name);
    } else {
        if (!file_size(file, &size)) {
            printf("Cannot read the VBIOS file %s\n", name);
        } else if (size == 0 || size > VBIOS_FILE_MAX) {
            printf(
                "The VBIOS file %s is empty or larger than %u KiB\n", name, VBIOS_FILE_MAX / 1024
            );
        } else if ((vbios->buffer = read_file(file, (uint32_t)size)) == NULL) {
            printf("Cannot read the VBIOS file %s\n", name);
        }
        file->close(file);
    }
    root->close(root);

    if (vbios->buffer == NULL) {
        return false;
    }
    vbios->image = vbios->buffer;
    vbios->size = (uint32_t)size;
    snprintf(vbios->source, sizeof(vbios->source), "the file %s", name);
    return true;
}

// Failures leave the setting to be treated as empty.
static bool load_user_vbios(struct user_vbios *vbios) {
    const uint16_t *setting = config.vgabios;
    bool loaded;

    memset(vbios, 0, sizeof(struct user_vbios));
    if (setting[0] == 0) {
        return false;
    }

    if ((setting_starts_with(setting, CBFS_SETTING) && setting[sizeof(CBFS_SETTING) - 1] == 0)
     || setting_starts_with(setting, CBFS_SETTING_PREFIX)) {
        loaded = load_cbfs_vbios(vbios);
    } else {
        loaded = load_file_vbios(vbios);
    }
    if (!loaded) {
        return false;
    }

    const uint8_t *image = vbios->image;
    if (vbios->size < 2 || image[0] != 0x55 || image[1] != 0xaa) {
        printf("Not using %s as the VBIOS, as it lacks the ROM signature\n", vbios->source);
        if (vbios->buffer != NULL) {
            efi_bs->free_pool(vbios->buffer);
            vbios->buffer = NULL;
        }
        return false;
    }
    return true;
}

static bool gpu_listed(
    const struct gpu_lists_id *list,
    size_t count,
    const struct pci_io_device *dev
) {
    for (size_t i = 0; i < count; i++) {
        const struct gpu_lists_id *id = &list[i];
        if (id->vendor == dev->vendor_id && id->first <= dev->device_id
         && id->last >= dev->device_id
         && (id->revision == GPU_LISTS_ANY_REVISION || id->revision == dev->revision)) {
            return true;
        }
    }
    return false;
}

// For GPUs the lists do not know: firmware can say a slot is behind the port
// to a GPU soldered to the board, so all it says of the machine has to agree.
static bool slotted_card(const struct pci_io_device *dev) {
    uint8_t value = 0;
    if (fwtables_smbios_byte(SMBIOS_PORTABLE_BATTERY, 0, &value)) {
        return false;
    }
    value = 0;
    fwtables_fadt_pm_profile(&value);
    if (value == FADT_PROFILE_MOBILE || value == FADT_PROFILE_APPLIANCE
     || value == FADT_PROFILE_TABLET) {
        return false;
    }
    value = 0;
    fwtables_smbios_byte(SMBIOS_CHASSIS, SMBIOS_CHASSIS_TYPE, &value);
    value &= SMBIOS_CHASSIS_TYPE_MASK;
    for (size_t i = 0; i < ARRAY_COUNT(slotless_chassis); i++) {
        if (value == slotless_chassis[i]) {
            return false;
        }
    }
    return pci_io_device_in_slot(dev);
}

static bool vbios_trusted(const struct pci_io_device *dev) {
    if (gpu_listed(gpu_lists_vbios_blocklist, gpu_lists_vbios_blocklist_count, dev)) {
        return false;
    }
    return gpu_listed(gpu_lists_vbios_allowlist, gpu_lists_vbios_allowlist_count, dev)
        || slotted_card(dev);
}

// The aliased variant of the I/O decoding is preferred, as legacy software
// expects ISA-style decoding.
static bool enable_vga_decoding(struct efi_pci_io_protocol *io) {
    uint64_t supported;
    uint64_t vga_io;

    if (io->attributes(io, EFI_PCI_IO_ATTRIBUTE_OPERATION_SUPPORTED, 0, &supported) != EFI_SUCCESS
     || (supported & EFI_PCI_IO_ATTRIBUTE_VGA_MEMORY) == 0) {
        return false;
    }
    if ((supported & EFI_PCI_IO_ATTRIBUTE_VGA_IO) != 0) {
        vga_io = EFI_PCI_IO_ATTRIBUTE_VGA_IO;
    } else if ((supported & EFI_PCI_IO_ATTRIBUTE_VGA_IO_16) != 0) {
        vga_io = EFI_PCI_IO_ATTRIBUTE_VGA_IO_16;
    } else {
        return false;
    }
    efi_status status = io->attributes(
        io, EFI_PCI_IO_ATTRIBUTE_OPERATION_ENABLE, EFI_PCI_IO_ATTRIBUTE_VGA_MEMORY | vga_io, NULL
    );
    return status == EFI_SUCCESS;
}

// Only one device decodes the VGA ranges, so another one that does, such as
// the card the firmware drives when the configuration names another, gives
// them up, and gets them back if the device still cannot have them.
static bool take_vga_decoding(const struct pci_io_device *dev) {
    if (enable_vga_decoding(dev->io)) {
        return true;
    }

    size_t count;
    struct pci_io_device *devices = pci_io_devices(&count);
    uint64_t *held;
    bool taken = false;
    if (devices == NULL
     || efi_bs->allocate_pool(EFI_LOADER_DATA, count * sizeof(uint64_t), (void **)&held)
         != EFI_SUCCESS) {
        if (devices != NULL) {
            efi_bs->free_pool(devices);
        }
        return false;
    }

    for (size_t i = 0; i < count; i++) {
        struct efi_pci_io_protocol *io = devices[i].io;
        uint64_t attributes;
        held[i] = 0;
        if (handoff_pci_address_equal(devices[i].address, dev->address)
         || io->attributes(io, EFI_PCI_IO_ATTRIBUTE_OPERATION_GET, 0, &attributes) != EFI_SUCCESS
         || (attributes & VGA_ATTRIBUTES) == 0) {
            continue;
        }
        efi_status status = io->attributes(
            io, EFI_PCI_IO_ATTRIBUTE_OPERATION_DISABLE, attributes & VGA_ATTRIBUTES, NULL
        );
        if (status == EFI_SUCCESS) {
            held[i] = attributes & VGA_ATTRIBUTES;
        }
    }

    taken = enable_vga_decoding(dev->io);
    for (size_t i = 0; i < count; i++) {
        if (held[i] == 0) {
            continue;
        }
        if (taken) {
            printf(
                "Legacy VGA decoding moves from " HANDOFF_PCI_ADDRESS_FMT " to "
                HANDOFF_PCI_ADDRESS_FMT "\n", HANDOFF_PCI_ADDRESS_ARGS(devices[i].address),
                HANDOFF_PCI_ADDRESS_ARGS(dev->address)
            );
        } else {
            devices[i].io->attributes(
                devices[i].io, EFI_PCI_IO_ATTRIBUTE_OPERATION_ENABLE, held[i], NULL
            );
        }
    }

    efi_bs->free_pool(held);
    efi_bs->free_pool(devices);
    return taken;
}

// Uses the user's VBIOS if given, which needs no matching IDs, or else the
// legacy image of the device's expansion ROM.
static bool try_device(
    const struct pci_io_device *dev,
    const struct user_vbios *user,
    struct vbios_choice *choice
) {
    const void *image;
    size_t size;

    if (user != NULL) {
        image = user->image;
        size = user->size;
    } else if (config.vga_emulation != HANDOFF_VGA_EMULATION_NEVER && !vbios_trusted(dev)) {
        printf(
            "Skipping the VBIOS of " HANDOFF_PCI_ADDRESS_FMT
            " (%04x:%04x, revision %02x), not of a card in a slot or graphics from before 2018, "
            "but with vga_emulation = never\n", HANDOFF_PCI_ADDRESS_ARGS(dev->address),
            dev->vendor_id, dev->device_id, dev->revision
        );
        return false;
    } else {
        bool legacy = pci_rom_find_legacy_image(
            dev->io->rom_image, dev->io->rom_size, dev->vendor_id, dev->device_id, &image, &size
        );
        if (!legacy) {
            printf(
                "No legacy VBIOS in the expansion ROM of " HANDOFF_PCI_ADDRESS_FMT
                " (%04x:%04x)\n", HANDOFF_PCI_ADDRESS_ARGS(dev->address), dev->vendor_id,
                dev->device_id
            );
            return false;
        }
    }

    if (!take_vga_decoding(dev)) {
        printf(
            "Skipping the VBIOS of " HANDOFF_PCI_ADDRESS_FMT
            " (%04x:%04x), as its legacy VGA decoding cannot be enabled\n",
            HANDOFF_PCI_ADDRESS_ARGS(dev->address), dev->vendor_id, dev->device_id
        );
        return false;
    }
    // Firmware without a driver for the card, as for a Voodoo, leaves its BARs
    // undecoded, so that its VBIOS would program nothing.
    pci_io_enable_device(dev);
    uint16_t command = 0;
    dev->io->pci.read(dev->io, EFI_PCI_IO_WIDTH_UINT16, PCI_COMMAND, 1, &command);
    printf(
        "VGA device " HANDOFF_PCI_ADDRESS_FMT " decodes, command %#x\n",
        HANDOFF_PCI_ADDRESS_ARGS(dev->address), command
    );

    choice->dev = *dev;
    choice->image = image;
    choice->size = size;
    choice->source = user != NULL ? user->source : "its expansion ROM";
    return true;
}

static bool find_pci_device(struct handoff_pci_address address, struct pci_io_device *dev) {
    size_t count;
    struct pci_io_device *devices = pci_io_devices(&count);
    bool found = false;

    for (size_t i = 0; i < count && !found; i++) {
        if (handoff_pci_address_equal(devices[i].address, address)) {
            *dev = devices[i];
            found = true;
        }
    }

    if (devices != NULL) {
        efi_bs->free_pool(devices);
    }
    return found;
}

// The VBIOS path left a 2019 iMac's panel dark, and Macs without a CSM run no
// legacy images, so nothing vouches for those of their graphics cards.
static bool apple_firmware(void) {
    static const uint16_t apple[] = u"Apple";
    const uint16_t *vendor = efi_st->firmware_vendor;
    if (vendor == NULL) {
        return false;
    }
    for (size_t i = 0; apple[i] != 0; i++) {
        if (vendor[i] != apple[i]) {
            return false;
        }
    }
    return true;
}

// For when no graphics output shows which card the firmware's console is on.
static bool try_display_devices(struct vbios_choice *choice) {
    size_t count;
    struct pci_io_device *devices = pci_io_devices(&count);
    bool found = false;

    for (size_t i = 0; i < count && !found; i++) {
        if (devices[i].base_class == PCI_BASE_CLASS_DISPLAY) {
            found = try_device(&devices[i], NULL, choice);
        }
    }

    if (devices != NULL) {
        efi_bs->free_pool(devices);
    }
    return found;
}

static void record_vga_device(const struct pci_io_device *dev) {
    struct handoff_video *video = &loader_handoff->video;
    video->vga_present = true;
    video->vga_device = dev->address;
    video->vga_vendor_id = dev->vendor_id;
    video->vga_device_id = dev->device_id;
}

static bool install_vbios(const struct vbios_choice *choice) {
    struct handoff_video *video = &loader_handoff->video;
    const struct pci_io_device *dev = &choice->dev;
    size_t pages = EFI_SIZE_TO_PAGES(choice->size);

    uintptr_t copy = loader_alloc_reserved(pages, 0);
    if (copy == 0) {
        printf("Cannot allocate %zu bytes for the VBIOS\n", choice->size);
        return false;
    }
    memcpy((void *)(uintptr_t)copy, choice->image, choice->size);
    memset((uint8_t *)(uintptr_t)copy + choice->size, 0, pages * EFI_PAGE_SIZE - choice->size);

    record_vga_device(dev);
    video->vbios_present = true;
    video->vbios.address = dev->address;
    video->vbios.vendor_id = dev->vendor_id;
    video->vbios.device_id = dev->device_id;
    video->vbios.base_class = dev->base_class;
    video->vbios.subclass = dev->subclass;
    video->vbios.prog_if = dev->prog_if;
    video->vbios.size = choice->size;
    video->vbios.image = copy;

    printf(
        "VGA device " HANDOFF_PCI_ADDRESS_FMT " (%04x:%04x), VBIOS from %s, %zu bytes\n",
        HANDOFF_PCI_ADDRESS_ARGS(dev->address), dev->vendor_id, dev->device_id, choice->source,
        choice->size
    );
    return true;
}

static efi_handle gop_handle;

void video_find_framebuffer(void) {
    struct efi_graphics_output_protocol *gop = NULL;
    gop_handle = choose_gop(&gop);
    if (gop_handle != NULL) {
        struct handoff_framebuffer *fb = &loader_handoff->video.framebuffer;
        describe_framebuffer(gop, fb);
        panel_fix_framebuffer(fb);
        fb->rotation = panel_rotation(fb);
    } else {
        printf("No usable graphics output\n");
    }
}

// The PCI function of the graphics output in use: the one its device path
// leads to, else, where firmware gives the output a path that leads to none,
// the display function one of whose BARs holds its framebuffer, as Linux 7.3
// finds the boot display's (drivers/video/screen_info_pci.c,
// screen_info_pci_dev()).
static bool gop_device(struct pci_io_device *dev) {
    if (gop_pci_device(gop_handle, dev)) {
        return true;
    }
    uint64_t base = loader_handoff->video.framebuffer.base;
    if (base == 0 || !pci_io_display_holding(base, dev)) {
        return false;
    }
    printf(
        "The graphics output's framebuffer is in a BAR of " HANDOFF_PCI_ADDRESS_FMT "\n",
        HANDOFF_PCI_ADDRESS_ARGS(dev->address)
    );
    return true;
}

void video_init(void) {
    struct user_vbios user;
    struct pci_io_device dev;
    struct vbios_choice choice;
    bool found = false;
    bool have_dev = false;

    bool have_user = load_user_vbios(&user);
    const struct user_vbios *user_vbios = have_user ? &user : NULL;
    bool automatic = config.vga_emulation == HANDOFF_VGA_EMULATION_AUTO && !have_user;
    bool apple = automatic && apple_firmware();
    // A VBIOS draws as the panel scans, sideways on one mounted sideways.
    bool sideways = automatic && loader_handoff->video.framebuffer.rotation != HANDOFF_ROTATE_0;
    bool skip = apple || sideways;

    if (config.vga_specified) {
        have_dev = find_pci_device(config.vga_device, &dev);
        if (have_dev) {
            found = !skip && try_device(&dev, user_vbios, &choice);
        } else {
            printf(
                "The configured VGA device " HANDOFF_PCI_ADDRESS_FMT " does not exist\n",
                HANDOFF_PCI_ADDRESS_ARGS(config.vga_device)
            );
        }
    } else if (gop_handle != NULL && gop_device(&dev)) {
        // Another card's VBIOS would put the legacy display where the
        // firmware's is not.
        have_dev = true;
        found = !skip && try_device(&dev, user_vbios, &choice);
    } else if (have_user) {
        printf(
            "Not using %s as the VBIOS: no VGA device is configured, and no graphics output leads "
            "to one\n", user.source
        );
    } else if (gop_handle == NULL) {
        found = !skip && try_display_devices(&choice);
    }

    if (!found || !install_vbios(&choice)) {
        const char *message = "No usable VBIOS\n";
        if (apple) {
            message = "No VBIOS under Apple's firmware but with vga_emulation = never\n";
        } else if (sideways) {
            message = "No VBIOS on a panel mounted sideways but with vga_emulation = never\n";
        }
        printf("%s", message);
        // It keeps its legacy VGA resources, for the emulated VGA to stand in
        // for.
        if (have_dev) {
            record_vga_device(&dev);
        }
    }

    if (user.buffer != NULL) {
        efi_bs->free_pool(user.buffer);
    }
}

void video_exit_boot_services(void) {
    struct handoff_video *video = &loader_handoff->video;
    struct pci_io_device dev;
    size_t count;
    bool driven = false;

    // The emulated VGA draws on the framebuffer the driver set up, which some
    // drivers turn off when stopped.
    if (!video->vbios_present || display_vga_emulated()) {
        return;
    }
    if (efi_bs->hdr.revision < EFI_1_10_SYSTEM_TABLE_REVISION) {
        printf("Cannot stop the firmware's graphics output before EFI 1.10\n");
        return;
    }

    // What draws through the graphics output stops, the graphics driver itself
    // staying bound to its device, which a driver stopped can turn off: the
    // graphics output's own handle is the one disconnected, which is a child
    // of the device's where the driver makes one.
    efi_handle gop = NULL;
    efi_handle *handles = efi_glue_locate_handles(&gop_guid, &count);
    for (size_t i = 0; i < count && !driven; i++) {
        driven = gop_pci_device(handles[i], &dev)
              && handoff_pci_address_equal(dev.address, video->vga_device);
        if (driven) {
            gop = handles[i];
        }
    }
    if (handles != NULL) {
        efi_bs->free_pool(handles);
    }
    if (!driven) {
        return;
    }

    // Where the graphics output is the device's own handle, the driver stops,
    // and a stopped driver restores the attributes the device had before it
    // started, and a VGA controller starts with its decoding off.
    uint64_t attributes;
    efi_status status = dev.io->attributes(
        dev.io, EFI_PCI_IO_ATTRIBUTE_OPERATION_GET, 0, &attributes
    );
    if (status != EFI_SUCCESS) {
        attributes = 0;
    }
    attributes &= VGA_DECODE_ATTRIBUTES;

    status = efi_bs->disconnect_controller(gop, NULL, NULL);
    if (status != EFI_SUCCESS) {
        printf(
            "Cannot stop the firmware's graphics output for " HANDOFF_PCI_ADDRESS_FMT
            ", status %#llx\n", HANDOFF_PCI_ADDRESS_ARGS(dev.address), (unsigned long long)status
        );
        return;
    }
    if (attributes != 0) {
        status = dev.io->attributes(
            dev.io, EFI_PCI_IO_ATTRIBUTE_OPERATION_ENABLE, attributes, NULL
        );
        if (status != EFI_SUCCESS) {
            printf(
                "Cannot restore the decoding of " HANDOFF_PCI_ADDRESS_FMT
                " after stopping its graphics output\n",
                HANDOFF_PCI_ADDRESS_ARGS(dev.address)
            );
        }
    }
}
