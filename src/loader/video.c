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
#include <efi.h>
#include <memory.h>
#include <handoff.h>
#include <lib/log.h>
#include <lib/printf.h>
#include <loader/cbfs.h>
#include <loader/config.h>
#include <loader/efi_glue.h>
#include <loader/loader.h>
#include <loader/pci_io.h>
#include <loader/pci_rom.h>
#include <loader/video.h>

#define VBIOS_FILE_MAX (256u * 1024)
#define CBFS_SETTING "cbfs"
#define CBFS_SETTING_PREFIX "cbfs:"
#define CBFS_DEFAULT_NAME "vgabios.bin"

#define AMD_VENDOR_ID 0x1002

// What a stopped graphics driver may turn off, and the VBIOS needs.
#define VGA_DECODE_ATTRIBUTES \
    (EFI_PCI_IO_ATTRIBUTE_IO | EFI_PCI_IO_ATTRIBUTE_MEMORY | EFI_PCI_IO_ATTRIBUTE_BUS_MASTER \
     | EFI_PCI_IO_ATTRIBUTE_VGA_MEMORY | EFI_PCI_IO_ATTRIBUTE_VGA_IO \
     | EFI_PCI_IO_ATTRIBUTE_VGA_IO_16)

#define VGA_ATTRIBUTES \
    (EFI_PCI_IO_ATTRIBUTE_VGA_MEMORY | EFI_PCI_IO_ATTRIBUTE_VGA_IO \
     | EFI_PCI_IO_ATTRIBUTE_VGA_IO_16)

static EFI_GUID gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
static EFI_GUID device_path_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
static EFI_GUID loaded_image_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
static EFI_GUID file_system_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
static EFI_GUID file_info_guid = EFI_FILE_INFO_ID;

// AMD integrated Vega graphics, whose VBIOS works unlike that of the other
// integrated graphics.
static const uint16_t amd_working_igpus[] = {0x15d8, 0x15dd, 0x15e7, 0x1636, 0x1638, 0x164c};

// A VBIOS image the configuration names.
struct user_vbios {
    const void *image;
    uint32_t size;
    // Pool memory holding the image, if it was read from a file.
    void *buffer;
    char source[CONFIG_VGABIOS_MAX + 16];
};

struct vbios_choice {
    struct pci_io_device dev;
    const void *image;
    uint32_t size;
    const char *source;
};

static bool setting_starts_with(const EFI_CHAR16 *setting, const char *prefix) {
    for (size_t i = 0; prefix[i] != 0; i++) {
        if (setting[i] != (uint8_t)prefix[i]) {
            return false;
        }
    }
    return true;
}

static void setting_to_ascii(const EFI_CHAR16 *setting, char *out, size_t out_size) {
    size_t i = 0;
    for (; i + 1 < out_size && setting[i] != 0; i++) {
        out[i] = setting[i] < 0x80 ? (char)setting[i] : '?';
    }
    out[i] = 0;
}

static bool gop_mode_usable(EFI_GRAPHICS_OUTPUT_PROTOCOL *gop) {
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *mode = gop->Mode;
    if (mode == NULL || mode->Info == NULL || mode->FrameBufferBase == 0) {
        return false;
    }

    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = mode->Info;
    EFI_PIXEL_BITMASK *masks = &info->PixelInformation;
    bool format_usable = info->PixelFormat == EfiPixelRedGreenBlueReserved8BitPerColor
                         || info->PixelFormat == EfiPixelBlueGreenRedReserved8BitPerColor
                         || (info->PixelFormat == EfiPixelBitMask
                             && (masks->RedMask | masks->GreenMask | masks->BlueMask
                                 | masks->ReservedMask) != 0);

    return format_usable && info->HorizontalResolution != 0 && info->VerticalResolution != 0
           && info->PixelsPerScanLine >= info->HorizontalResolution;
}

