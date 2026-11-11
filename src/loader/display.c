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

// An EDID's blocks, the base block's version, extension block count, and
// checksum, and the tag of the CTA's extension blocks (VESA E-EDID A2, 3.3,
// 3.5, and 3.11).
#define EDID_BLOCK 128
#define EDID_VERSION 0x12
#define EDID_EXTENSIONS 0x7e
#define EDID_CHECKSUM 0x7f
#define EDID_TAG_CTA 0x02

static const uint8_t edid_header[8] = { 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00 };

// Room for two of the framebuffer's frames at 32 bits per pixel, a power of
// two, as the emulated display controller's BAR has its size.
#define VRAM_MIN ((uint64_t)16 << 20)
#define VRAM_MAX ((uint64_t)256 << 20)

static struct efi_guid gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
static struct efi_guid edid_active_guid = EFI_EDID_ACTIVE_PROTOCOL_GUID;
static struct efi_guid edid_discovered_guid = EFI_EDID_DISCOVERED_PROTOCOL_GUID;

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
    uint64_t back_size = (uint64_t)fb->pitch * fb->height;
    uintptr_t back = loader_alloc_reserved(EFI_SIZE_TO_PAGES(back_size), 0);
    if (back == 0) {
        printf(
            "Cannot reserve %llu MiB for the emulated VGA's back buffer\n",
            (unsigned long long)((back_size + (1 << 20) - 1) >> 20)
        );
        return;
    }
    video->back_base = back;
    video->back_size = back_size;

    uint64_t frame = (uint64_t)fb->width * fb->height * 4;
    size_t size = VRAM_MIN;
    while (size < 2 * frame && size < VRAM_MAX) {
        size <<= 1;
    }
    uintptr_t base = loader_alloc_reserved(EFI_SIZE_TO_PAGES(2 * size), 0);
    if (base == 0) {
        printf(
            "Cannot reserve %llu MiB of video memory for the emulated VGA\n",
            (unsigned long long)(2 * size >> 20)
        );
        return;
    }
    memset((void *)(uintptr_t)base, 0, 2 * size);
    video->vram_base = base;
    video->vram_size = size;
    printf(
        "Video memory for the emulated VGA: %llu MiB at %#llx, and as much for its copy\n",
        (unsigned long long)(size >> 20), (unsigned long long)base
    );
}

static uint8_t edid_sum(const uint8_t *block) {
    uint8_t sum = 0;
    for (size_t i = 0; i < EDID_BLOCK; i++) {
        sum += block[i];
    }
    return sum;
}

// A base block: the header, version 1, and bytes that sum to 0.
static bool edid_base_valid(const uint8_t *block) {
    return memcmp(block, edid_header, sizeof(edid_header)) == 0 && block[EDID_VERSION] == 1
        && edid_sum(block) == 0;
}

// Keeps the extension blocks the base block counts and size holds whose bytes
// sum to 0, or that are the CTA's whatever their sum, and makes the base
// block's count and checksum match, as Linux 7.3's drm_edid.c has it
// (edid_block_status_valid(), edid_filter_invalid_blocks()).
static size_t edid_filter(uint8_t *edid, size_t size) {
    size_t count = (size_t)edid[EDID_EXTENSIONS] + 1;
    if (count > size / EDID_BLOCK) {
        count = size / EDID_BLOCK;
    }
    size_t blocks = 1;
    for (size_t i = 1; i < count; i++) {
        const uint8_t *block = edid + i * EDID_BLOCK;
        if (edid_sum(block) == 0 || block[0] == EDID_TAG_CTA) {
            memmove(edid + blocks * EDID_BLOCK, block, EDID_BLOCK);
            blocks++;
        }
    }
    edid[EDID_EXTENSIONS] = (uint8_t)(blocks - 1);
    edid[EDID_CHECKSUM] = 0;
    edid[EDID_CHECKSUM] = (uint8_t)-edid_sum(edid);
    return blocks * EDID_BLOCK;
}

// The EDID that the framebuffer's graphics output has, as the display reports
// it or else as the firmware found it, if its base block is valid.
static void collect_edid(struct handoff_video *video) {
    size_t count;
    efi_handle *handles = efi_glue_locate_handles(&gop_guid, &count);
    if (handles == NULL) {
        return;
    }
    for (size_t i = 0; i < count && video->edid_size == 0; i++) {
        struct efi_graphics_output_protocol *gop;
        if (efi_bs->handle_protocol(handles[i], &gop_guid, (void **)&gop) != EFI_SUCCESS
         || gop->mode == NULL || gop->mode->frame_buffer_base != video->framebuffer.base) {
            continue;
        }
        struct efi_guid *guids[] = { &edid_active_guid, &edid_discovered_guid };
        for (size_t g = 0; g < 2; g++) {
            // Both protocols have the same layout.
            struct efi_edid_active_protocol *edid;
            if (efi_bs->handle_protocol(handles[i], guids[g], (void **)&edid) != EFI_SUCCESS
             || edid->size_of_edid < EDID_BLOCK || edid->edid == NULL
             || !edid_base_valid(edid->edid)) {
                continue;
            }
            size_t size = edid->size_of_edid < HANDOFF_MAX_EDID
                        ? edid->size_of_edid
                        : HANDOFF_MAX_EDID;
            size &= ~(size_t)(EDID_BLOCK - 1);
            memcpy(video->edid, edid->edid, size);
            size = edid_filter(video->edid, size);
            video->edid_size = size;
            printf("Display EDID: %zu bytes, %s\n", size, g == 0 ? "active" : "discovered");
            break;
        }
    }
    efi_bs->free_pool(handles);
}

void display_collect(void) {
    struct handoff *h = loader_handoff;
    if (!display_vga_emulated()) {
        return;
    }
    reserve_vram(&h->video);
    collect_edid(&h->video);
}
