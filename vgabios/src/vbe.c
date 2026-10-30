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

// The VBE 3.0 functions (VBE Core Functions Standard 3.0) and VBE/DDC's
// (VBE/DDC Standard 1.1), on the hypervisor's Bochs display interface.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <vgabios.h>
#include <vgabios_info.h>

#define DISPI_INDEX 0x1ce
#define DISPI_DATA 0x1cf
#define DISPI_XRES 0x1
#define DISPI_YRES 0x2
#define DISPI_BPP 0x3
#define DISPI_ENABLE 0x4
#define DISPI_BANK 0x5
#define DISPI_VIRT_WIDTH 0x6
#define DISPI_VIRT_HEIGHT 0x7
#define DISPI_X_OFFSET 0x8
#define DISPI_Y_OFFSET 0x9
#define DISPI_VIDEO_MEMORY_64K 0xa
#define DISPI_ENABLED 0x01
#define DISPI_8BIT_DAC 0x20
#define DISPI_LFB_ENABLED 0x40
#define DISPI_NOCLEARMEM 0x80
#define DISPI_MAX_XRES 16000

#define STATUS_SUCCESS 0x004f
#define STATUS_FAILED 0x014f
#define STATUS_UNSUPPORTED 0x024f
#define STATUS_INVALID 0x034f

#define MODE_NUMBER 0x01ff
#define MODE_LFB 0x4000
#define MODE_KEEP 0x8000

#define BDA 0x0040
#define BDA_MODE 0x49
#define BDA_VIDEO_CONTROL 0x87
#define VIDEO_CONTROL_KEPT 0x80

#define INPUT_STATUS1 0x3da
#define STATUS1_RETRACE 0x08

#define ATTRIBUTES_SUPPORTED 0x0001
#define ATTRIBUTES_EXTENDED 0x0002
#define ATTRIBUTES_COLOR 0x0008
#define ATTRIBUTES_GRAPHICS 0x0010
#define ATTRIBUTES_LFB 0x0080
#define WINDOW_RELOCATABLE_RW 0x07
#define MEMORY_MODEL_PACKED 4
#define MEMORY_MODEL_DIRECT 6

struct resolution {
    uint16_t width;
    uint16_t height;
};

// Each in 8, 15, 16, 24, and 32 bits per pixel, then the display's own.
static const __seg_fs uint8_t depths[5] = { 8, 15, 16, 24, 32 };
static const __seg_fs struct resolution resolutions[] = {
    { 320, 200 }, { 640, 400 }, { 640, 480 }, { 800, 600 }, { 1024, 768 },
    { 1152, 864 }, { 1280, 720 }, { 1280, 768 }, { 1280, 800 }, { 1280, 960 },
    { 1280, 1024 }, { 1360, 768 }, { 1400, 1050 }, { 1440, 900 },
    { 1600, 900 }, { 1600, 1200 }, { 1680, 1050 }, { 1920, 1080 },
    { 1920, 1200 }, { 2048, 1152 }, { 2048, 1536 }, { 2560, 1080 },
    { 2560, 1440 }, { 2560, 1600 }, { 2880, 1800 }, { 3200, 1800 },
    { 3440, 1440 }, { 3840, 2160 }, { 3840, 2400 }, { 4096, 2160 },
};

#define RESOLUTION_COUNT (sizeof(resolutions) / sizeof(struct resolution))
#define MODE_COUNT (RESOLUTION_COUNT * 5 + 5)

