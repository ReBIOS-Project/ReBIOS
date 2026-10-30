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
#include <virt/virt.h>

#define MAX_IO_HANDLERS 32

static struct io_handler handlers[MAX_IO_HANDLERS];
static size_t handler_count;

void io_register(const struct io_handler *handler) {
    if (handler_count == MAX_IO_HANDLERS) {
        panic("Too many I/O handlers\n");
    }
    handlers[handler_count++] = *handler;
    virt_intercept_io(handler->base, handler->count, true);
}

bool io_dispatch(
    struct vcpu *v, uint16_t port, size_t size, bool in, uint32_t *value
) {
    for (size_t i = 0; i < handler_count; i++) {
        struct io_handler *h = &handlers[i];
        if (port >= h->base && (size_t)(port - h->base) < h->count) {
            h->access(v, h->ctx, port - h->base, size, in, value);
            return true;
        }
    }
    return false;
}