// Leaves the graphics output in a usable mode if it has one, preferring the
// current mode so as not to disturb the display.
static bool gop_prepare(EFI_GRAPHICS_OUTPUT_PROTOCOL *gop) {
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = NULL;
    EFI_UINTN info_size;

    if (gop->Mode == NULL) {
        return false;
    }

    // Some firmware leaves the protocol unstarted until a mode is set.
    EFI_STATUS status = gop->QueryMode(gop, gop->Mode->Mode, &info_size, &info);
    if (status == EFI_NOT_STARTED) {
        gop->SetMode(gop, 0);
        status = gop->QueryMode(gop, gop->Mode->Mode, &info_size, &info);
    }
    // The buffer is the caller's, unless the firmware handed out its own.
    if (status == EFI_SUCCESS && info != NULL && info != gop->Mode->Info) {
        efi_bs->FreePool(info);
    }

    if (gop_mode_usable(gop)) {
        return true;
    }
    EFI_UINT32 original = gop->Mode->Mode;
    for (EFI_UINT32 mode = 0; mode < gop->Mode->MaxMode; mode++) {
        if (gop->SetMode(gop, mode) == EFI_SUCCESS && gop_mode_usable(gop)) {
            return true;
        }
    }
    if (original < gop->Mode->MaxMode) {
        gop->SetMode(gop, original);
    }
    return false;
}

static bool gop_pci_device(EFI_HANDLE handle, struct pci_io_device *dev) {
    EFI_DEVICE_PATH_PROTOCOL *path;
    if (efi_bs->HandleProtocol(handle, &device_path_guid, (void **)&path) != EFI_SUCCESS
        || path == NULL) {
        return false;
    }
    return pci_io_device_on_path(path, dev);
}

static bool gop_of_configured_device(EFI_HANDLE handle) {
    struct pci_io_device dev;
    return config.vga_specified && gop_pci_device(handle, &dev)
           && pci_address_equal(dev.address, config.vga_device);
}

// Returns the first usable graphics output, preferring those of the
// configured VGA device, or NULL if there is none.
static EFI_HANDLE choose_gop(EFI_GRAPHICS_OUTPUT_PROTOCOL **chosen) {
    EFI_UINTN count;
    EFI_HANDLE *handles = efi_locate_handles(&gop_guid, &count);
    EFI_HANDLE found = NULL;

    for (int pass = 0; pass < 2 && found == NULL; pass++) {
        for (EFI_UINTN i = 0; i < count && found == NULL; i++) {
            EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;
            if (gop_of_configured_device(handles[i]) != (pass == 0)) {
                continue;
            }
            if (efi_bs->HandleProtocol(handles[i], &gop_guid, (void **)&gop) == EFI_SUCCESS
                && gop != NULL && gop_prepare(gop)) {
                found = handles[i];
                *chosen = gop;
            }
        }
    }

    if (handles != NULL) {
        efi_bs->FreePool(handles);
    }
    return found;
}

static uint8_t mask_shift(uint32_t mask) {
    return mask == 0 ? 0 : (uint8_t)__builtin_ctz(mask);
}

static uint8_t mask_size(uint32_t mask) {
    return (uint8_t)__builtin_popcount(mask);
}

// Takes the current mode from Mode->Info rather than from QueryMode(), as
// some firmware reports a wrong PixelsPerScanLine through the latter.
static void describe_framebuffer(EFI_GRAPHICS_OUTPUT_PROTOCOL *gop,
                                 struct handoff_framebuffer *fb) {
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *mode = gop->Mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = mode->Info;
    EFI_PIXEL_BITMASK masks = info->PixelInformation;

    if (info->PixelFormat == EfiPixelRedGreenBlueReserved8BitPerColor) {
        masks = (EFI_PIXEL_BITMASK){0x000000ff, 0x0000ff00, 0x00ff0000, 0xff000000};
    } else if (info->PixelFormat == EfiPixelBlueGreenRedReserved8BitPerColor) {
        masks = (EFI_PIXEL_BITMASK){0x00ff0000, 0x0000ff00, 0x000000ff, 0xff000000};
    }

    uint32_t all = masks.RedMask | masks.GreenMask | masks.BlueMask | masks.ReservedMask;
    uint32_t bpp = (32 - (uint32_t)__builtin_clz(all) + 7) / 8 * 8;

    fb->base = mode->FrameBufferBase;
    fb->size = mode->FrameBufferSize;
    fb->width = info->HorizontalResolution;
    fb->height = info->VerticalResolution;
    fb->pitch = info->PixelsPerScanLine * (bpp / 8);
    fb->bpp = (uint8_t)bpp;
    fb->red_size = mask_size(masks.RedMask);
    fb->red_shift = mask_shift(masks.RedMask);
    fb->green_size = mask_size(masks.GreenMask);
    fb->green_shift = mask_shift(masks.GreenMask);
    fb->blue_size = mask_size(masks.BlueMask);
    fb->blue_shift = mask_shift(masks.BlueMask);
    fb->reserved_size = mask_size(masks.ReservedMask);
    fb->reserved_shift = mask_shift(masks.ReservedMask);
    fb->present = 1;

    printf("Framebuffer at %#llx, %ux%u, %u bpp, pitch %u\n", (unsigned long long)fb->base,
           fb->width, fb->height, bpp, fb->pitch);
}