// VBE 1.2's numbers where it has them, the others from 140h, and the display's
// resolution from 1F0h.
static const __seg_fs uint16_t mode_numbers[MODE_COUNT + 1] = {
    0x140, 0x10d, 0x10e, 0x10f, 0x144, 0x100, 0x146, 0x147, 0x148, 0x149,
    0x101, 0x110, 0x111, 0x112, 0x14e, 0x103, 0x113, 0x114, 0x115, 0x153,
    0x105, 0x116, 0x117, 0x118, 0x158, 0x159, 0x15a, 0x15b, 0x15c, 0x15d,
    0x15e, 0x15f, 0x160, 0x161, 0x162, 0x163, 0x164, 0x165, 0x166, 0x167,
    0x168, 0x169, 0x16a, 0x16b, 0x16c, 0x16d, 0x16e, 0x16f, 0x170, 0x171,
    0x107, 0x119, 0x11a, 0x11b, 0x176, 0x177, 0x178, 0x179, 0x17a, 0x17b,
    0x17c, 0x17d, 0x17e, 0x17f, 0x180, 0x181, 0x182, 0x183, 0x184, 0x185,
    0x186, 0x187, 0x188, 0x189, 0x18a, 0x18b, 0x18c, 0x18d, 0x18e, 0x18f,
    0x190, 0x191, 0x192, 0x193, 0x194, 0x195, 0x196, 0x197, 0x198, 0x199,
    0x19a, 0x19b, 0x19c, 0x19d, 0x19e, 0x19f, 0x1a0, 0x1a1, 0x1a2, 0x1a3,
    0x1a4, 0x1a5, 0x1a6, 0x1a7, 0x1a8, 0x1a9, 0x1aa, 0x1ab, 0x1ac, 0x1ad,
    0x1ae, 0x1af, 0x1b0, 0x1b1, 0x1b2, 0x1b3, 0x1b4, 0x1b5, 0x1b6, 0x1b7,
    0x1b8, 0x1b9, 0x1ba, 0x1bb, 0x1bc, 0x1bd, 0x1be, 0x1bf, 0x1c0, 0x1c1,
    0x1c2, 0x1c3, 0x1c4, 0x1c5, 0x1c6, 0x1c7, 0x1c8, 0x1c9, 0x1ca, 0x1cb,
    0x1cc, 0x1cd, 0x1ce, 0x1cf, 0x1d0, 0x1d1, 0x1d2, 0x1d3, 0x1d4, 0x1d5,
    0x1f0, 0x1f1, 0x1f2, 0x1f3, 0x1f4, 0xffff,
};

static const __seg_fs char oem_string[] = "ReBIOS VBE";
static const __seg_fs char vendor_name[] = "ReBIOS";
static const __seg_fs char product_name[] = "ReBIOS VGA";
static const __seg_fs char product_revision[] = "";

// Each depth's red, green, blue, and reserved field sizes and positions.
static const __seg_fs uint8_t color_fields[5][8] = {
    { 0, 0, 0, 0, 0, 0, 0, 0 },
    { 5, 10, 5, 5, 5, 0, 1, 15 },
    { 5, 11, 6, 5, 5, 0, 0, 0 },
    { 8, 16, 8, 8, 8, 0, 0, 0 },
    { 8, 16, 8, 8, 8, 0, 8, 24 },
};

// Function 04h's buffer holds the states it is asked for in this order: the
// controller's registers, the BIOS data, the DAC, and the Bochs interface's
// registers, each taking this many 64-byte blocks.
static const __seg_fs uint8_t state_blocks[4] = { 2, 1, 13, 1 };

// The Bochs interface registers function 04h keeps, in the order they are
// restored, the enable register's write needing the depth and resolution and
// clearing what follows it.
static const __seg_fs uint8_t state_dispi[8] = {
    DISPI_BPP, DISPI_XRES, DISPI_YRES, DISPI_ENABLE, DISPI_VIRT_WIDTH,
    DISPI_X_OFFSET, DISPI_Y_OFFSET, DISPI_BANK,
};

struct mode {
    uint16_t number;
    uint16_t width;
    uint16_t height;
    uint8_t bpp;
};

static bool present(void) {
    return vgabios_info.flags & VGABIOS_INFO_DISPI;
}

static void dispi_write(uint16_t index, uint16_t value) {
    outw(DISPI_INDEX, index);
    outw(DISPI_DATA, value);
}

static uint16_t dispi_read(uint16_t index) {
    outw(DISPI_INDEX, index);
    return inw(DISPI_DATA);
}

static size_t vram_size(void) {
    return (uint32_t)dispi_read(DISPI_VIDEO_MEMORY_64K) << 16;
}

static size_t depth_index(uint8_t bpp) {
    for (size_t i = 0; i < 5; i++) {
        if (depths[i] == bpp) {
            return i;
        }
    }
    return 0;
}

static uint8_t bytes_per_pixel(uint8_t bpp) {
    return (uint8_t)((bpp + 7) / 8);
}

// The width of the display's own resolution, in the multiples of 8 pixels the
// interface takes.
static uint16_t native_width(void) {
    return vgabios_info.max_width & (uint16_t)~(uint32_t)7;
}

static bool mode_get(uint16_t number, struct mode *m) {
    for (size_t k = 0; k < MODE_COUNT; k++) {
        if (mode_numbers[k] != number) {
            continue;
        }
        m->number = number;
        if (k < RESOLUTION_COUNT * 5) {
            m->width = resolutions[k / 5].width;
            m->height = resolutions[k / 5].height;
            m->bpp = depths[k % 5];
        } else {
            m->width = native_width();
            m->height = vgabios_info.max_height;
            m->bpp = depths[k - RESOLUTION_COUNT * 5];
        }
        return true;
    }
    return false;
}

static bool native_listed(void) {
    for (size_t i = 0; i < RESOLUTION_COUNT; i++) {
        if (resolutions[i].width == native_width()
         && resolutions[i].height == vgabios_info.max_height) {
            return true;
        }
    }
    return false;
}

