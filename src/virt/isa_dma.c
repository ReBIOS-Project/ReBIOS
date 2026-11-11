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
#include <virt/isa_dma.h>
#include <lib/spinlock.h>
#include <virt/io.h>
#include <virt/vcpu.h>

// The PC/AT's two 8237s and their page registers, as the ICH10 has them
// (datasheet 319973-003, 13.2): every PC has them, but no device of the
// guest's does DMA through them, so their counts never run, no channel ever
// requests, and none reaches its terminal count.

// The first controller's registers, aliased 10h up, and the second's, one at
// each even port, aliased at the odd one after it (Table 13-2).
#define FIRST_BASE 0x00
#define FIRST_COUNT 0x20
#define SECOND_BASE 0xc0
#define SECOND_COUNT 0x20
#define REGISTER_MASK 0x0f
// The page registers, aliased 90h up, but at 92h, port 92h's.
#define PAGES_BASE 0x80
#define PAGES_COUNT 0x20
#define PAGES_ALIAS_SKIP 0x92

#define REG_COMMAND 0x08
#define REG_REQUEST 0x09
#define REG_SINGLE_MASK 0x0a
#define REG_MODE 0x0b
#define REG_CLEAR_FLIP_FLOP 0x0c
#define REG_MASTER_CLEAR 0x0d
#define REG_CLEAR_MASK 0x0e
#define REG_ALL_MASK 0x0f
#define CHANNEL_MASK 0x03
#define SINGLE_MASK_SET 0x04
#define ALL_MASK_BITS 0x0f
#define CHANNELS 4

struct controller {
    uint16_t address[CHANNELS];
    uint16_t count[CHANNELS];
    uint8_t mode[CHANNELS];
    uint8_t command;
    uint8_t mask;
    // The high byte of an address or count comes next.
    bool high;
};

static struct spinlock lock;
static struct controller controllers[2];
static uint8_t pages[16];

static void master_clear(struct controller *c) {
    c->command = 0;
    c->high = false;
    c->mask = ALL_MASK_BITS;
}

static uint8_t controller_read(struct controller *c, uint8_t reg) {
    if (reg < REG_COMMAND) {
        uint16_t *word = (reg & 1) != 0 ? &c->count[reg / 2] : &c->address[reg / 2];
        uint8_t byte = (uint8_t)(c->high ? *word >> 8 : *word);
        c->high = !c->high;
        return byte;
    }
    switch (reg) {
        case REG_COMMAND: {
            // The status: nothing requested, nothing at its terminal count.
            return 0;
        }
        case REG_ALL_MASK: {
            return c->mask;
        }
        default: {
            return 0xff;
        }
    }
}

static void controller_write(struct controller *c, uint8_t reg, uint8_t value) {
    if (reg < REG_COMMAND) {
        uint16_t *word = (reg & 1) != 0 ? &c->count[reg / 2] : &c->address[reg / 2];
        if (c->high) {
            *word = (uint16_t)((*word & 0x00ff) | (uint16_t)value << 8);
        } else {
            *word = (uint16_t)((*word & 0xff00) | value);
        }
        c->high = !c->high;
        return;
    }
    switch (reg) {
        case REG_COMMAND: {
            c->command = value;
            break;
        }
        case REG_SINGLE_MASK: {
            uint8_t bit = (uint8_t)(1 << (value & CHANNEL_MASK));
            c->mask = (value & SINGLE_MASK_SET) != 0 ? c->mask | bit : c->mask & (uint8_t)~bit;
            break;
        }
        case REG_MODE: {
            c->mode[value & CHANNEL_MASK] = value;
            break;
        }
        case REG_CLEAR_FLIP_FLOP: {
            c->high = false;
            break;
        }
        case REG_MASTER_CLEAR: {
            master_clear(c);
            break;
        }
        case REG_CLEAR_MASK: {
            c->mask = 0;
            break;
        }
        case REG_ALL_MASK: {
            c->mask = value & ALL_MASK_BITS;
            break;
        }
        default: {
            // The request register, whose requests nothing serves.
            break;
        }
    }
}

static void controller_access(
    struct vcpu *v,
    void *ctx,
    uint16_t offset,
    size_t size,
    bool in,
    uint32_t *value
) {
    (void)v;
    (void)size;
    struct controller *c = ctx;
    // The second controller's registers are a port apart.
    uint8_t reg = c == &controllers[0] ? offset & REGISTER_MASK : (offset >> 1) & REGISTER_MASK;
    spinlock_acquire(&lock);
    if (in) {
        *value = (*value & ~(uint32_t)0xff) | controller_read(c, reg);
    } else {
        controller_write(c, reg, (uint8_t)*value);
    }
    spinlock_release(&lock);
}

static void pages_access(
    struct vcpu *v,
    void *ctx,
    uint16_t offset,
    size_t size,
    bool in,
    uint32_t *value
) {
    (void)v;
    (void)size;
    uint16_t first = (uint16_t)(uintptr_t)ctx;
    uint8_t page = (uint8_t)((first + offset) & REGISTER_MASK);
    spinlock_acquire(&lock);
    if (in) {
        *value = (*value & ~(uint32_t)0xff) | pages[page];
    } else {
        pages[page] = (uint8_t)*value;
    }
    spinlock_release(&lock);
}

void isa_dma_init(void) {
    master_clear(&controllers[0]);
    master_clear(&controllers[1]);

    struct io_handler first = {
        FIRST_BASE, FIRST_COUNT, controller_access, &controllers[0], true
    };
    io_register(&first);
    struct io_handler second = {
        SECOND_BASE, SECOND_COUNT, controller_access, &controllers[1], true
    };
    io_register(&second);
    struct io_handler low_pages = {
        PAGES_BASE, PAGES_ALIAS_SKIP - PAGES_BASE, pages_access, (void *)(uintptr_t)PAGES_BASE,
        true
    };
    io_register(&low_pages);
    struct io_handler high_pages = {
        PAGES_ALIAS_SKIP + 1, PAGES_BASE + PAGES_COUNT - PAGES_ALIAS_SKIP - 1, pages_access,
        (void *)(uintptr_t)(PAGES_ALIAS_SKIP + 1), true
    };
    io_register(&high_pages);
}