static bool load_cbfs_vbios(struct user_vbios *vbios) {
    const EFI_CHAR16 *setting = config.vgabios;
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

static bool file_size(EFI_FILE_PROTOCOL *file, uint64_t *size) {
    EFI_UINTN info_size = 0;
    EFI_FILE_INFO *info;
    bool ok = false;

    if (file->GetInfo(file, &file_info_guid, &info_size, NULL) != EFI_BUFFER_TOO_SMALL
        || efi_bs->AllocatePool(EfiLoaderData, info_size, (void **)&info) != EFI_SUCCESS) {
        return false;
    }
    if (file->GetInfo(file, &file_info_guid, &info_size, info) == EFI_SUCCESS
        && !(info->Attribute & EFI_FILE_DIRECTORY)) {
        *size = info->FileSize;
        ok = true;
    }
    efi_bs->FreePool(info);
    return ok;
}

// Returns the contents in pool memory, or NULL.
static void *read_file(EFI_FILE_PROTOCOL *file, uint32_t size) {
    uint8_t *buffer;

    if (efi_bs->AllocatePool(EfiLoaderData, size, (void **)&buffer) != EFI_SUCCESS) {
        return NULL;
    }
    for (uint32_t done = 0; done < size;) {
        EFI_UINTN chunk = size - done;
        if (file->Read(file, &chunk, buffer + done) != EFI_SUCCESS || chunk == 0
            || chunk > size - done) {
            efi_bs->FreePool(buffer);
            return NULL;
        }
        done += (uint32_t)chunk;
    }
    return buffer;
}

static bool load_file_vbios(struct user_vbios *vbios) {
    EFI_LOADED_IMAGE_PROTOCOL *loaded_image;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs;
    EFI_FILE_PROTOCOL *root;
    EFI_FILE_PROTOCOL *file;
    EFI_CHAR16 path[CONFIG_VGABIOS_MAX];
    char name[CONFIG_VGABIOS_MAX];
    uint64_t size = 0;

    // FAT names cannot hold forward slashes, so these can only be separators.
    size_t i = 0;
    for (; i + 1 < CONFIG_VGABIOS_MAX && config.vgabios[i] != 0; i++) {
        path[i] = config.vgabios[i] == u'/' ? u'\\' : config.vgabios[i];
    }
    path[i] = 0;
    setting_to_ascii(path, name, sizeof(name));

    if (efi_bs->HandleProtocol(efi_image_handle, &loaded_image_guid,
                               (void **)&loaded_image) != EFI_SUCCESS
        || efi_bs->HandleProtocol(loaded_image->DeviceHandle, &file_system_guid,
                                  (void **)&fs) != EFI_SUCCESS
        || fs->OpenVolume(fs, &root) != EFI_SUCCESS) {
        printf("Cannot open the volume ReBIOS was loaded from to read the VBIOS file %s\n",
               name);
        return false;
    }

    if (root->Open(root, &file, path, EFI_FILE_MODE_READ, 0) != EFI_SUCCESS) {
        printf("Cannot open the VBIOS file %s\n", name);
    } else {
        if (!file_size(file, &size)) {
            printf("Cannot read the VBIOS file %s\n", name);
        } else if (size == 0 || size > VBIOS_FILE_MAX) {
            printf("The VBIOS file %s is empty or larger than %u KiB\n", name,
                   VBIOS_FILE_MAX / 1024);
        } else if ((vbios->buffer = read_file(file, (uint32_t)size)) == NULL) {
            printf("Cannot read the VBIOS file %s\n", name);
        }
        file->Close(file);
    }
    root->Close(root);

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
    const EFI_CHAR16 *setting = config.vgabios;
    bool loaded;

    memset(vbios, 0, sizeof(*vbios));
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
            efi_bs->FreePool(vbios->buffer);
            vbios->buffer = NULL;
        }
        return false;
    }
    return true;
}

