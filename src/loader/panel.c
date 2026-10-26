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
#include <handoff.h>
#include <lib/log.h>
#include <loader/fwtables.h>
#include <loader/panel.h>

// The SMBIOS strings that Linux 7.3's DMI quirks match on
// (drivers/firmware/dmi_scan.c, dmi_decode()), by structure type and offset:
// DMTF DSP0134 3.9.0, Tables 10 and 13.
enum dmi_field {
    DMI_SYS_VENDOR,
    DMI_PRODUCT_NAME,
    DMI_PRODUCT_VERSION,
    DMI_BOARD_VENDOR,
    DMI_BOARD_NAME,
    DMI_FIELD_COUNT,
};

static const struct {
    uint8_t type;
    uint8_t offset;
} dmi_fields[DMI_FIELD_COUNT] = {
    [DMI_SYS_VENDOR] = {1, 0x04},
    [DMI_PRODUCT_NAME] = {1, 0x05},
    [DMI_PRODUCT_VERSION] = {1, 0x06},
    [DMI_BOARD_VENDOR] = {2, 0x04},
    [DMI_BOARD_NAME] = {2, 0x05},
};

#define DMI_MATCHES_MAX 4

// A field's string, equal to text where exact, else holding it.
struct dmi_match {
    uint8_t field;
    bool exact;
    const char *text;
};

#define DMI_EXACT_MATCH(field, text) {field, true, text}
#define DMI_MATCH(field, text) {field, false, text}

// Portrait panels whose firmware gives their framebuffer's size and pitch as
// if it were landscape, which Linux 7.3 swaps for these models
// (drivers/firmware/efi/sysfb_efi.c, efifb_dmi_swap_width_height[]). Where
// width is set, only a framebuffer given as width by height is swapped, and
// its pitch is that of the panel's memory.
struct swap_quirk {
    struct dmi_match matches[DMI_MATCHES_MAX];
    uint16_t width;
    uint16_t height;
    uint16_t pitch;
};

static const struct swap_quirk swap_quirks[] = {
    // Lenovo D330 with 800x1280 or 1200x1920 portrait screen
    {{DMI_EXACT_MATCH(DMI_SYS_VENDOR, "LENOVO"),
      DMI_EXACT_MATCH(DMI_PRODUCT_VERSION, "Lenovo ideapad D330-10IGM")},
     0, 0, 0},
    // Lenovo IdeaPad Duet 3 10IGL5 and 10IGL5-LTE with 1200x1920 portrait screen
    {{DMI_EXACT_MATCH(DMI_SYS_VENDOR, "LENOVO"),
      DMI_MATCH(DMI_PRODUCT_VERSION, "IdeaPad Duet 3 10IGL5")},
     0, 0, 0},
    // Valve Steam Deck (Jupiter)
    {{DMI_EXACT_MATCH(DMI_SYS_VENDOR, "Valve"), DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "Jupiter"),
      DMI_EXACT_MATCH(DMI_PRODUCT_VERSION, "1")},
     1280, 800, 3328},
    // Valve Steam Deck (Galileo)
    {{DMI_EXACT_MATCH(DMI_SYS_VENDOR, "Valve"), DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "Galileo"),
      DMI_EXACT_MATCH(DMI_PRODUCT_VERSION, "1")},
     1280, 800, 3328},
};

static bool text_equal(const char *a, const char *b) {
    while (*a != 0 && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static bool text_contains(const char *haystack, const char *needle) {
    for (; *haystack != 0; haystack++) {
        size_t i = 0;
        while (needle[i] != 0 && haystack[i] == needle[i]) {
            i++;
        }
        if (needle[i] == 0) {
            return true;
        }
    }
    return false;
}

// As Linux 7.3's dmi_matches(): every match must hold, and a field the
// firmware does not give matches nothing.
static bool dmi_matches(const struct dmi_match *matches) {
    for (size_t i = 0; i < DMI_MATCHES_MAX && matches[i].text != NULL; i++) {
        const struct dmi_match *m = &matches[i];
        const char *value =
            fwtables_smbios_string(dmi_fields[m->field].type, dmi_fields[m->field].offset);
        if (value == NULL) {
            return false;
        }
        if (m->exact ? !text_equal(value, m->text) : !text_contains(value, m->text)) {
            return false;
        }
    }
    return true;
}

void panel_fix_framebuffer(struct handoff_framebuffer *fb) {
    for (size_t i = 0; i < sizeof(swap_quirks) / sizeof(swap_quirks[0]); i++) {
        const struct swap_quirk *q = &swap_quirks[i];
        if (!dmi_matches(q->matches)) {
            continue;
        }
        // As Linux's, the first quirk that matches decides.
        if (q->width != 0 && (fb->width != q->width || fb->height != q->height)) {
            return;
        }
        uint32_t width = fb->height;
        fb->height = fb->width;
        fb->width = width;
        fb->pitch = q->pitch != 0 ? q->pitch : width * (fb->bpp / 8);
        if ((uint64_t)fb->pitch * fb->height > fb->size) {
            fb->size = (uint64_t)fb->pitch * fb->height;
        }
        printf("This machine's firmware gives its portrait panel as landscape: the framebuffer "
               "is %ux%u, pitch %u\n", fb->width, fb->height, fb->pitch);
        return;
    }
}