// Up to the display's resolution, in the video memory there is.
static bool mode_supported(const struct mode *m) {
    if (m->width == 0 || m->height == 0 || m->width > vgabios_info.max_width
     || m->height > vgabios_info.max_height) {
        return false;
    }
    if (m->number >= 0x1f0 && native_listed()) {
        return false;
    }
    size_t bytes = (uint32_t)m->width * bytes_per_pixel(m->bpp) * m->height;
    return bytes <= vram_size();
}

static bool mode_find(uint16_t number, struct mode *m) {
    return mode_get(number, m) && mode_supported(m);
}

static uintptr_t lfb_address(void) {
    return vgabios_info.lfb;
}

static void wait_retrace(void) {
    while ((inb(INPUT_STATUS1) & STATUS1_RETRACE) == 0) {
    }
}

// The VbeInfoBlock's fields up to OemProductRevPtr, the rest being 0 but for
// the OEM area of VBE 2.0 blocks, which strings could go in.
struct __attribute__((packed)) controller_info_block {
    char signature[4];
    uint16_t version;
    uint32_t oem_string;
    uint32_t capabilities;
    uint32_t mode_list;
    uint16_t total_memory;
    uint16_t software_revision;
    uint32_t vendor_name;
    uint32_t product_name;
    uint32_t product_revision;
};

_Static_assert(
    sizeof(struct controller_info_block) == 0x22, "VbeInfoBlock layout"
);

// The ModeInfoBlock's fields up to MaxPixelClock, the rest being 0.
struct __attribute__((packed)) mode_info_block {
    uint16_t attributes;
    uint8_t window_a_attributes;
    uint8_t window_b_attributes;
    uint16_t window_granularity;
    uint16_t window_size;
    uint16_t window_a_segment;
    uint16_t window_b_segment;
    uint32_t window_function;
    uint16_t bytes_per_line;
    uint16_t width;
    uint16_t height;
    uint8_t char_width;
    uint8_t char_height;
    uint8_t planes;
    uint8_t bpp;
    uint8_t banks;
    uint8_t memory_model;
    uint8_t bank_size;
    uint8_t image_pages;
    uint8_t reserved;
    uint8_t fields[8];
    uint8_t direct_color;
    uint32_t lfb;
    uint32_t reserved2;
    uint16_t reserved3;
    uint16_t linear_bytes_per_line;
    uint8_t banked_image_pages;
    uint8_t linear_image_pages;
    uint8_t linear_fields[8];
    uint32_t max_pixel_clock;
};

_Static_assert(sizeof(struct mode_info_block) == 0x42, "ModeInfoBlock layout");

// A far pointer to the ROM's offset.
static uint32_t far_pointer(uint16_t offset) {
    return (uint32_t)code_segment() << 16 | offset;
}

// The mode numbers the Reserved area of a VbeInfoBlock holds, after the fields
// and before the terminator.
#define MODE_LIST_MAX 110

// Lists the modes that fit, of the uncommon depths or of the others, after the
// count already at segment:list.
static unsigned int list_modes(
    uint16_t segment, uint16_t list, size_t count, bool uncommon
) {
    for (size_t i = 0; i < MODE_COUNT && count < MODE_LIST_MAX; i++) {
        struct mode m;
        if (mode_find(mode_numbers[i], &m)
         && (m.bpp == 15 || m.bpp == 24) == uncommon) {
            pokew(segment, (uint16_t)(list + 2 * count), mode_numbers[i]);
            count++;
        }
    }
    return count;
}

static void controller_info(struct vgabios_regs *r) {
    bool vbe2 = peekw(r->es, r->di) == ('V' | 'B' << 8)
        && peekw(r->es, (uint16_t)(r->di + 2)) == ('E' | '2' << 8);
    struct controller_info_block b;
    local_fill(&b, 0, sizeof(struct controller_info_block));
    b.signature[0] = 'V';
    b.signature[1] = 'E';
    b.signature[2] = 'S';
    b.signature[3] = 'A';
    b.version = 0x0300;
    b.oem_string = far_pointer((uint16_t)(uintptr_t)oem_string);
    // The DAC can take 8 bits per primary.
    b.capabilities = 0x00000001;
    b.total_memory = dispi_read(DISPI_VIDEO_MEMORY_64K);
    if (vbe2) {
        b.vendor_name = far_pointer((uint16_t)(uintptr_t)vendor_name);
        b.product_name = far_pointer((uint16_t)(uintptr_t)product_name);
        b.product_revision = far_pointer(
            (uint16_t)(uintptr_t)product_revision
        );
    }
    far_fill8(r->es, r->di, 0, vbe2 ? 512 : 256);
    // The mode list goes in the block, as some callers see nothing else of
    // real-mode memory: Windows on x86-64, through its emulator. Should more
    // modes fit than the block holds, 15 and 24 bpp ones are left out first.
    uint16_t list = (uint16_t)(r->di + sizeof(struct controller_info_block));
    size_t count = list_modes(r->es, list, 0, false);
    count = list_modes(r->es, list, count, true);
    pokew(r->es, (uint16_t)(list + 2 * count), 0xffff);
    b.mode_list = (uint32_t)r->es << 16 | list;
    far_copy(
        r->es, r->di, stack_segment(), (uint16_t)(uintptr_t)&b,
        sizeof(struct controller_info_block)
    );
    r->ax = STATUS_SUCCESS;
}