// AMD's RDNA generations, and its integrated graphics but for the Vega ones.
static bool vbios_known_broken(uint16_t vendor_id, uint16_t device_id) {
    if (vendor_id != AMD_VENDOR_ID) {
        return false;
    }
    if (device_id >= 0x7300 && device_id <= 0x75ff) {
        return true;
    }
    bool integrated = (device_id >= 0x1400 && device_id <= 0x16ff)
                      || (device_id >= 0x1900 && device_id <= 0x19ff);
    if (!integrated) {
        return false;
    }
    for (size_t i = 0; i < sizeof(amd_working_igpus) / sizeof(amd_working_igpus[0]); i++) {
        if (device_id == amd_working_igpus[i]) {
            return false;
        }
    }
    return true;
}

// The aliased variant of the I/O decoding is preferred, as legacy software
// expects ISA-style decoding.
static bool enable_vga_decoding(EFI_PCI_IO_PROTOCOL *io) {
    EFI_UINT64 supported;
    EFI_UINT64 vga_io;

    if (io->Attributes(io, EfiPciIoAttributeOperationSupported, 0, &supported) != EFI_SUCCESS
        || !(supported & EFI_PCI_IO_ATTRIBUTE_VGA_MEMORY)) {
        return false;
    }
    if (supported & EFI_PCI_IO_ATTRIBUTE_VGA_IO) {
        vga_io = EFI_PCI_IO_ATTRIBUTE_VGA_IO;
    } else if (supported & EFI_PCI_IO_ATTRIBUTE_VGA_IO_16) {
        vga_io = EFI_PCI_IO_ATTRIBUTE_VGA_IO_16;
    } else {
        return false;
    }
    return io->Attributes(io, EfiPciIoAttributeOperationEnable,
                          EFI_PCI_IO_ATTRIBUTE_VGA_MEMORY | vga_io, NULL) == EFI_SUCCESS;
}

// Only one device decodes the VGA ranges, so another one that does, such as
// the card the firmware drives when the configuration names another, gives
// them up, and gets them back if the device still cannot have them.
static bool take_vga_decoding(const struct pci_io_device *dev) {
    if (enable_vga_decoding(dev->io)) {
        return true;
    }

    EFI_UINTN count;
    struct pci_io_device *devices = pci_io_devices(&count);
    EFI_UINT64 *held;
    bool taken = false;
    if (devices == NULL || efi_bs->AllocatePool(EfiLoaderData, count * sizeof(*held),
                                                (void **)&held) != EFI_SUCCESS) {
        if (devices != NULL) {
            efi_bs->FreePool(devices);
        }
        return false;
    }

    for (EFI_UINTN i = 0; i < count; i++) {
        EFI_PCI_IO_PROTOCOL *io = devices[i].io;
        EFI_UINT64 attributes;
        held[i] = 0;
        if (pci_address_equal(devices[i].address, dev->address)
            || io->Attributes(io, EfiPciIoAttributeOperationGet, 0, &attributes) != EFI_SUCCESS
            || (attributes & VGA_ATTRIBUTES) == 0) {
            continue;
        }
        if (io->Attributes(io, EfiPciIoAttributeOperationDisable, attributes & VGA_ATTRIBUTES,
                           NULL) == EFI_SUCCESS) {
            held[i] = attributes & VGA_ATTRIBUTES;
        }
    }

    taken = enable_vga_decoding(dev->io);
    for (EFI_UINTN i = 0; i < count; i++) {
        if (held[i] == 0) {
            continue;
        }
        if (taken) {
            printf("Legacy VGA decoding moves from " PCI_ADDRESS_FMT " to " PCI_ADDRESS_FMT "\n",
                   PCI_ADDRESS_ARGS(devices[i].address), PCI_ADDRESS_ARGS(dev->address));
        } else {
            devices[i].io->Attributes(devices[i].io, EfiPciIoAttributeOperationEnable, held[i],
                                      NULL);
        }
    }

    efi_bs->FreePool(held);
    efi_bs->FreePool(devices);
    return taken;
}

