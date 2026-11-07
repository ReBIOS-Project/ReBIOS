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
#include <virt/vgabios.h>
#include <core.h>
#include <handoff.h>
#include <lib/log.h>
#include <lib/spinlock.h>
#include <memory.h>
#include <sys/task.h>
#include <sys/x86.h>
#include <virt/bda.h>
#include <virt/edid.h>
#include <virt/io.h>
#include <virt/mmio.h>
#include <virt/vcpu.h>
#include <virt/vga.h>
#include <virt/vgabios_stub.h>

#define CALLS 8
#define PORT_COUNT (CALLS + 1)

#define REGISTER_COUNT 7

#define VGA_DAC_DATA 0x3c9

#define ALL_ONES 0xffff

extern const uint8_t vgabios_stub[];
extern const uint8_t vgabios_stub_end[];
extern const uint8_t vgabios_stub_int10[];
extern const uint8_t vgabios_stub_window[];

struct call {
    struct task task;
    bool task_made;
    bool busy;
    bool finished;
    // In true real mode: everything happens here, in the exit, and the words
    // to and from the ROM are only for when that takes too long at once.
    bool direct;
    // The ROM's initialisation rather than INT 10h.
    bool init;
    uint64_t order;
    struct vcpu *v;
    uintptr_t frame;
    struct vgabios_regs regs;
    // The words the ROM is to read next, then the ones it is to write.
    const uint16_t *head;
    size_t head_left;
    const uint8_t *data;
    size_t data_left;
    uint16_t *into;
    size_t into_left;
};

static const uint8_t functionality[16] = {
    // What INT 10h AH=1Bh points to: modes 0-7, 0Dh-13h, 200, 350, and 400
    // scan lines, 8 character blocks, 2 at once, and the usual functions.
    0xff, 0xe0, 0x0f, 0x00, 0x00, 0x00, 0x00, 0x07, 0x08, 0x02, 0xff, 0x0e,
    0x00, 0x00, 0x00, 0x00
};

struct vgabios_display vgabios_display;

static struct call calls[CALLS + 1];
static uint64_t next_order;
static size_t knocks;
static bool init_knocked;
static struct call *current;
static uint8_t rom_image[VGABIOS_ROM_SIZE];
static struct spinlock lock;

static void slice_end(void) {
    task_yield_if_over();
}

// Hands words to the ROM, and takes others from it, until the call goes on.
static void exchange(
    const uint16_t *head, size_t head_count, const uint8_t *data, size_t data_words,
    uint16_t *into, size_t into_count
) {
    struct call *c = current;
    c->head = head;
    c->head_left = head_count;
    c->data = data;
    c->data_left = data_words;
    c->into = into;
    c->into_left = into_count;
    task_yield();
}

static uintptr_t linear(uint16_t segment, uint16_t offset) {
    return ((uintptr_t)segment << 4) + offset;
}

static uint32_t direct_port(uint16_t port, size_t size, bool in, uint32_t out) {
    uint32_t value = out;
    io_dispatch(current->v, port, size, in, &value);
    return value;
}

static uint16_t direct_read(uint16_t segment, uint16_t offset, size_t size) {
    uint16_t value = 0;
    if (!mmio_read_linear(current->v, linear(segment, offset), &value, size)) {
        return ALL_ONES;
    }
    return value;
}

static void direct_write(uint16_t segment, uint16_t offset, uint16_t value, size_t size) {
    mmio_write_linear(current->v, linear(segment, offset), &value, size);
}

uint8_t vgabios_in(uint16_t port) {
    if (current->direct) {
        return (uint8_t)direct_port(port, 1, true, 0);
    }
    uint16_t head = (uint16_t)(VGABIOS_STUB_OP_VGA_IN | (port - VGABIOS_STUB_VGA_PORTS));
    uint16_t value;
    exchange(&head, 1, NULL, 0, &value, 1);
    return (uint8_t)value;
}

void vgabios_out(uint16_t port, uint8_t value) {
    if (current->direct) {
        direct_port(port, 1, false, value);
        return;
    }
    uint16_t head = (uint16_t)(VGABIOS_STUB_OP_VGA_OUT | (port - VGABIOS_STUB_VGA_PORTS)
                             | value << 8);
    exchange(&head, 1, NULL, 0, NULL, 0);
}