static void mode_info(struct vgabios_regs *r) {
    struct mode m;
    if (!mode_get(r->cx & MODE_NUMBER, &m)) {
        r->ax = STATUS_FAILED;
        return;
    }
    size_t bytes = bytes_per_pixel(m.bpp);
    uint32_t pitch = (uint32_t)m.width * bytes;
    uint32_t page = (pitch * m.height + 0xffff) & ~(uint32_t)0xffff;
    size_t pages = page != 0 ? vram_size() / page : 0;
    uint8_t images = pages == 0 ? 0 : (uint8_t)(pages > 256 ? 255 : pages - 1);
    struct mode_info_block b;
    local_fill(&b, 0, sizeof(struct mode_info_block));
    b.attributes = ATTRIBUTES_EXTENDED | ATTRIBUTES_COLOR
        | ATTRIBUTES_GRAPHICS;
    if (mode_supported(&m)) {
        b.attributes |= ATTRIBUTES_SUPPORTED;
    }
    b.lfb = lfb_address();
    if (b.lfb != 0) {
        b.attributes |= ATTRIBUTES_LFB;
    }
    b.window_a_attributes = WINDOW_RELOCATABLE_RW;
    b.window_granularity = 64;
    b.window_size = 64;
    b.window_a_segment = 0xa000;
    b.window_function = far_pointer((uint16_t)(uintptr_t)vbe_window_entry);
    b.bytes_per_line = (uint16_t)pitch;
    b.width = m.width;
    b.height = m.height;
    b.char_width = 8;
    b.char_height = 16;
    b.planes = 1;
    b.bpp = m.bpp;
    b.banks = 1;
    b.memory_model = m.bpp == 8 ? MEMORY_MODEL_PACKED : MEMORY_MODEL_DIRECT;
    b.image_pages = images;
    b.reserved = 1;
    const __seg_fs uint8_t *fields = color_fields[depth_index(m.bpp)];
    for (size_t i = 0; i < 8; i++) {
        b.fields[i] = fields[i];
        b.linear_fields[i] = fields[i];
    }
    // The reserved bits of a direct colour pixel are the application's.
    b.direct_color = fields[6] != 0 ? 0x02 : 0x00;
    b.linear_bytes_per_line = (uint16_t)pitch;
    b.banked_image_pages = images;
    b.linear_image_pages = images;
    far_fill8(r->es, r->di, 0, 256);
    far_copy(
        r->es, r->di, stack_segment(), (uint16_t)(uintptr_t)&b,
        sizeof(struct mode_info_block)
    );
    r->ax = STATUS_SUCCESS;
}

void vbe_disable(void) {
    if (present()) {
        dispi_write(DISPI_ENABLE, 0);
    }
}

static void set_mode(struct vgabios_regs *r) {
    uint16_t number = r->bx & MODE_NUMBER;
    bool keep = r->bx & MODE_KEEP;
    if (number < 0x100) {
        vgabios_set_mode((uint8_t)((number & 0x7f) | (keep ? 0x80 : 0)));
        r->ax = STATUS_SUCCESS;
        return;
    }
    struct mode m;
    bool lfb = r->bx & MODE_LFB;
    if (!mode_find(number, &m) || (lfb && lfb_address() == 0)) {
        r->ax = STATUS_FAILED;
        return;
    }
    // The DAC is back to 6 bits per primary.
    dispi_write(DISPI_ENABLE, 0);
    dispi_write(DISPI_BPP, m.bpp);
    dispi_write(DISPI_XRES, m.width);
    dispi_write(DISPI_YRES, m.height);
    dispi_write(DISPI_BANK, 0);
    dispi_write(
        DISPI_ENABLE,
        (uint16_t)(DISPI_ENABLED | (lfb ? DISPI_LFB_ENABLED : 0)
            | (keep ? DISPI_NOCLEARMEM : 0))
    );
    if (m.bpp == 8) {
        vgabios_load_palette_256();
    }
    uint8_t control = peekb(BDA, BDA_VIDEO_CONTROL)
        & (uint8_t)~VIDEO_CONTROL_KEPT;
    pokeb(
        BDA, BDA_VIDEO_CONTROL,
        (uint8_t)(control | (keep ? VIDEO_CONTROL_KEPT : 0))
    );
    r->ax = STATUS_SUCCESS;
}