// Uses the user's VBIOS if given, which needs no matching IDs, or else the
// legacy image of the device's expansion ROM.
static bool try_device(const struct pci_io_device *dev, const struct user_vbios *user,
                       struct vbios_choice *choice) {
    const void *image;
    uint32_t size;

    if (user != NULL) {
        image = user->image;
        size = user->size;
    } else if (vbios_known_broken(dev->vendor_id, dev->device_id)) {
        printf("Skipping the VBIOS of " PCI_ADDRESS_FMT " (%04x:%04x), known to be broken\n",
               PCI_ADDRESS_ARGS(dev->address), dev->vendor_id, dev->device_id);
        return false;
    } else if (!pci_rom_find_legacy_image(dev->io->RomImage, dev->io->RomSize, dev->vendor_id,
                                          dev->device_id, &image, &size)) {
        printf("No legacy VBIOS in the expansion ROM of " PCI_ADDRESS_FMT " (%04x:%04x)\n",
               PCI_ADDRESS_ARGS(dev->address), dev->vendor_id, dev->device_id);
        return false;
    }

    if (!take_vga_decoding(dev)) {
        printf("Skipping the VBIOS of " PCI_ADDRESS_FMT " (%04x:%04x), as its legacy VGA "
               "decoding cannot be enabled\n",
               PCI_ADDRESS_ARGS(dev->address), dev->vendor_id, dev->device_id);
        return false;
    }

    choice->dev = *dev;
    choice->image = image;
    choice->size = size;
    choice->source = user != NULL ? user->source : "its expansion ROM";
    return true;
}

static bool find_pci_device(struct handoff_pci_address address, struct pci_io_device *dev) {
    EFI_UINTN count;
    struct pci_io_device *devices = pci_io_devices(&count);
    bool found = false;

    for (EFI_UINTN i = 0; i < count && !found; i++) {
        if (pci_address_equal(devices[i].address, address)) {
            *dev = devices[i];
            found = true;
        }
    }

    if (devices != NULL) {
        efi_bs->FreePool(devices);
    }
    return found;
}

static bool try_other_display_devices(const struct pci_io_device *tried,
                                      struct vbios_choice *choice) {
    EFI_UINTN count;
    struct pci_io_device *devices = pci_io_devices(&count);
    bool found = false;

    for (EFI_UINTN i = 0; i < count && !found; i++) {
        if (devices[i].base_class != PCI_CLASS_DISPLAY
            || (tried != NULL && pci_address_equal(devices[i].address, tried->address))) {
            continue;
        }
        found = try_device(&devices[i], NULL, choice);
    }

    if (devices != NULL) {
        efi_bs->FreePool(devices);
    }
    return found;
}

static bool install_vbios(const struct vbios_choice *choice) {
    struct handoff_video *video = &loader_handoff->video;
    const struct pci_io_device *dev = &choice->dev;
    uint64_t pages = ((uint64_t)choice->size + 4095) / 4096;

    uint64_t copy = loader_alloc_reserved(pages, 0);
    if (copy == 0) {
        printf("Cannot allocate %u bytes for the VBIOS\n", choice->size);
        return false;
    }
    memcpy((void *)(uintptr_t)copy, choice->image, choice->size);
    memset((uint8_t *)(uintptr_t)copy + choice->size, 0, pages * 4096 - choice->size);

    video->vga_present = 1;
    video->vbios_present = 1;
    video->vga_device = dev->address;
    video->vga_vendor_id = dev->vendor_id;
    video->vga_device_id = dev->device_id;
    video->vbios.address = dev->address;
    video->vbios.vendor_id = dev->vendor_id;
    video->vbios.device_id = dev->device_id;
    video->vbios.base_class = dev->base_class;
    video->vbios.subclass = dev->subclass;
    video->vbios.prog_if = dev->prog_if;
    video->vbios.size = choice->size;
    video->vbios.image = copy;

    printf("VGA device " PCI_ADDRESS_FMT " (%04x:%04x), VBIOS from %s, %u bytes\n",
           PCI_ADDRESS_ARGS(dev->address), dev->vendor_id, dev->device_id, choice->source,
           choice->size);
    return true;
}