void vgabios_out_indexed(uint16_t port, uint8_t index, uint8_t value) {
    uint16_t both = (uint16_t)(value << 8 | index);
    if (current->direct) {
        direct_port(port, 2, false, both);
        return;
    }
    uint16_t set = VGABIOS_STUB_OP_INDEXED_CRTC;
    if (port == 0x3b4) {
        set = VGABIOS_STUB_OP_INDEXED_CRTC_MONO;
    } else if (port == 0x3c4) {
        set = VGABIOS_STUB_OP_INDEXED_SEQ;
    } else if (port == 0x3ce) {
        set = VGABIOS_STUB_OP_INDEXED_GC;
    }
    uint16_t head = both | set;
    exchange(&head, 1, NULL, 0, NULL, 0);
}

void vgabios_out_block(uint16_t port, const uint8_t *bytes, size_t count) {
    if (current->direct) {
        for (size_t i = 0; i < count; i++) {
            direct_port(port, 1, false, bytes[i]);
            slice_end();
        }
        return;
    }
    size_t words = count / 2;
    uint16_t head[3] = { VGABIOS_STUB_OP_OUT_BLOCK, port, (uint16_t)words };
    exchange(head, 3, bytes, words, NULL, 0);
    if ((count & 1) != 0) {
        vgabios_out(port, bytes[count - 1]);
    }
}

void vgabios_dac_from_rom(uint16_t offset, size_t count) {
    if (current->direct) {
        for (size_t i = 0; i < (size_t)256 * 3; i++) {
            direct_port(VGA_DAC_DATA, 1, false, i < count ? rom_image[offset + i] : 0);
            slice_end();
        }
        return;
    }
    uint16_t head[3] = { VGABIOS_STUB_OP_DAC_ROM, offset, (uint16_t)count };
    exchange(head, 3, NULL, 0, NULL, 0);
}

// What is in the ROM is known here, and asking for it would only take longer.
static bool in_rom(uint16_t segment, uint16_t offset, size_t size) {
    return segment == VGABIOS_SEGMENT && offset + size <= VGABIOS_ROM_SIZE;
}

uint8_t vgabios_peekb(uint16_t segment, uint16_t offset) {
    if (in_rom(segment, offset, 1)) {
        return rom_image[offset];
    }
    if (current->direct) {
        return (uint8_t)direct_read(segment, offset, 1);
    }
    uint16_t value;
    if (segment == BDA && offset <= 0xff) {
        uint16_t head = (uint16_t)(VGABIOS_STUB_OP_BDA_READ8 | offset << 8);
        exchange(&head, 1, NULL, 0, &value, 1);
    } else {
        uint16_t head[3] = { VGABIOS_STUB_OP_READ8, segment, offset };
        exchange(head, 3, NULL, 0, &value, 1);
    }
    return (uint8_t)value;
}

uint16_t vgabios_peekw(uint16_t segment, uint16_t offset) {
    if (in_rom(segment, offset, 2)) {
        return (uint16_t)(rom_image[offset] | rom_image[offset + 1] << 8);
    }
    if (current->direct) {
        return direct_read(segment, offset, 2);
    }
    uint16_t value;
    if (segment == BDA && offset <= 0xff) {
        uint16_t head = (uint16_t)(VGABIOS_STUB_OP_BDA_READ16 | offset << 8);
        exchange(&head, 1, NULL, 0, &value, 1);
    } else {
        uint16_t head[3] = { VGABIOS_STUB_OP_READ16, segment, offset };
        exchange(head, 3, NULL, 0, &value, 1);
    }
    return value;
}

void vgabios_pokeb(uint16_t segment, uint16_t offset, uint8_t value) {
    if (current->direct) {
        direct_write(segment, offset, value, 1);
        return;
    }
    if (segment == BDA && offset <= 0xff) {
        uint16_t head[2] = { (uint16_t)(VGABIOS_STUB_OP_BDA_WRITE8 | offset << 8), value };
        exchange(head, 2, NULL, 0, NULL, 0);
    } else {
        uint16_t head[3] = { (uint16_t)(VGABIOS_STUB_OP_WRITE8 | value << 8), segment, offset };
        exchange(head, 3, NULL, 0, NULL, 0);
    }
}

