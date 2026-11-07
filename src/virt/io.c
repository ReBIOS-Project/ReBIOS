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
#include <virt/io.h>
#include <lib/log.h>
#include <sys/x86.h>
#include <virt/virt.h>

#define MAX_IO_HANDLERS 32
#define LEGACY_PORTS 0x1000
#define PORTS 0x10000

static struct io_handler handlers[MAX_IO_HANDLERS];
static size_t handler_count;
// The ports io_block() keeps from the guest, whatever gives them.
static uint8_t denied[PORTS / 8];
// How many of what the guest gets have each port below 1000h.
static uint8_t users[LEGACY_PORTS];

void io_init(void) {
    virt_intercept_io(0, LEGACY_PORTS, true);
}

void io_register(const struct io_handler *handler) {
    if (handler_count == MAX_IO_HANDLERS) {
        panic("Too many I/O handlers\n");
    }
    handlers[handler_count++] = *handler;
    virt_intercept_io(handler->base, handler->count, true);
}

static const struct io_handler *handler_for(uint16_t port);

// Traps the port where a handler has it, where the guest is kept from it, and
// below 1000h where nothing the guest gets has it.
static void settle(uint16_t port) {
    bool trapped = handler_for(port) != NULL || (denied[port / 8] & (1 << (port % 8))) != 0
                || (port < LEGACY_PORTS && users[port] == 0);
    virt_intercept_io(port, 1, trapped);
}

void io_pass(uint16_t base, size_t count) {
    for (size_t i = 0; i < count; i++) {
        uint16_t port = (uint16_t)(base + i);
        if (port < LEGACY_PORTS && users[port] != UINT8_MAX) {
            users[port]++;
        }
        settle(port);
    }
}

void io_block(uint16_t base, size_t count) {
    for (size_t i = 0; i < count; i++) {
        uint16_t port = (uint16_t)(base + i);
        denied[port / 8] |= (uint8_t)(1 << (port % 8));
        settle(port);
    }
}

void io_unpass(uint16_t base, size_t count) {
    for (size_t i = 0; i < count; i++) {
        uint16_t port = (uint16_t)(base + i);
        // A count that ran out stays.
        if (port < LEGACY_PORTS && users[port] != 0 && users[port] != UINT8_MAX) {
            users[port]--;
        }
        settle(port);
    }
}

static const struct io_handler *handler_for(uint16_t port) {
    for (size_t i = 0; i < handler_count; i++) {
        const struct io_handler *h = &handlers[i];
        if (port >= h->base && (size_t)(port - h->base) < h->count) {
            return h;
        }
    }
    return NULL;
}

static uint32_t native(uint16_t port, size_t size, bool in, uint32_t value) {
    switch (size) {
        case 1: {
            if (in) {
                return inb(port);
            }
            outb(port, (uint8_t)value);
            break;
        }
        case 2: {
            if (in) {
                return inw(port);
            }
            outw(port, (uint16_t)value);
            break;
        }
        default: {
            if (in) {
                return inl(port);
            }
            outl(port, value);
            break;
        }
    }
    return value;
}

static bool all_given(uint16_t port, size_t size) {
    for (size_t i = 0; i < size; i++) {
        if (virt_io_intercepted((uint16_t)(port + i))) {
            return false;
        }
    }
    return true;
}

void io_dispatch(struct vcpu *v, uint16_t port, size_t size, bool in, uint32_t *value) {
    const struct io_handler *h = handler_for(port);
    if (h != NULL && !h->bytes) {
        h->access(v, h->ctx, (uint16_t)(port - h->base), size, in, value);
        return;
    }
    // Only a nested guest's accesses exit whole on ports the guest is given.
    if (h == NULL && all_given(port, size)) {
        *value = native(port, size, in, *value);
        return;
    }
    for (size_t i = 0; i < size; i++) {
        uint16_t p = (uint16_t)(port + i);
        uint32_t byte = (*value >> (8 * i)) & 0xff;
        const struct io_handler *owner = handler_for(p);
        if (owner != NULL) {
            owner->access(v, owner->ctx, (uint16_t)(p - owner->base), 1, in, &byte);
        } else if (!virt_io_intercepted(p)) {
            byte = native(p, 1, in, byte);
        } else {
            byte = 0xff;
        }
        if (in) {
            *value = (*value & ~((uint32_t)0xff << (8 * i))) | (byte & 0xff) << (8 * i);
        }
    }
}