static void current_mode(struct vgabios_regs *r) {
    uint16_t enable = dispi_read(DISPI_ENABLE);
    if ((enable & DISPI_ENABLED) == 0) {
        bool kept = peekb(BDA, BDA_VIDEO_CONTROL) & VIDEO_CONTROL_KEPT;
        r->bx = (uint16_t)(peekb(BDA, BDA_MODE) | (kept ? MODE_KEEP : 0));
        r->ax = STATUS_SUCCESS;
        return;
    }
    uint16_t width = dispi_read(DISPI_XRES);
    uint16_t height = dispi_read(DISPI_YRES);
    uint16_t bpp = dispi_read(DISPI_BPP);
    uint16_t number = 0;
    for (size_t k = 0; k < MODE_COUNT && number == 0; k++) {
        struct mode m;
        if (mode_get(mode_numbers[k], &m) && m.width == width
         && m.height == height && m.bpp == bpp) {
            number = m.number;
        }
    }
    r->bx = (uint16_t)(number
        | ((enable & DISPI_LFB_ENABLED) != 0 ? MODE_LFB : 0)
        | ((enable & DISPI_NOCLEARMEM) != 0 ? MODE_KEEP : 0));
    r->ax = STATUS_SUCCESS;
}

static uint16_t crtc_base(void) {
    return (inb(0x3cc) & 1) != 0 ? 0x3d4 : 0x3b4;
}

static void hardware_save(uint16_t seg, uint16_t at) {
    uint16_t crtc = crtc_base();
    pokeb(seg, at, inb(0x3cc));
    pokeb(seg, (uint16_t)(at + 1), inb(0x3c4));
    for (size_t i = 0; i < 5; i++) {
        outb(0x3c4, i);
        pokeb(seg, (uint16_t)(at + 2 + i), inb(0x3c5));
    }
    outb(0x3c4, peekb(seg, (uint16_t)(at + 1)));
    pokeb(seg, (uint16_t)(at + 7), inb(crtc));
    for (size_t i = 0; i < 25; i++) {
        outb(crtc, i);
        pokeb(seg, (uint16_t)(at + 8 + i), inb((uint16_t)(crtc + 1)));
    }
    outb(crtc, peekb(seg, (uint16_t)(at + 7)));
    pokeb(seg, (uint16_t)(at + 33), inb(0x3ce));
    for (size_t i = 0; i < 9; i++) {
        outb(0x3ce, i);
        pokeb(seg, (uint16_t)(at + 34 + i), inb(0x3cf));
    }
    outb(0x3ce, peekb(seg, (uint16_t)(at + 33)));
    inb((uint16_t)(crtc + 6));
    pokeb(seg, (uint16_t)(at + 43), inb(0x3c0));
    for (size_t i = 0; i < 21; i++) {
        inb((uint16_t)(crtc + 6));
        outb(0x3c0, i);
        pokeb(seg, (uint16_t)(at + 44 + i), inb(0x3c1));
    }
    inb((uint16_t)(crtc + 6));
    outb(0x3c0, (uint8_t)(peekb(seg, (uint16_t)(at + 43)) | 0x20));
    inb((uint16_t)(crtc + 6));
    pokeb(seg, (uint16_t)(at + 65), inb(0x3ca));
}

static void hardware_restore(uint16_t seg, uint16_t at) {
    outb(0x3c2, peekb(seg, at));
    uint16_t crtc = crtc_base();
    for (size_t i = 0; i < 5; i++) {
        outb(0x3c4, i);
        outb(0x3c5, peekb(seg, (uint16_t)(at + 2 + i)));
    }
    outb(0x3c4, peekb(seg, (uint16_t)(at + 1)));
    // Unprotected first, so that registers 0-7 take their values.
    outb(crtc, 0x11);
    outb((uint16_t)(crtc + 1), peekb(seg, (uint16_t)(at + 8 + 0x11)) & 0x7f);
    for (size_t i = 0; i < 25; i++) {
        outb(crtc, i);
        outb((uint16_t)(crtc + 1), peekb(seg, (uint16_t)(at + 8 + i)));
    }
    outb(crtc, peekb(seg, (uint16_t)(at + 7)));
    for (size_t i = 0; i < 9; i++) {
        outb(0x3ce, i);
        outb(0x3cf, peekb(seg, (uint16_t)(at + 34 + i)));
    }
    outb(0x3ce, peekb(seg, (uint16_t)(at + 33)));
    inb((uint16_t)(crtc + 6));
    for (size_t i = 0; i < 21; i++) {
        outb(0x3c0, i);
        outb(0x3c0, peekb(seg, (uint16_t)(at + 44 + i)));
    }
    outb(0x3c0, (uint8_t)(peekb(seg, (uint16_t)(at + 43)) | 0x20));
    inb((uint16_t)(crtc + 6));
    outb((uint16_t)(crtc + 6), peekb(seg, (uint16_t)(at + 65)));
}