void vgabios_pokew(uint16_t segment, uint16_t offset, uint16_t value) {
    if (current->direct) {
        direct_write(segment, offset, value, 2);
        return;
    }
    if (segment == BDA && offset <= 0xff) {
        uint16_t head[2] = { (uint16_t)(VGABIOS_STUB_OP_BDA_WRITE16 | offset << 8), value };
        exchange(head, 2, NULL, 0, NULL, 0);
    } else {
        uint16_t head[4] = { VGABIOS_STUB_OP_WRITE16, segment, offset, value };
        exchange(head, 4, NULL, 0, NULL, 0);
    }
}

void vgabios_poke_block(uint16_t segment, uint16_t offset, const void *bytes, size_t count) {
    const uint8_t *p = bytes;
    if (current->direct) {
        for (size_t i = 0; i + 2 <= count; i += 2) {
            direct_write(segment, (uint16_t)(offset + i), (uint16_t)(p[i] | p[i + 1] << 8), 2);
            slice_end();
        }
        return;
    }
    uint16_t head[4] = { VGABIOS_STUB_OP_WRITE_BLOCK, segment, offset, (uint16_t)(count / 2) };
    exchange(head, 4, p, count / 2, NULL, 0);
}

void vgabios_far_fill8(uint16_t segment, uint16_t offset, uint8_t value, size_t count) {
    if (count == 0) {
        return;
    }
    if (current->direct) {
        for (size_t i = 0; i < count; i++) {
            direct_write(segment, (uint16_t)(offset + i), value, 1);
            slice_end();
        }
        return;
    }
    uint16_t head[4] = {
        (uint16_t)(VGABIOS_STUB_OP_FILL8 | value << 8), segment, offset, (uint16_t)count
    };
    exchange(head, 4, NULL, 0, NULL, 0);
}

void vgabios_far_fill16(uint16_t segment, uint16_t offset, uint16_t value, size_t count) {
    if (count == 0) {
        return;
    }
    if (current->direct) {
        for (size_t i = 0; i < count; i++) {
            direct_write(segment, (uint16_t)(offset + i * 2), value, 2);
            slice_end();
        }
        return;
    }
    uint16_t head[5] = { VGABIOS_STUB_OP_FILL16, segment, offset, (uint16_t)count, value };
    exchange(head, 5, NULL, 0, NULL, 0);
}

void vgabios_far_move(uint16_t segment, uint16_t to, uint16_t from, size_t count) {
    if (count == 0) {
        return;
    }
    bool backwards = to > from;
    if (current->direct) {
        for (size_t i = 0; i < count; i++) {
            size_t at = backwards ? count - 1 - i : i;
            direct_write(
                segment, (uint16_t)(to + at), direct_read(segment, (uint16_t)(from + at), 1), 1
            );
            slice_end();
        }
        return;
    }
    uint16_t head[6] = {
        (uint16_t)(VGABIOS_STUB_OP_COPY | (backwards ? 1 : 0) << 8), segment,
        (uint16_t)(backwards ? to + count - 1 : to), segment,
        (uint16_t)(backwards ? from + count - 1 : from), (uint16_t)count
    };
    exchange(head, 6, NULL, 0, NULL, 0);
}

void vgabios_far_font(
    uint16_t to_segment, uint16_t to, uint16_t from_segment, uint16_t from, uint8_t height,
    size_t count
) {
    if (count == 0) {
        return;
    }
    if (current->direct) {
        for (size_t c = 0; c < count; c++) {
            for (size_t i = 0; i < height; i++) {
                uint16_t source = (uint16_t)(from + c * height + i);
                direct_write(
                    to_segment, (uint16_t)(to + c * 32 + i), vgabios_peekb(from_segment, source), 1
                );
            }
            slice_end();
        }
        return;
    }
    uint16_t head[6] = {
        (uint16_t)(VGABIOS_STUB_OP_FONT | height << 8), to_segment, to, from_segment, from,
        (uint16_t)count
    };
    exchange(head, 6, NULL, 0, NULL, 0);
}

