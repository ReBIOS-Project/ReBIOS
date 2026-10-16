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
#include <virt/edid.h>

#define REFRESH_HZ 60

// Reduced blanking v2 (VESA CVT 1.2, 3.4.3 and Table 5-4): fixed horizontal
// blanking, sync, and front porch, an 8-line vertical sync, and a 6-line back
// porch, after at least 460 us of vertical blanking.
#define H_BLANK 80
#define H_SYNC 32
#define H_FRONT_PORCH 8
#define V_SYNC 8
#define V_BACK_PORCH 6
#define V_FRONT_PORCH_MIN 1
#define V_BLANK_MIN_US 460

// What an 18-byte detailed timing descriptor (E-EDID A2, Table 3.21) holds.
#define DTD_ACTIVE_MAX 4095
#define DTD_CLOCK_MAX 65535
#define DTD_V_FRONT_PORCH_MAX 63

// "REB", in five bits a letter from 'A' as 1 (E-EDID A2, 3.4.1), a code the
// UEFI PNP ID registry has not assigned.
#define MANUFACTURER_ID 0x48a2

// The sRGB primaries and white point, as the colour characteristics bytes
// (E-EDID A2, 3.7) give them.
static const uint8_t srgb[10] = {0xee, 0x91, 0xa3, 0x54, 0x4c, 0x99, 0x26, 0x0f, 0x50, 0x54};

static void display_descriptor(uint8_t *d, uint8_t tag) {
    memset(d, 0, 18);
    d[3] = tag;
}

static void preferred_timing(uint8_t *d, uint32_t width, uint32_t height, uint32_t clock,
                             uint32_t v_blank) {
    uint32_t v_front = v_blank - V_SYNC - V_BACK_PORCH;
    if (v_front > DTD_V_FRONT_PORCH_MAX) {
        v_front = DTD_V_FRONT_PORCH_MAX;
    }
    d[0] = (uint8_t)clock;
    d[1] = (uint8_t)(clock >> 8);
    d[2] = (uint8_t)width;
    d[3] = H_BLANK;
    d[4] = (uint8_t)((width >> 8) << 4 | H_BLANK >> 8);
    d[5] = (uint8_t)height;
    d[6] = (uint8_t)v_blank;
    d[7] = (uint8_t)((height >> 8) << 4 | v_blank >> 8);
    d[8] = H_FRONT_PORCH;
    d[9] = H_SYNC;
    d[10] = (uint8_t)((v_front & 0xf) << 4 | (V_SYNC & 0xf));
    d[11] = (uint8_t)((H_FRONT_PORCH >> 8) << 6 | (H_SYNC >> 8) << 4 | (v_front >> 4) << 2
                      | V_SYNC >> 4);
    // Digital separate sync, the horizontal one positive, the vertical one
    // negative, as for reduced blanking.
    d[17] = 0x1a;
}

bool edid_synthesize(uint8_t *edid, uint32_t width, uint32_t height) {
    if (width == 0 || height == 0 || width > DTD_ACTIVE_MAX || height > DTD_ACTIVE_MAX) {
        return false;
    }
    // CVT 1.2, 5.4, steps 8 to 13.
    uint32_t v_blank = (uint32_t)((uint64_t)V_BLANK_MIN_US * REFRESH_HZ * height
                                  / (1000000 - V_BLANK_MIN_US * REFRESH_HZ)) + 1;
    if (v_blank < V_FRONT_PORCH_MIN + V_SYNC + V_BACK_PORCH) {
        v_blank = V_FRONT_PORCH_MIN + V_SYNC + V_BACK_PORCH;
    }
    uint64_t total_h = width + H_BLANK;
    uint64_t clock_khz = REFRESH_HZ * total_h * (height + v_blank) / 1000;
    uint64_t clock = clock_khz / 10;
    if (clock == 0 || clock > DTD_CLOCK_MAX || v_blank > DTD_ACTIVE_MAX) {
        return false;
    }

    memset(edid, 0, EDID_BLOCK_SIZE);
    static const uint8_t header[8] = {0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00};
    memcpy(edid, header, sizeof(header));
    edid[0x08] = MANUFACTURER_ID >> 8;
    edid[0x09] = MANUFACTURER_ID & 0xff;
    edid[0x0a] = 0x01;
    // Made in 2026, its week not given.
    edid[0x11] = 2026 - 1990;
    edid[0x12] = 1;
    edid[0x13] = 4;
    // Digital, 8 bits per primary.
    edid[0x14] = 0xa0;
    // The aspect ratio in place of a size, landscape or portrait.
    if (width >= height) {
        uint32_t ratio = (width * 100 + height / 2) / height;
        edid[0x15] = (uint8_t)(ratio - 99 > 255 ? 255 : ratio - 99);
    } else {
        uint32_t ratio = (height * 100 + width / 2) / width;
        edid[0x16] = (uint8_t)(ratio - 99 > 255 ? 255 : ratio - 99);
    }
    // Gamma 2.2.
    edid[0x17] = 120;
    // sRGB, and a preferred timing in the display's native format and rate.
    edid[0x18] = 0x06;
    memcpy(edid + 0x19, srgb, sizeof(srgb));
    // The established timings the display's size takes in.
    if (width >= 640 && height >= 480) {
        edid[0x23] |= 0x20;
    }
    if (width >= 800 && height >= 600) {
        edid[0x23] |= 0x01;
    }
    if (width >= 1024 && height >= 768) {
        edid[0x24] |= 0x08;
    }
    for (unsigned i = 0x26; i < 0x36; i++) {
        edid[i] = 0x01;
    }

    preferred_timing(edid + 0x36, width, height, (uint32_t)clock, v_blank);

    uint8_t *range = edid + 0x48;
    display_descriptor(range, 0xfd);
    uint64_t h_khz = (clock_khz + total_h - 1) / total_h;
    uint64_t max_clock = (clock_khz + 9999) / 10000;
    range[5] = 50;
    range[6] = 75;
    range[7] = 15;
    range[8] = (uint8_t)(h_khz < 31 ? 31 : h_khz > 255 ? 255 : h_khz);
    range[9] = (uint8_t)(max_clock > 255 ? 255 : max_clock);
    // Range limits only.
    range[10] = 0x01;
    range[11] = 0x0a;
    memset(range + 12, ' ', 6);

    uint8_t *name = edid + 0x5a;
    display_descriptor(name, 0xfc);
    static const char product[] = "ReBIOS VGA";
    memcpy(name + 5, product, sizeof(product) - 1);
    name[5 + sizeof(product) - 1] = 0x0a;
    memset(name + 5 + sizeof(product), ' ', 13 - sizeof(product));

    display_descriptor(edid + 0x6c, 0x10);

    uint8_t sum = 0;
    for (unsigned i = 0; i + 1 < EDID_BLOCK_SIZE; i++) {
        sum = (uint8_t)(sum + edid[i]);
    }
    edid[EDID_BLOCK_SIZE - 1] = (uint8_t)-sum;
    return true;
}