static void bios_data_copy(uint16_t seg, uint16_t at, bool save) {
    for (size_t i = 0; i < 30; i++) {
        uint16_t bda = (uint16_t)(0x49 + i);
        if (save) {
            pokeb(seg, (uint16_t)(at + i), peekb(BDA, bda));
        } else {
            pokeb(BDA, bda, peekb(seg, (uint16_t)(at + i)));
        }
    }
    for (size_t i = 0; i < 7; i++) {
        uint16_t bda = (uint16_t)(0x84 + i);
        if (save) {
            pokeb(seg, (uint16_t)(at + 30 + i), peekb(BDA, bda));
        } else {
            pokeb(BDA, bda, peekb(seg, (uint16_t)(at + 30 + i)));
        }
    }
    // The vectors of the graphics font, INT 1Fh's and 43h's.
    for (size_t i = 0; i < 4; i++) {
        uint16_t vector = (uint16_t)((i < 2 ? 0x1f * 4 : 0x43 * 4)
            + (i & 1) * 2);
        if (save) {
            pokew(seg, (uint16_t)(at + 37 + i * 2), peekw(0, vector));
        } else {
            pokew(0, vector, peekw(seg, (uint16_t)(at + 37 + i * 2)));
        }
    }
}

static void dac_save(uint16_t seg, uint16_t at) {
    pokeb(seg, at, inb(0x3c6));
    pokeb(seg, (uint16_t)(at + 1), inb(0x3c7));
    pokeb(seg, (uint16_t)(at + 2), inb(0x3c8));
    outb(0x3c7, 0);
    for (size_t i = 0; i < 768; i++) {
        pokeb(seg, (uint16_t)(at + 3 + i), inb(0x3c9));
    }
}

static void dac_restore(uint16_t seg, uint16_t at) {
    outb(0x3c6, peekb(seg, at));
    outb(0x3c8, 0);
    for (size_t i = 0; i < 768; i++) {
        outb(0x3c9, peekb(seg, (uint16_t)(at + 3 + i)));
    }
    uint8_t index = peekb(seg, (uint16_t)(at + 2));
    if ((peekb(seg, (uint16_t)(at + 1)) & 3) != 0) {
        outb(0x3c7, index);
    } else {
        outb(0x3c8, index);
    }
}

static void dispi_save(uint16_t seg, uint16_t at) {
    for (size_t i = 0; i < 8; i++) {
        pokew(seg, (uint16_t)(at + i * 2), dispi_read(state_dispi[i]));
    }
}

static void dispi_restore(uint16_t seg, uint16_t at) {
    dispi_write(DISPI_ENABLE, 0);
    for (size_t i = 0; i < 8; i++) {
        uint16_t value = peekw(seg, (uint16_t)(at + i * 2));
        if (state_dispi[i] == DISPI_ENABLE) {
            // Turned on without clearing the video memory, then as it was.
            dispi_write(DISPI_ENABLE, (uint16_t)(value | DISPI_NOCLEARMEM));
        }
        dispi_write(state_dispi[i], value);
    }
}

static void vbe_state(struct vgabios_regs *r) {
    uint8_t states = r->cl & 0x0f;
    if (r->dl == 0x00) {
        uint16_t blocks = 0;
        for (size_t i = 0; i < 4; i++) {
            if ((states & ((uint32_t)1 << i)) != 0) {
                blocks = (uint16_t)(blocks + state_blocks[i]);
            }
        }
        r->bx = blocks;
        r->ax = STATUS_SUCCESS;
        return;
    }
    if (r->dl != 0x01 && r->dl != 0x02) {
        r->ax = STATUS_FAILED;
        return;
    }
    bool save = r->dl == 0x01;
    uint16_t seg = r->es;
    uint16_t at = r->bx;
    for (size_t i = 0; i < 4; i++) {
        if ((states & ((uint32_t)1 << i)) == 0) {
            continue;
        }
        switch (i) {
            case 0: {
                if (save) {
                    hardware_save(seg, at);
                } else {
                    hardware_restore(seg, at);
                }
                break;
            }
            case 1: {
                bios_data_copy(seg, at, save);
                break;
            }
            case 2: {
                if (save) {
                    dac_save(seg, at);
                } else {
                    dac_restore(seg, at);
                }
                break;
            }
            default: {
                if (save) {
                    dispi_save(seg, at);
                } else {
                    dispi_restore(seg, at);
                }
                break;
            }
        }
        at = (uint16_t)(at + state_blocks[i] * (uint32_t)64);
    }
    r->ax = STATUS_SUCCESS;
}