uint32_t vgabios_ecx(const struct vgabios_regs *r) {
    if (current->direct) {
        return (uint32_t)current->v->gprs.rcx;
    }
    uint16_t head = VGABIOS_STUB_OP_ECX_HIGH;
    uint16_t high;
    exchange(&head, 1, NULL, 0, &high, 1);
    return (uint32_t)high << 16 | r->cx;
}

void vgabios_relax(void) {
    if (current->direct) {
        slice_end();
    }
}

uint16_t vgabios_palette_offset(size_t index) {
    size_t at = VGABIOS_ROM_PALETTES;
    for (size_t i = 0; i < index; i++) {
        at += vgabios_tables_palettes[i].count * 3;
    }
    return (uint16_t)at;
}

uint16_t vgabios_int10_offset(void) {
    return (uint16_t)(vgabios_stub_int10 - vgabios_stub);
}

uint16_t vgabios_window_offset(void) {
    return (uint16_t)(vgabios_stub_window - vgabios_stub);
}

// Where the stub saves each register, in struct vgabios_regs's order.
static const uint8_t frame_offsets[REGISTER_COUNT] = {
    VGABIOS_STUB_FRAME_AX, VGABIOS_STUB_FRAME_BX, VGABIOS_STUB_FRAME_CX, VGABIOS_STUB_FRAME_DX,
    VGABIOS_STUB_FRAME_DI, VGABIOS_STUB_FRAME_BP, VGABIOS_STUB_FRAME_ES
};

static void frame_load(struct call *c) {
    uint16_t words[REGISTER_COUNT];
    for (size_t i = 0; i < REGISTER_COUNT; i++) {
        words[i] = 0;
        mmio_read_linear(c->v, c->frame + frame_offsets[i], &words[i], 2);
    }
    memcpy(&c->regs, words, sizeof(words));
}

// What the call changed, back in the registers the ROM saved.
static void registers_give(struct call *c, const struct vgabios_regs *before) {
    uint16_t old[REGISTER_COUNT];
    uint16_t now[REGISTER_COUNT];
    memcpy(old, before, sizeof(old));
    memcpy(now, &c->regs, sizeof(now));
    for (size_t i = 0; i < REGISTER_COUNT; i++) {
        if (now[i] == old[i]) {
            continue;
        }
        if (c->direct) {
            mmio_write_linear(c->v, c->frame + frame_offsets[i], &now[i], 2);
        } else {
            uint16_t head[2] = {
                (uint16_t)(VGABIOS_STUB_OP_SET_REGISTER | frame_offsets[i] << 8), now[i]
            };
            exchange(head, 2, NULL, 0, NULL, 0);
        }
    }
}

static void call_main(void *arg) {
    struct call *c = arg;
    for (;;) {
        if (!c->direct) {
            uint16_t head = VGABIOS_STUB_OP_REGISTERS;
            uint16_t words[REGISTER_COUNT];
            exchange(&head, 1, NULL, 0, words, REGISTER_COUNT);
            memcpy(&c->regs, words, sizeof(words));
        }
        struct vgabios_regs before = c->regs;
        if (c->init) {
            vgabios_int10_init_call();
        } else {
            vgabios_int10(&c->regs);
        }
        registers_give(c, &before);
        c->finished = true;
        task_yield();
    }
}

static void call_run(struct call *c, struct vcpu *v) {
    c->v = v;
    current = c;
    task_run(&c->task);
    current = NULL;
}

// A real mode caller's own CPU is at this instruction of the ROM: nothing can
// be virtualising the VGA, and what its registers and memory hold is the
// truth.
static bool real_mode_rom(struct vcpu *v) {
    if ((vcpu_cr0(v) & CR0_PE) != 0) {
        return false;
    }
    struct vcpu_segment cs;
    vcpu_get_seg(v, VCPU_SEG_CS, &cs);
    return cs.selector == VGABIOS_SEGMENT;
}

