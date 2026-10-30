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
#include <loader/panel.h>
#include <handoff.h>
#include <lib/log.h>
#include <loader/fwtables.h>

// The SMBIOS strings that Linux 7.3's DMI quirks match on
// (drivers/firmware/dmi_scan.c, dmi_decode()), by structure type and offset:
// DMTF DSP0134 3.9.0, Tables 6, 10, and 13.
#define DMI_BIOS_DATE 0
#define DMI_SYS_VENDOR 1
#define DMI_PRODUCT_NAME 2
#define DMI_PRODUCT_VERSION 3
#define DMI_BOARD_VENDOR 4
#define DMI_BOARD_NAME 5
#define DMI_FIELD_COUNT 6

static const struct {
    uint8_t type;
    uint8_t offset;
} dmi_fields[DMI_FIELD_COUNT] = {
    [DMI_BIOS_DATE] = { 0, 0x08 },
    [DMI_SYS_VENDOR] = { 1, 0x04 },
    [DMI_PRODUCT_NAME] = { 1, 0x05 },
    [DMI_PRODUCT_VERSION] = { 1, 0x06 },
    [DMI_BOARD_VENDOR] = { 2, 0x04 },
    [DMI_BOARD_NAME] = { 2, 0x05 },
};

#define DMI_MATCHES_MAX 4

// A field's string, equal to text where exact, else holding it.
struct dmi_match {
    uint8_t field;
    bool exact;
    const char *text;
};

#define DMI_EXACT_MATCH(field, text) { field, true, text }
#define DMI_MATCH(field, text) { field, false, text }

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
    {
        {
            DMI_EXACT_MATCH(DMI_SYS_VENDOR, "LENOVO"),
            DMI_EXACT_MATCH(DMI_PRODUCT_VERSION, "Lenovo ideapad D330-10IGM")
        },
        0, 0, 0
    },
    // Lenovo IdeaPad Duet 3 10IGL5 and 10IGL5-LTE with 1200x1920 portrait
    // screen
    {
        {
            DMI_EXACT_MATCH(DMI_SYS_VENDOR, "LENOVO"),
            DMI_MATCH(DMI_PRODUCT_VERSION, "IdeaPad Duet 3 10IGL5")
        },
        0, 0, 0
    },
    // Valve Steam Deck (Jupiter)
    {
        {
            DMI_EXACT_MATCH(DMI_SYS_VENDOR, "Valve"),
            DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "Jupiter"),
            DMI_EXACT_MATCH(DMI_PRODUCT_VERSION, "1")
        },
        1280, 800, 3328
    },
    // Valve Steam Deck (Galileo)
    {
        {
            DMI_EXACT_MATCH(DMI_SYS_VENDOR, "Valve"),
            DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "Galileo"),
            DMI_EXACT_MATCH(DMI_PRODUCT_VERSION, "1")
        },
        1280, 800, 3328
    },
};

// Panels mounted turned in their casing, the panel's left or right side at the
// casing's top, as Linux 7.3 lists them (drivers/gpu/drm/
// drm_panel_orientation_quirks.c, orientation_data[], Copyright (C) 2017 Hans
// de Goede <hdegoede@redhat.com>, MIT), but for the models with no VT-d (Bay
// Trail and Cherry Trail). A quirk applies only to a panel of its size, and
// where it lists BIOS release dates, as its strings are generic, only to
// firmware of one of those dates.
#define LEFT_UP 0
#define RIGHT_UP 1

struct orientation_quirk {
    struct dmi_match matches[DMI_MATCHES_MAX];
    uint16_t width;
    uint16_t height;
    const char *const *bios_dates;
    uint8_t orientation;
};

static const char *const gpd_micropc_dates[] = { "04/26/2019", NULL };
static const char *const gpd_onemix2s_dates[] = {
    "05/21/2018", "10/26/2018", "03/04/2019", NULL
};
static const char *const gpd_pocket2_dates[] = {
    "06/28/2018", "08/28/2018", "12/07/2018", NULL
};
static const char *const gpd_win2_dates[] = {
    "12/07/2017", "05/24/2018", "06/29/2018", NULL
};
static const char *const onegx1_pro_dates[] = { "12/17/2020", NULL };