static bool vbe_mode_on(void) {
    return dispi_read(DISPI_ENABLE) & DISPI_ENABLED;
}

void vbe_window(struct vgabios_regs *r) {
    if (r->bl != 0) {
        r->ax = STATUS_FAILED;
        return;
    }
    if (!vbe_mode_on()) {
        r->ax = STATUS_INVALID;
        return;
    }
    switch (r->bh) {
        case 0x00: {
            dispi_write(DISPI_BANK, r->dx);
            r->ax = STATUS_SUCCESS;
            break;
        }
        case 0x01: {
            r->dx = dispi_read(DISPI_BANK);
            r->ax = STATUS_SUCCESS;
            break;
        }
        default: {
            r->ax = STATUS_FAILED;
            break;
        }
    }
}

static void scan_line_length(struct vgabios_regs *r) {
    if (!vbe_mode_on()) {
        r->ax = STATUS_INVALID;
        return;
    }
    size_t bytes = bytes_per_pixel((uint8_t)dispi_read(DISPI_BPP));
    uint32_t height = dispi_read(DISPI_YRES);
    uint32_t width;
    switch (r->bl) {
        case 0x00: {
            width = r->cx;
            break;
        }
        case 0x02: {
            width = (r->cx + bytes - (uint32_t)1) / bytes;
            break;
        }
        case 0x01: {
            width = 0;
            break;
        }
        case 0x03: {
            uint32_t max = vram_size() / height / bytes;
            if (max > DISPI_MAX_XRES) {
                max = DISPI_MAX_XRES;
            }
            max &= ~(uint32_t)7;
            r->bx = (uint16_t)(max * bytes);
            r->cx = (uint16_t)max;
            r->dx = (uint16_t)height;
            r->ax = STATUS_SUCCESS;
            return;
        }
        default: {
            r->ax = STATUS_FAILED;
            return;
        }
    }
    if (r->bl != 0x01) {
        // The interface's lines come in multiples of 8 pixels.
        width = (width + 7) & ~(uint32_t)7;
        if (width > DISPI_MAX_XRES || width * bytes * height > vram_size()) {
            r->ax = STATUS_UNSUPPORTED;
            return;
        }
        dispi_write(DISPI_VIRT_WIDTH, (uint16_t)width);
    }
    uint16_t virt_width = dispi_read(DISPI_VIRT_WIDTH);
    r->bx = (uint16_t)(virt_width * bytes);
    r->cx = virt_width;
    r->dx = dispi_read(DISPI_VIRT_HEIGHT);
    r->ax = STATUS_SUCCESS;
}

static void display_start(struct vgabios_regs *r) {
    if (!vbe_mode_on()) {
        r->ax = STATUS_INVALID;
        return;
    }
    switch (r->bl) {
        case 0x00:
        case 0x80: {
            if ((r->bl & 0x80) != 0) {
                wait_retrace();
            }
            dispi_write(DISPI_X_OFFSET, r->cx);
            dispi_write(DISPI_Y_OFFSET, r->dx);
            r->ax = STATUS_SUCCESS;
            break;
        }
        case 0x01: {
            r->bh = 0;
            r->cx = dispi_read(DISPI_X_OFFSET);
            r->dx = dispi_read(DISPI_Y_OFFSET);
            r->ax = STATUS_SUCCESS;
            break;
        }
        case 0x02:
        case 0x82: {
            size_t bytes = bytes_per_pixel((uint8_t)dispi_read(DISPI_BPP));
            uint32_t line = (uint32_t)dispi_read(DISPI_VIRT_WIDTH) * bytes;
            if ((r->bl & 0x80) != 0) {
                wait_retrace();
            }
            dispi_write(DISPI_X_OFFSET, (uint16_t)(r->ecx % line / bytes));
            dispi_write(DISPI_Y_OFFSET, (uint16_t)(r->ecx / line));
            r->ax = STATUS_SUCCESS;
            break;
        }
        default: {
            r->ax = STATUS_FAILED;
            break;
        }
    }
}