// The call a new one takes the place of: one that nothing ended, as with a
// virtual machine taken away during it, once all others are in use.
static struct call *call_slot(bool vga_ports) {
    struct call *oldest = &calls[CALLS];
    if (!vga_ports) {
        oldest = &calls[0];
        for (size_t i = 0; i < CALLS; i++) {
            if (!calls[i].busy) {
                return &calls[i];
            }
            if (calls[i].order < oldest->order) {
                oldest = &calls[i];
            }
        }
    } else if (!oldest->busy) {
        return oldest;
    }
    printf("The video BIOS gives up on a call that did not end\n");
    task_restart(&oldest->task);
    return oldest;
}

static uint16_t call_begin(struct vcpu *v, bool vga_ports) {
    struct call *c = call_slot(vga_ports);
    if (!c->task_made) {
        task_init(&c->task, call_main, c);
        c->task_made = true;
    }
    c->busy = true;
    c->finished = false;
    c->order = next_order++;
    c->head_left = 0;
    c->data_left = 0;
    c->into_left = 0;
    c->direct = real_mode_rom(v);
    c->init = init_knocked;
    init_knocked = false;
    uint16_t port = vga_ports
                  ? VGABIOS_STUB_PORT_VGA + VGABIOS_STUB_PORT_VGA_CALL_OFFSET
                  : (uint16_t)(VGABIOS_STUB_PORT_ALLOCATE + 1 + (c - calls));
    if (!c->direct) {
        return port;
    }
    struct vcpu_segment ss;
    vcpu_get_seg(v, VCPU_SEG_SS, &ss);
    c->v = v;
    c->frame = ss.base + (uint16_t)v->gprs.rbp;
    frame_load(c);
    call_run(c, v);
    if (c->finished) {
        c->busy = false;
        return 0;
    }
    return port;
}

static uint16_t call_word(struct call *c, struct vcpu *v) {
    if (c->head_left == 0 && c->data_left == 0) {
        if (c->into_left != 0) {
            return VGABIOS_STUB_OP_NOTHING;
        }
        call_run(c, v);
        if (c->finished) {
            c->busy = false;
            return VGABIOS_STUB_OP_DONE;
        }
    }
    if (c->head_left != 0) {
        c->head_left--;
        return *c->head++;
    }
    if (c->data_left != 0) {
        uint16_t word = (uint16_t)(c->data[0] | c->data[1] << 8);
        c->data += 2;
        c->data_left--;
        return word;
    }
    return VGABIOS_STUB_OP_NOTHING;
}

static uint32_t allocate_access(struct vcpu *v, bool vga_ports, bool in, uint16_t word) {
    if (in) {
        if (knocks == 0) {
            return 0xffffffff;
        }
        knocks--;
        return call_begin(v, vga_ports);
    }
    if (knocks < CALLS && (word == VGABIOS_STUB_KNOCK || word == VGABIOS_STUB_KNOCK_INIT)) {
        knocks++;
        init_knocked = word == VGABIOS_STUB_KNOCK_INIT;
    }
    return 0xffffffff;
}

static uint32_t call_access(struct call *c, struct vcpu *v, bool in, uint16_t word) {
    if (!c->busy) {
        return 0xffffffff;
    }
    if (in) {
        return call_word(c, v);
    }
    if (c->into_left != 0) {
        c->into_left--;
        *c->into++ = word;
    }
    return 0xffffffff;
}

static void ports_access(
    struct vcpu *v, void *ctx, uint16_t offset, size_t size, bool in, uint32_t *value
) {
    (void)ctx;
    uint32_t result = 0xffffffff;
    if (size == 2) {
        spinlock_acquire(&lock);
        if (offset == 0) {
            result = allocate_access(v, false, in, (uint16_t)*value);
        } else {
            result = call_access(&calls[offset - 1], v, in, (uint16_t)*value);
        }
        spinlock_release(&lock);
    }
    if (in) {
        *value = result;
    }
}

static void vga_ports_access(
    struct vcpu *v, void *ctx, uint16_t offset, size_t size, bool in, uint32_t *value
) {
    (void)ctx;
    uint32_t result = 0xffffffff;
    if (size == 2) {
        spinlock_acquire(&lock);
        if (offset == VGABIOS_STUB_PORT_VGA_ALLOCATE_OFFSET) {
            result = allocate_access(v, true, in, (uint16_t)*value);
        } else if (offset == VGABIOS_STUB_PORT_VGA_CALL_OFFSET) {
            result = call_access(&calls[CALLS], v, in, (uint16_t)*value);
        }
        spinlock_release(&lock);
    }
    if (in) {
        *value = result;
    }
}