static const struct orientation_quirk orientation_quirks[] = {
    // Anbernic Win600
    {
        {
            DMI_EXACT_MATCH(DMI_BOARD_VENDOR, "Anbernic"),
            DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "Win600")
        },
        720, 1280, NULL, RIGHT_UP
    },
    // AYA NEO AYANEO 2/2S
    {
        {
            DMI_EXACT_MATCH(DMI_SYS_VENDOR, "AYANEO"),
            DMI_MATCH(DMI_PRODUCT_NAME, "AYANEO 2")
        },
        1200, 1920, NULL, RIGHT_UP
    },
    // AYA NEO 2021
    {
        {
            DMI_EXACT_MATCH(DMI_SYS_VENDOR, "AYADEVICE"),
            DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "AYA NEO 2021")
        },
        800, 1280, NULL, RIGHT_UP
    },
    // AYA NEO AIR
    {
        {
            DMI_EXACT_MATCH(DMI_SYS_VENDOR, "AYANEO"),
            DMI_MATCH(DMI_PRODUCT_NAME, "AIR")
        },
        1080, 1920, NULL, LEFT_UP
    },
    // AYA NEO Flip DS Bottom Screen
    {
        {
            DMI_EXACT_MATCH(DMI_SYS_VENDOR, "AYANEO"),
            DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "FLIP DS")
        },
        640, 960, NULL, LEFT_UP
    },
    // AYA NEO Flip KB/DS Top Screen
    {
        {
            DMI_EXACT_MATCH(DMI_SYS_VENDOR, "AYANEO"),
            DMI_MATCH(DMI_PRODUCT_NAME, "FLIP")
        },
        1080, 1920, NULL, LEFT_UP
    },
    // AYA NEO Founder
    {
        {
            DMI_EXACT_MATCH(DMI_SYS_VENDOR, "AYA NEO"),
            DMI_MATCH(DMI_PRODUCT_NAME, "AYA NEO Founder")
        },
        800, 1280, NULL, RIGHT_UP
    },
    // AYA NEO GEEK
    {
        {
            DMI_EXACT_MATCH(DMI_SYS_VENDOR, "AYANEO"),
            DMI_MATCH(DMI_PRODUCT_NAME, "GEEK")
        },
        800, 1280, NULL, RIGHT_UP
    },
    // AYA NEO NEXT
    {
        {
            DMI_EXACT_MATCH(DMI_BOARD_VENDOR, "AYANEO"),
            DMI_MATCH(DMI_BOARD_NAME, "NEXT")
        },
        800, 1280, NULL, RIGHT_UP
    },
    // AYA NEO KUN
    {
        {
            DMI_EXACT_MATCH(DMI_BOARD_VENDOR, "AYANEO"),
            DMI_MATCH(DMI_BOARD_NAME, "KUN")
        },
        1600, 2560, NULL, RIGHT_UP
    },
    // AYA NEO SLIDE
    {
        {
            DMI_EXACT_MATCH(DMI_SYS_VENDOR, "AYANEO"),
            DMI_MATCH(DMI_PRODUCT_NAME, "SLIDE")
        },
        1080, 1920, NULL, LEFT_UP
    },
    // AYN Loki Max
    {
        {
            DMI_EXACT_MATCH(DMI_SYS_VENDOR, "ayn"),
            DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "Loki Max")
        },
        1080, 1920, NULL, LEFT_UP
    },
    // AYN Loki Zero
    {
        {
            DMI_EXACT_MATCH(DMI_SYS_VENDOR, "ayn"),
            DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "Loki Zero")
        },
        1080, 1920, NULL, LEFT_UP
    },
    // Dynabook K50
    {
        {
            DMI_EXACT_MATCH(DMI_SYS_VENDOR, "Dynabook Inc."),
            DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "dynabook K50/FR")
        },
        800, 1280, NULL, LEFT_UP
    },
    // GPD MicroPC (generic strings, also match on bios date)
    {
        {
            DMI_EXACT_MATCH(DMI_SYS_VENDOR, "Default string"),
            DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "Default string"),
            DMI_EXACT_MATCH(DMI_BOARD_VENDOR, "Default string"),
            DMI_EXACT_MATCH(DMI_BOARD_NAME, "Default string")
        },
        720, 1280, gpd_micropc_dates, RIGHT_UP
    },
    // GPD MicroPC (later BIOS versions with proper DMI strings)
    {
        {
            DMI_EXACT_MATCH(DMI_SYS_VENDOR, "GPD"),
            DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "MicroPC")
        },
        720, 1280, NULL, RIGHT_UP
    },
    // GPD Win Max
    {
        {
            DMI_EXACT_MATCH(DMI_SYS_VENDOR, "GPD"),
            DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "G1619-01")
        },
        800, 1280, NULL, RIGHT_UP
    },
    // GPD Pocket 2 (generic strings, also match on bios date)
    {
        {
            DMI_EXACT_MATCH(DMI_SYS_VENDOR, "Default string"),
            DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "Default string"),
            DMI_EXACT_MATCH(DMI_BOARD_VENDOR, "Default string"),
            DMI_EXACT_MATCH(DMI_BOARD_NAME, "Default string")
        },
        1200, 1920, gpd_pocket2_dates, RIGHT_UP
    },
    // GPD Win 2 (too generic strings, also match on bios date)
    {
        {
            DMI_EXACT_MATCH(DMI_SYS_VENDOR, "Default string"),
            DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "Default string"),
            DMI_EXACT_MATCH(DMI_BOARD_VENDOR, "Default string"),
            DMI_EXACT_MATCH(DMI_BOARD_NAME, "Default string")
        },
        720, 1280, gpd_win2_dates, RIGHT_UP
    },
    // GPD Win 2 (correct DMI strings)
    {
        {
            DMI_EXACT_MATCH(DMI_SYS_VENDOR, "GPD"),
            DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "WIN2")
        },
        720, 1280, NULL, RIGHT_UP
    },
    // GPD Win 3
    {
        {
            DMI_EXACT_MATCH(DMI_SYS_VENDOR, "GPD"),
            DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "G1618-03")
        },
        720, 1280, NULL, RIGHT_UP
    },
    // GPD Win Mini
    {
        {
            DMI_EXACT_MATCH(DMI_SYS_VENDOR, "GPD"),
            DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "G1617-01")
        },
        1080, 1920, NULL, RIGHT_UP
    },
    // Lenovo Ideapad D330-10IGM (HD)
    {
        {
            DMI_EXACT_MATCH(DMI_SYS_VENDOR, "LENOVO"),
            DMI_EXACT_MATCH(DMI_PRODUCT_VERSION, "Lenovo ideapad D330-10IGM")
        },
        800, 1280, NULL, RIGHT_UP
    },
    // Lenovo Ideapad D330-10IGM (FHD)
    {
        {
            DMI_EXACT_MATCH(DMI_SYS_VENDOR, "LENOVO"),
            DMI_EXACT_MATCH(DMI_PRODUCT_VERSION, "Lenovo ideapad D330-10IGM")
        },
        1200, 1920, NULL, RIGHT_UP
    },
    // Lenovo Ideapad D330-10IGL (HD)
    {
        {
            DMI_EXACT_MATCH(DMI_SYS_VENDOR, "LENOVO"),
            DMI_EXACT_MATCH(DMI_PRODUCT_VERSION, "Lenovo ideapad D330-10IGL")
        },
        800, 1280, NULL, RIGHT_UP
    },
    // Lenovo IdeaPad Duet 3 10IGL5
    {
        {
            DMI_EXACT_MATCH(DMI_SYS_VENDOR, "LENOVO"),
            DMI_EXACT_MATCH(DMI_PRODUCT_VERSION, "IdeaPad Duet 3 10IGL5")
        },
        1200, 1920, NULL, RIGHT_UP
    },
    // Lenovo Legion Go 8APU1
    {
        {
            DMI_EXACT_MATCH(DMI_SYS_VENDOR, "LENOVO"),
            DMI_EXACT_MATCH(DMI_PRODUCT_VERSION, "Legion Go 8APU1")
        },
        1600, 2560, NULL, LEFT_UP
    },
    // Nanote UMPC-01
    {
        {
            DMI_MATCH(DMI_SYS_VENDOR, "RWC CO.,LTD"),
            DMI_MATCH(DMI_PRODUCT_NAME, "UMPC-01")
        },
        1200, 1920, NULL, RIGHT_UP
    },
    // OneGX1 Pro
    {
        {
            DMI_EXACT_MATCH(DMI_SYS_VENDOR, "SYSTEM_MANUFACTURER"),
            DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "SYSTEM_PRODUCT_NAME"),
            DMI_EXACT_MATCH(DMI_PRODUCT_VERSION, "Default string")
        },
        1200, 1920, onegx1_pro_dates, RIGHT_UP
    },
    // OneXPlayer
    {
        {
            DMI_EXACT_MATCH(
                DMI_SYS_VENDOR, "ONE-NETBOOK TECHNOLOGY CO., LTD."
            ),
            DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "ONE XPLAYER")
        },
        1600, 2560, NULL, LEFT_UP
    },
    // OneXPlayer Mini (Intel)
    {
        {
            DMI_EXACT_MATCH(
                DMI_SYS_VENDOR, "ONE-NETBOOK TECHNOLOGY CO., LTD."
            ),
            DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "ONE XPLAYER")
        },
        1200, 1920, NULL, LEFT_UP
    },
    // OrangePi Neo
    {
        {
            DMI_EXACT_MATCH(DMI_SYS_VENDOR, "OrangePi"),
            DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "NEO-01")
        },
        1200, 1920, NULL, RIGHT_UP
    },
    // Samsung GalaxyBook 10.6
    {
        {
            DMI_EXACT_MATCH(DMI_SYS_VENDOR, "SAMSUNG ELECTRONICS CO., LTD."),
            DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "Galaxy Book 10.6")
        },
        1280, 1920, NULL, RIGHT_UP
    },
    // Valve Steam Deck (Jupiter)
    {
        {
            DMI_EXACT_MATCH(DMI_SYS_VENDOR, "Valve"),
            DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "Jupiter"),
            DMI_EXACT_MATCH(DMI_PRODUCT_VERSION, "1")
        },
        800, 1280, NULL, RIGHT_UP
    },
    // Valve Steam Deck (Galileo)
    {
        {
            DMI_EXACT_MATCH(DMI_SYS_VENDOR, "Valve"),
            DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "Galileo"),
            DMI_EXACT_MATCH(DMI_PRODUCT_VERSION, "1")
        },
        800, 1280, NULL, RIGHT_UP
    },
    // ZOTAC Gaming Zone
    {
        {
            DMI_EXACT_MATCH(DMI_SYS_VENDOR, "ZOTAC"),
            DMI_EXACT_MATCH(DMI_BOARD_NAME, "G0A1W")
        },
        1080, 1920, NULL, LEFT_UP
    },
    // One Mix 2S (generic strings, also match on bios date)
    {
        {
            DMI_EXACT_MATCH(DMI_SYS_VENDOR, "Default string"),
            DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "Default string"),
            DMI_EXACT_MATCH(DMI_BOARD_VENDOR, "Default string"),
            DMI_EXACT_MATCH(DMI_BOARD_NAME, "Default string")
        },
        1200, 1920, gpd_onemix2s_dates, RIGHT_UP
    },
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

