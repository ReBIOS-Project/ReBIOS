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

#ifndef VGABIOS_H
#define VGABIOS_H

// The video BIOS of the hypervisor's VGA: real-mode code the guest runs from
// segment C000h. It runs with DS = ES = SS, the caller's stack segment, so
// that pointers to locals work, reads its own tables through FS = CS, which
// is why they are __seg_fs, and reaches any other memory through GS with the
// helpers below.

#include <stdint.h>
#include <stdbool.h>
#include <vgabios_info.h>

#define MODE_TEXT 0
#define MODE_CGA 1
#define MODE_CGA1 2
#define MODE_PLANAR 3
#define MODE_LINEAR 4

struct vgabios_mode {
    uint8_t mode;
    uint8_t kind;
    uint8_t columns;
    uint8_t rows;
    uint8_t char_height;
    uint8_t palette;
    uint16_t width;
    uint16_t height;
    uint16_t segment;
    uint16_t page_size;
    // Text modes come at 200, 350, and 400 lines.
    uint16_t scan_lines;
    uint8_t misc;
    // Sequencer registers 1 to 4.
    uint8_t seq[4];
    uint8_t crtc[25];
    // Attribute controller registers 0 to 13h.
    uint8_t ac[20];
    uint8_t gc[9];
};

struct vgabios_palette {
    const __seg_fs uint8_t *colors;
    uint16_t count;
};

extern const __seg_fs uint8_t vgabios_font8[256 * 8];
extern const __seg_fs uint8_t vgabios_font14[256 * 14];
extern const __seg_fs uint8_t vgabios_font16[256 * 16];
extern const __seg_fs struct vgabios_palette vgabios_palettes[];
extern const __seg_fs uint8_t vgabios_mode_count;
extern const __seg_fs struct vgabios_mode vgabios_modes[];

// The caller's registers, as the entry code saves them.
struct vgabios_regs {
    uint16_t gs;
    uint16_t fs;
    uint16_t es;
    uint16_t ds;
    union {
        uint32_t edi;
        uint16_t di;
    };
    union {
        uint32_t esi;
        uint16_t si;
    };
    union {
        uint32_t ebp;
        uint16_t bp;
    };
    uint32_t esp;
    union {
        uint32_t ebx;
        uint16_t bx;
        struct {
            uint8_t bl;
            uint8_t bh;
        };
    };
    union {
        uint32_t edx;
        uint16_t dx;
        struct {
            uint8_t dl;
            uint8_t dh;
        };
    };
    union {
        uint32_t ecx;
        uint16_t cx;
        struct {
            uint8_t cl;
            uint8_t ch;
        };
    };
    union {
        uint32_t eax;
        uint16_t ax;
        struct {
            uint8_t al;
            uint8_t ah;
        };
    };
    uint16_t ip;
    uint16_t cs;
    uint16_t flags;
};

void vgabios_init(void);
void vgabios_int10(struct vgabios_regs *r);
extern const char vgabios_int10_entry[];

// Defined in assembly, so that nothing assumes the values it has in the image.
extern const __seg_fs struct vgabios_info vgabios_info;

void vgabios_set_mode(uint8_t mode);
// The DAC's colours of the standard 256-colour mode.
void vgabios_load_palette_256(void);

// The VBE functions, INT 10h AH=4Fh, and the window function they give a far
// pointer to, which far calls reach with the registers of AX=4F05h.
void vbe_int10(struct vgabios_regs *r);
void vbe_window(struct vgabios_regs *r);
extern const char vbe_window_entry[];
// Turns the Bochs display interface off, for a VGA mode.
void vbe_disable(void);