void video_init(void) {
    struct handoff_video *video = &loader_handoff->video;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
    struct user_vbios user;
    struct pci_io_device dev;
    struct vbios_choice choice;
    bool found = false;

    memset(video, 0, sizeof(*video));

    EFI_HANDLE gop_handle = choose_gop(&gop);
    if (gop_handle != NULL) {
        describe_framebuffer(gop, &video->framebuffer);
    } else {
        printf("No usable graphics output\n");
    }

    bool have_user = load_user_vbios(&user);
    const struct user_vbios *user_vbios = have_user ? &user : NULL;

    if (config.vga_specified) {
        if (find_pci_device(config.vga_device, &dev)) {
            found = try_device(&dev, user_vbios, &choice);
        } else {
            printf("The configured VGA device " PCI_ADDRESS_FMT " does not exist\n",
                   PCI_ADDRESS_ARGS(config.vga_device));
        }
    } else if (gop_handle != NULL && gop_pci_device(gop_handle, &dev)) {
        found = try_device(&dev, user_vbios, &choice)
                || try_other_display_devices(&dev, &choice);
    } else if (have_user) {
        printf("Not using %s as the VBIOS: no VGA device is configured, and no graphics "
               "output leads to one\n", user.source);
    } else {
        found = try_other_display_devices(NULL, &choice);
    }

    if (!found || !install_vbios(&choice)) {
        printf("No usable VBIOS\n");
    }

    if (user.buffer != NULL) {
        efi_bs->FreePool(user.buffer);
    }
}

void video_exit_boot_services(void) {
    struct handoff_video *video = &loader_handoff->video;
    struct pci_io_device dev;
    EFI_UINTN count;
    bool driven = false;

    if (!video->vbios_present) {
        return;
    }
    if (efi_bs->Hdr.Revision < EFI_1_10_SYSTEM_TABLE_REVISION) {
        printf("Cannot stop the firmware's graphics driver before EFI 1.10\n");
        return;
    }

    EFI_HANDLE *handles = efi_locate_handles(&gop_guid, &count);
    for (EFI_UINTN i = 0; i < count && !driven; i++) {
        driven = gop_pci_device(handles[i], &dev)
                 && pci_address_equal(dev.address, video->vga_device);
    }
    if (handles != NULL) {
        efi_bs->FreePool(handles);
    }
    if (!driven) {
        return;
    }

    // A stopped driver restores the attributes the device had before it
    // started, and a VGA controller starts with its decoding off.
    EFI_UINT64 attributes;
    if (dev.io->Attributes(dev.io, EfiPciIoAttributeOperationGet, 0,
                           &attributes) != EFI_SUCCESS) {
        attributes = 0;
    }
    attributes &= VGA_DECODE_ATTRIBUTES;

    EFI_STATUS status = efi_bs->DisconnectController(dev.handle, NULL, NULL);
    if (status != EFI_SUCCESS) {
        printf("Cannot stop the firmware's graphics driver for " PCI_ADDRESS_FMT
               ", status %#llx\n", PCI_ADDRESS_ARGS(dev.address), (unsigned long long)status);
        return;
    }
    if (attributes != 0 && dev.io->Attributes(dev.io, EfiPciIoAttributeOperationEnable,
                                              attributes, NULL) != EFI_SUCCESS) {
        printf("Cannot restore the decoding of " PCI_ADDRESS_FMT
               " after stopping its graphics driver\n", PCI_ADDRESS_ARGS(dev.address));
    }
}
