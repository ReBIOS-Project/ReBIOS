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
#include <stdbool.h>
#include <memory.h>
#include <handoff.h>
#include <lib/log.h>
#include <loader/display.h>
#include <loader/loader.h>

// Room for two of the framebuffer's frames at 32 bits per pixel, a power of
// two, as the emulated display controller's BAR has its size.
#define VRAM_MIN (16ull << 20)
#define VRAM_MAX (256ull << 20)

// As the core's vga_init() decides.
static bool vga_emulated(const struct handoff *h) {
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
    uint64_t size = VRAM_MIN;
    while (size < 2 * frame && size < VRAM_MAX) {
        size <<= 1;
    }
    uint64_t base = loader_alloc_reserved(size / 4096, 0);
    if (base == 0) {
        printf("Cannot reserve %llu MiB of video memory for the emulated VGA\n",
               (unsigned long long)(size >> 20));
        return;
    }
    memset((void *)(uintptr_t)base, 0, size);
    video->vram_base = base;
    video->vram_size = size;
    printf("Video memory for the emulated VGA: %llu MiB at %#llx\n",
           (unsigned long long)(size >> 20), (unsigned long long)base);
}

void display_collect(void) {
    struct handoff *h = loader_handoff;
    if (!vga_emulated(h)) {
        return;
    }
    reserve_vram(&h->video);
}
