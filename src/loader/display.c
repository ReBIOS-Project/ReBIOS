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
#include <loader/display.h>
#include <handoff.h>
#include <lib/log.h>
#include <loader/efi_glue.h>
#include <loader/loader.h>
#include <memory.h>

// Room for two of the framebuffer's frames at 32 bits per pixel, a power of
// two, as the emulated display controller's BAR has its size.
#define VRAM_MIN ((uint64_t)16 << 20)
#define VRAM_MAX ((uint64_t)256 << 20)

static EFI_GUID gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
static EFI_GUID edid_active_guid = EFI_EDID_ACTIVE_PROTOCOL_GUID;
static EFI_GUID edid_discovered_guid = EFI_EDID_DISCOVERED_PROTOCOL_GUID;

bool display_vga_emulated(void) {
    const struct handoff *h = loader_handoff;
    const struct handoff_framebuffer *fb = &h->video.framebuffer;
    if (!fb->present || (fb->bpp != 32 && fb->bpp != 24 && fb->bpp != 16)) {
        return false;
    }
    switch (h->config.vga_emulation) {
        case HANDOFF_VGA_EMULATION_NEVER: {
            return false;
        }
        case HANDOFF_VGA_EMULATION_ALWAYS: {
            return true;
        }
        default: {
            return !h->video.vbios_present;
        }
    }
}

static void reserve_vram(struct handoff_video *video) {
    const struct handoff_framebuffer *fb = &video->framebuffer;
    uint64_t frame = (uint64_t)fb->width * fb->height * 4;
    size_t size = VRAM_MIN;
    while (size < 2 * frame && size < VRAM_MAX) {
        size <<= 1;
    }
    uintptr_t base = loader_alloc_reserved(size / 4096, 0);
    if (base == 0) {
        printf(
            "Cannot reserve %llu MiB of video memory for the emulated VGA\n",
            (unsigned long long)(size >> 20)
        );
        return;
    }
    memset((void *)(uintptr_t)base, 0, size);
    video->vram_base = base;
    video->vram_size = size;
    printf(
        "Video memory for the emulated VGA: %llu MiB at %#llx\n",
        (unsigned long long)(size >> 20), (unsigned long long)base
    );
}

// The EDID that the framebuffer's graphics output has, as the display reports
// it or else as the firmware found it.
static void collect_edid(struct handoff_video *video) {
    size_t count;
    EFI_HANDLE *handles = efi_locate_handles(&gop_guid, &count);
    if (handles == NULL) {
        return;
    }
    for (size_t i = 0; i < count && video->edid_size == 0; i++) {
        EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;
        if (efi_bs->HandleProtocol(handles[i], &gop_guid, (void **)&gop)
         != EFI_SUCCESS
         || gop->Mode == NULL
         || gop->Mode->FrameBufferBase != video->framebuffer.base) {
            continue;
        }
        EFI_GUID *guids[] = { &edid_active_guid, &edid_discovered_guid };
        for (size_t g = 0; g < 2; g++) {
            // Both protocols have the same layout.
            EFI_EDID_ACTIVE_PROTOCOL *edid;
            if (efi_bs->HandleProtocol(handles[i], guids[g], (void **)&edid)
             != EFI_SUCCESS
             || edid->SizeOfEdid < 128 || edid->Edid == NULL) {
                continue;
            }
            size_t size = edid->SizeOfEdid < HANDOFF_MAX_EDID
                ? edid->SizeOfEdid : HANDOFF_MAX_EDID;
            size &= ~(uint32_t)127;
            memcpy(video->edid, edid->Edid, size);
            video->edid_size = size;
            printf(
                "Display EDID: %zu bytes, %s\n", size,
                g == 0 ? "active" : "discovered"
            );
            break;
        }
    }
    efi_bs->FreePool(handles);
}

void display_collect(void) {
    struct handoff *h = loader_handoff;
    if (!display_vga_emulated()) {
        return;
    }
    reserve_vram(&h->video);
    collect_edid(&h->video);
}