static void dac_format(struct vgabios_regs *r) {
    uint16_t enable = dispi_read(DISPI_ENABLE);
    if ((enable & DISPI_ENABLED) != 0 && dispi_read(DISPI_BPP) > 8) {
        r->ax = STATUS_INVALID;
        return;
    }
    switch (r->bl) {
        case 0x00: {
            uint16_t bank = dispi_read(DISPI_BANK);
            enable = r->bh >= 8 ? (uint16_t)(enable | DISPI_8BIT_DAC)
                : (uint16_t)(enable & ~DISPI_8BIT_DAC);
            // Writing the enable register selects bank 0 again.
            dispi_write(DISPI_ENABLE, enable);
            dispi_write(DISPI_BANK, bank);
            r->bh = (enable & DISPI_8BIT_DAC) != 0 ? 8 : 6;
            r->ax = STATUS_SUCCESS;
            break;
        }
        case 0x01: {
            r->bh = (enable & DISPI_8BIT_DAC) != 0 ? 8 : 6;
            r->ax = STATUS_SUCCESS;
            break;
        }
        default: {
            r->ax = STATUS_FAILED;
            break;
        }
    }
}

static void palette_data(struct vgabios_regs *r) {
    if (r->cx > 256 || r->dx > 256 - r->cx) {
        r->ax = STATUS_FAILED;
        return;
    }
    uint16_t seg = r->es;
    uint16_t off = r->di;
    switch (r->bl) {
        case 0x00:
        case 0x80: {
            if ((r->bl & 0x80) != 0) {
                wait_retrace();
            }
            outb(0x3c8, (uint8_t)r->dx);
            for (size_t i = 0; i < r->cx; i++) {
                uint16_t at = (uint16_t)(off + i * 4);
                outb(0x3c9, peekb(seg, (uint16_t)(at + 2)));
                outb(0x3c9, peekb(seg, (uint16_t)(at + 1)));
                outb(0x3c9, peekb(seg, at));
            }
            r->ax = STATUS_SUCCESS;
            break;
        }
        case 0x01: {
            outb(0x3c7, (uint8_t)r->dx);
            for (size_t i = 0; i < r->cx; i++) {
                uint16_t at = (uint16_t)(off + i * 4);
                pokeb(seg, (uint16_t)(at + 2), inb(0x3c9));
                pokeb(seg, (uint16_t)(at + 1), inb(0x3c9));
                pokeb(seg, at, inb(0x3c9));
                pokeb(seg, (uint16_t)(at + 3), 0);
            }
            r->ax = STATUS_SUCCESS;
            break;
        }
        default: {
            r->ax = STATUS_UNSUPPORTED;
            break;
        }
    }
}

// Any pixel clock a mode asks for is the one it gets.
static void pixel_clock(struct vgabios_regs *r) {
    r->ax = r->bl == 0x00 ? STATUS_SUCCESS : STATUS_FAILED;
}

static void ddc(struct vgabios_regs *r) {
    size_t edid_size = vgabios_info.edid_size;
    switch (r->bl) {
        case 0x00: {
            if (edid_size == 0) {
                r->ax = STATUS_FAILED;
                break;
            }
            // DDC2, at no noticeable time per block.
            r->bx = 0x0002;
            r->ax = STATUS_SUCCESS;
            break;
        }
        case 0x01: {
            uint32_t at = (uint32_t)r->dx * 128;
            if (r->cx != 0 || at + 128 > edid_size) {
                r->ax = STATUS_FAILED;
                break;
            }
            for (size_t i = 0; i < 128; i++) {
                pokeb(r->es, (uint16_t)(r->di + i), vgabios_info.edid[at + i]);
            }
            r->ax = STATUS_SUCCESS;
            break;
        }
        default: {
            r->ax = STATUS_FAILED;
            break;
        }
    }
}

void vbe_int10(struct vgabios_regs *r) {
    if (!present()) {
        return;
    }
    switch (r->al) {
        case 0x00: {
            controller_info(r);
            break;
        }
        case 0x01: {
            mode_info(r);
            break;
        }
        case 0x02: {
            set_mode(r);
            break;
        }
        case 0x03: {
            current_mode(r);
            break;
        }
        case 0x04: {
            vbe_state(r);
            break;
        }
        case 0x05: {
            vbe_window(r);
            break;
        }
        case 0x06: {
            scan_line_length(r);
            break;
        }
        case 0x07: {
            display_start(r);
            break;
        }
        case 0x08: {
            dac_format(r);
            break;
        }
        case 0x09: {
            palette_data(r);
            break;
        }
        case 0x0b: {
            pixel_clock(r);
            break;
        }
        case 0x15: {
            ddc(r);
            break;
        }
        default: {
            r->ax = STATUS_FAILED;
            break;
        }
    }
}