static inline void outb(uint16_t port, uint8_t value) {
    asm volatile ("outb %0, %1" :: "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t value;
    asm volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void outw(uint16_t port, uint16_t value) {
    asm volatile ("outw %0, %1" :: "a"(value), "Nd"(port));
}

static inline uint16_t inw(uint16_t port) {
    uint16_t value;
    asm volatile ("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void outl(uint16_t port, uint32_t value) {
    asm volatile ("outl %0, %1" :: "a"(value), "Nd"(port));
}

static inline uint32_t inl(uint16_t port) {
    uint32_t value;
    asm volatile ("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void pokel(uint16_t segment, uint16_t offset, uint32_t value) {
    asm volatile ("movw %w0, %%gs\n\tmovl %1, %%gs:(%k2)"
                  :: "r"(segment), "r"(value), "r"((uint32_t)offset) : "memory");
}

static inline uint16_t code_segment(void) {
    uint16_t cs;
    asm ("movw %%cs, %0" : "=r"(cs));
    return cs;
}

static inline uint16_t stack_segment(void) {
    uint16_t ss;
    asm ("movw %%ss, %0" : "=r"(ss));
    return ss;
}

// Fills count bytes of the stack segment, which ES is, as DS is.
static inline void local_fill(void *p, uint8_t value, uint32_t count) {
    uint32_t di = (uint32_t)(uintptr_t)p;
    asm volatile ("rep stosb" : "+D"(di), "+c"(count) : "a"(value) : "memory");
}

static inline uint8_t peekb(uint16_t segment, uint16_t offset) {
    uint8_t value;
    asm volatile ("movw %w1, %%gs\n\tmovb %%gs:(%k2), %0"
                  : "=q"(value) : "r"(segment), "r"((uint32_t)offset) : "memory");
    return value;
}

static inline uint16_t peekw(uint16_t segment, uint16_t offset) {
    uint16_t value;
    asm volatile ("movw %w1, %%gs\n\tmovw %%gs:(%k2), %0"
                  : "=r"(value) : "r"(segment), "r"((uint32_t)offset) : "memory");
    return value;
}

static inline void pokeb(uint16_t segment, uint16_t offset, uint8_t value) {
    asm volatile ("movw %w0, %%gs\n\tmovb %1, %%gs:(%k2)"
                  :: "r"(segment), "q"(value), "r"((uint32_t)offset) : "memory");
}

static inline void pokew(uint16_t segment, uint16_t offset, uint16_t value) {
    asm volatile ("movw %w0, %%gs\n\tmovw %1, %%gs:(%k2)"
                  :: "r"(segment), "r"(value), "r"((uint32_t)offset) : "memory");
}

// Fills count words from segment:offset.
static inline void far_fill16(uint16_t segment, uint16_t offset, uint16_t value, uint32_t count) {
    uint32_t di = offset;
    asm volatile ("pushw %%es\n\tmovw %w3, %%es\n\trep stosw\n\tpopw %%es"
                  : "+D"(di), "+c"(count) : "a"(value), "r"(segment) : "memory");
}

// Fills count bytes from segment:offset.
static inline void far_fill8(uint16_t segment, uint16_t offset, uint8_t value, uint32_t count) {
    uint32_t di = offset;
    asm volatile ("pushw %%es\n\tmovw %w3, %%es\n\trep stosb\n\tpopw %%es"
                  : "+D"(di), "+c"(count) : "a"(value), "r"(segment) : "memory");
}

// Moves count bytes within a segment, overlapping or not.
static inline void far_move(uint16_t segment, uint16_t to, uint16_t from, uint32_t count) {
    if (count == 0) {
        return;
    }
    uint32_t di = to;
    uint32_t si = from;
    if (to > from) {
        di += count - 1;
        si += count - 1;
        asm volatile ("pushw %%ds\n\tpushw %%es\n\tmovw %w3, %%ds\n\tmovw %w3, %%es\n\t"
                      "std\n\trep movsb\n\tcld\n\tpopw %%es\n\tpopw %%ds"
                      : "+D"(di), "+S"(si), "+c"(count) : "r"(segment) : "memory");
    } else {
        asm volatile ("pushw %%ds\n\tpushw %%es\n\tmovw %w3, %%ds\n\tmovw %w3, %%es\n\t"
                      "rep movsb\n\tpopw %%es\n\tpopw %%ds"
                      : "+D"(di), "+S"(si), "+c"(count) : "r"(segment) : "memory");
    }
}

// Copies count bytes between segments.
static inline void far_copy(uint16_t to_segment, uint16_t to, uint16_t from_segment, uint16_t from,
                            uint32_t count) {
    uint32_t di = to;
    uint32_t si = from;
    asm volatile ("pushw %%ds\n\tpushw %%es\n\tmovw %w4, %%ds\n\tmovw %w3, %%es\n\t"
                  "rep movsb\n\tpopw %%es\n\tpopw %%ds"
                  : "+D"(di), "+S"(si), "+c"(count) : "r"(to_segment), "r"(from_segment)
                  : "memory");
}

// Copies count bytes of the ROM's own data to segment:offset.
static inline void far_copy_rom(uint16_t segment, uint16_t offset, const __seg_fs void *from,
                                uint32_t count) {
    uint32_t di = offset;
    uint32_t si = (uint32_t)(uintptr_t)from;
    asm volatile ("pushw %%es\n\tmovw %w3, %%es\n\trep movsb %%fs:(%%esi), %%es:(%%edi)\n\t"
                  "popw %%es"
                  : "+D"(di), "+S"(si), "+c"(count) : "r"(segment) : "memory");
}

#endif