static const char *dmi_string(uint8_t field) {
    return fwtables_smbios_string(
        dmi_fields[field].type, dmi_fields[field].offset
    );
}

// As Linux 7.3's dmi_matches(): every match must hold, and a field the
// firmware does not give matches nothing.
static bool dmi_matches(const struct dmi_match *matches) {
    for (size_t i = 0; i < DMI_MATCHES_MAX && matches[i].text != NULL; i++) {
        const struct dmi_match *m = &matches[i];
        const char *value = dmi_string(m->field);
        if (value == NULL) {
            return false;
        }
        if (m->exact ? !text_equal(value, m->text)
          : !text_contains(value, m->text)) {
            return false;
        }
    }
    return true;
}

void panel_fix_framebuffer(struct handoff_framebuffer *fb) {
    for (size_t i = 0; i < sizeof(swap_quirks) / sizeof(struct swap_quirk);
         i++) {
        const struct swap_quirk *q = &swap_quirks[i];
        if (!dmi_matches(q->matches)) {
            continue;
        }
        // As Linux's, the first quirk that matches decides.
        if (q->width != 0
         && (fb->width != q->width || fb->height != q->height)) {
            return;
        }
        uint32_t width = fb->height;
        fb->height = fb->width;
        fb->width = width;
        fb->pitch = q->pitch != 0 ? q->pitch : width * (fb->bpp / 8);
        if ((uint64_t)fb->pitch * fb->height > fb->size) {
            fb->size = (uint64_t)fb->pitch * fb->height;
        }
        printf(
            "This machine's firmware gives its portrait panel as landscape: "
            "the framebuffer is %ux%u, pitch %u\n",
            fb->width, fb->height, fb->pitch
        );
        return;
    }
}