void vgabios_init(void) {
    struct io_handler ports = {
        VGABIOS_STUB_PORT_ALLOCATE, PORT_COUNT, ports_access, NULL, false
    };
    io_register(&ports);
    struct io_handler vga_ports = {
        VGABIOS_STUB_PORT_VGA, VGABIOS_STUB_PORT_VGA_COUNT, vga_ports_access, NULL, false
    };
    io_register(&vga_ports);
}

// A far pointer's string, which the ROM keeps.
static void rom_string(uint16_t at, const char *text) {
    for (size_t i = 0; text[i] != 0; i++) {
        rom_image[at + i] = (uint8_t)text[i];
    }
}

// What the video BIOS learns of the display it drives.
static void display_fill(void) {
    const struct handoff_video *video = &core_handoff->video;
    struct vgabios_display *d = &vgabios_display;
    d->dispi = video->vram_size != 0;
    uint32_t width;
    uint32_t height;
    vga_screen_size(&width, &height);
    d->max_width = (uint16_t)width;
    d->max_height = (uint16_t)height;
    d->lfb = (uint32_t)vga_lfb();
    // A turned panel's own EDID describes it unturned.
    if (video->framebuffer.rotation == HANDOFF_ROTATE_0) {
        memcpy(d->edid, video->edid, video->edid_size);
        d->edid_size = (uint16_t)edid_sanitise(d->edid, video->edid_size);
    }
    // Without one, guests would not know the display's resolution.
    if (d->edid_size == 0 && edid_synthesize(d->edid, width, height)) {
        d->edid_size = EDID_BLOCK_SIZE;
        const char *reason = video->edid_size == 0
                           ? "No EDID from the firmware"
                           : video->framebuffer.rotation != HANDOFF_ROTATE_0
                               ? "The panel's EDID describes it unturned"
                               : "The firmware's EDID is unusable";
        printf(
            "%s, so the emulated VGA's display reports %ux%u at 60 Hz\n", reason, width, height
        );
    }
}

void vgabios_rom_fill(uint8_t *image) {
    size_t stub = (size_t)(vgabios_stub_end - vgabios_stub);
    if (stub > VGABIOS_ROM_FONT8) {
        panic("The video BIOS's program is larger than its room\n");
    }
    display_fill();
    memset(rom_image, 0, sizeof(rom_image));
    memcpy(rom_image, vgabios_stub, stub);
    rom_image[2] = VGABIOS_ROM_SIZE / 512;
    memcpy(rom_image + VGABIOS_ROM_FONT8, vgabios_tables_font8, sizeof(vgabios_tables_font8));
    memcpy(rom_image + VGABIOS_ROM_FONT14, vgabios_tables_font14, sizeof(vgabios_tables_font14));
    memcpy(rom_image + VGABIOS_ROM_FONT16, vgabios_tables_font16, sizeof(vgabios_tables_font16));
    memcpy(rom_image + VGABIOS_ROM_FUNCTIONALITY, functionality, sizeof(functionality));
    rom_string(VGABIOS_ROM_OEM_STRING, "ReBIOS VBE");
    rom_string(VGABIOS_ROM_VENDOR_NAME, "ReBIOS");
    rom_string(VGABIOS_ROM_PRODUCT_NAME, "ReBIOS VGA");
    rom_string(VGABIOS_ROM_PRODUCT_REVISION, "");
    for (size_t i = 0; i < vgabios_tables_palette_count; i++) {
        size_t bytes = vgabios_tables_palettes[i].count * 3;
        size_t at = vgabios_palette_offset(i);
        if (at + bytes > VGABIOS_ROM_SIZE) {
            panic("The video BIOS's palettes are larger than their room\n");
        }
        memcpy(rom_image + at, vgabios_tables_palettes[i].colours, bytes);
    }
    memcpy(image, rom_image, VGABIOS_ROM_SIZE);
}
