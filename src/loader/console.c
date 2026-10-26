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
#include <flanterm.h>
#include <flanterm_backends/fb.h>
#include <handoff.h>
#include <lib/log.h>
#include <loader/console.h>
#include <loader/loader.h>

// The memory stays the hypervisor's, as the console outlives boot services.
static void *console_alloc(size_t size) {
    uint64_t base = loader_alloc_reserved((size + 4095) / 4096, 0);
    return base != 0 ? (void *)(uintptr_t)base : NULL;
}

// Flanterm only frees what a start that failed allocated, which stays
// reserved.
static void console_free(void *ptr, size_t size) {
    (void)ptr;
    (void)size;
}

void console_init(void) {
    const struct handoff_framebuffer *fb = &loader_handoff->video.framebuffer;
    if (!fb->present || fb->bpp != 32) {
        return;
    }
    int rotation = FLANTERM_FB_ROTATE_0;
    if (fb->rotation == HANDOFF_ROTATE_90) {
        rotation = FLANTERM_FB_ROTATE_90;
    } else if (fb->rotation == HANDOFF_ROTATE_270) {
        rotation = FLANTERM_FB_ROTATE_270;
    }

    struct flanterm_context *screen = flanterm_fb_init(
        console_alloc, console_free, (uint32_t *)(uintptr_t)fb->base, fb->width, fb->height,
        fb->pitch, fb->red_size, fb->red_shift, fb->green_size, fb->green_shift, fb->blue_size,
        fb->blue_shift, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, 0, 0, 0, 0, 0, 0,
        rotation, false);
    if (screen != NULL) {
        log_set_screen(screen);
    }
}