static bool date_listed(const char *const *dates) {
    const char *date = dmi_string(DMI_BIOS_DATE);
    if (date == NULL) {
        return false;
    }
    for (size_t i = 0; dates[i] != NULL; i++) {
        if (text_equal(date, dates[i])) {
            return true;
        }
    }
    return false;
}

// As Linux 7.3's drm_get_panel_orientation_quirk(), turning the picture as its
// efifb.c turns the console: clockwise on a right side up panel, anticlockwise
// on a left side up one.
uint8_t panel_rotation(const struct handoff_framebuffer *fb) {
    for (size_t i = 0;
         i < sizeof(orientation_quirks) / sizeof(struct orientation_quirk);
         i++) {
        const struct orientation_quirk *q = &orientation_quirks[i];
        if (!dmi_matches(q->matches) || fb->width != q->width
         || fb->height != q->height) {
            continue;
        }
        if (q->bios_dates != NULL && !date_listed(q->bios_dates)) {
            continue;
        }
        bool right_up = q->orientation == RIGHT_UP;
        printf(
            "The panel is mounted with its %s side up, so its picture is "
            "turned %s\n",
            right_up ? "right" : "left",
            right_up ? "clockwise" : "anticlockwise"
        );
        return right_up ? HANDOFF_ROTATE_90 : HANDOFF_ROTATE_270;
    }
    return HANDOFF_ROTATE_0;
}
