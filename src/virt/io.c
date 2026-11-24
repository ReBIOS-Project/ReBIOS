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
// You should have received a copy of the GNU General Public License along with
// ReBIOS. If not, see <https://www.gnu.org/licenses/>.

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

#define MAX_IO_BLOCK_HANDLERS 4

static struct io_block_handler block_handlers[MAX_IO_BLOCK_HANDLERS];
static size_t block_handler_count;

void io_register_block(const struct io_block_handler *handler) {
    if (block_handler_count == MAX_IO_BLOCK_HANDLERS) {
        panic("Too many I/O block handlers\n");
    }
    block_handlers[block_handler_count++] = *handler;
}

void io_dispatch_block(
    struct vcpu *v,
    uint16_t port,
    size_t size,
    bool in,
    uint8_t *data,
    size_t count
) {
    for (size_t i = 0; i < block_handler_count; i++) {
        const struct io_block_handler *h = &block_handlers[i];
        if (port >= h->base && (size_t)(port - h->base) < h->count) {
            h->access(v, h->ctx, (uint16_t)(port - h->base), size, in, data, count);
            return;
        }
    }
    for (size_t n = 0; n < count; n++) {
        uint32_t value = 0;
        for (size_t i = 0; i < size; i++) {
            value |= (uint32_t)data[n * size + i] << (8 * i);
        }
        io_dispatch(v, port, size, in, &value);
        if (in) {
            for (size_t i = 0; i < size; i++) {
                data[n * size + i] = (uint8_t)(value >> (8 * i));
            }
        }
    }
}

#define MAX_IO_WATCHES 8

struct io_watch {
    uint16_t base;
    size_t count;
    void (*seen)(void *ctx);
    void *ctx;
};

// count is 0 where unused.
static struct io_watch watches[MAX_IO_WATCHES];
// The ports any watch has, which every access the hypervisor dispatches
// checks.
static uint8_t watched[PORTS / 8];

static bool is_watched(uint16_t port) {
    return (watched[port / 8] & (1 << (port % 8))) != 0;
}

static const struct io_watch *watch_for(uint16_t port, size_t size) {
    bool any = false;
    for (size_t i = 0; i < size; i++) {
        any |= is_watched((uint16_t)(port + i));
    }
    if (!any) {
        return NULL;
    }
    for (size_t i = 0; i < MAX_IO_WATCHES; i++) {
        const struct io_watch *w = &watches[i];
        if (w->count != 0 && port < w->base + w->count && port + size > w->base) {
            return w;
        }
    }
    return NULL;
}

// Whether the port is trapped: where a handler has it, where the guest is kept
// from it, where a watch has it, and below 1000h where nothing the guest gets
// has it.
static bool trapped(uint16_t port) {
    return handler_for(port) != NULL || (denied[port / 8] & (1 << (port % 8))) != 0
        || is_watched(port) || (port < LEGACY_PORTS && users[port] == 0);
}

static void settle(uint16_t port) {
    virt_intercept_io(port, 1, trapped(port));
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

bool io_passed(uint16_t base, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (trapped((uint16_t)(base + i))) {
            return false;
        }
    }
    return true;
}

bool io_watch(uint16_t base, size_t count, void (*seen)(void *ctx), void *ctx) {
    for (size_t i = 0; i < MAX_IO_WATCHES; i++) {
        if (watches[i].count != 0) {
            continue;
        }
        watches[i] = (struct io_watch){ base, count, seen, ctx };
        for (size_t k = 0; k < count; k++) {
            uint16_t port = (uint16_t)(base + k);
            watched[port / 8] |= (uint8_t)(1 << (port % 8));
            settle(port);
        }
        return true;
    }
    return false;
}

void io_unwatch(uint16_t base, size_t count) {
    for (size_t i = 0; i < MAX_IO_WATCHES; i++) {
        if (watches[i].count != count || watches[i].base != base) {
            continue;
        }
        watches[i].count = 0;
        // The other watches keep the ports they share with it, as where the
        // guest makes two I/O BARs overlap.
        for (size_t k = 0; k < count; k++) {
            uint16_t port = (uint16_t)(base + k);
            watched[port / 8] &= (uint8_t)~(1 << (port % 8));
            for (size_t j = 0; j < MAX_IO_WATCHES; j++) {
                const struct io_watch *w = &watches[j];
                if (w->count != 0 && port >= w->base && (size_t)(port - w->base) < w->count) {
                    watched[port / 8] |= (uint8_t)(1 << (port % 8));
                }
            }
            settle(port);
        }
        return;
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
    const struct io_watch *w = watch_for(port, size);
    if (w != NULL) {
        w->seen(w->ctx);
    }
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
