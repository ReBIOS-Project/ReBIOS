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

#ifndef VIRT__IO_H
#define VIRT__IO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct vcpu;

struct io_handler {
    uint16_t base;
    size_t count;
    // offset is from base. value is in and out.
    void (*access)(
        struct vcpu *v, void *ctx, uint16_t offset, size_t size, bool in, uint32_t *value
    );
    void *ctx;
    // An 8-bit device's, which takes a wider access as an ISA bus runs it, one
    // byte at a time, each to the port it is for.
    bool bytes;
};

// Traps the ports and routes them to the handler, for every vCPU.
void io_register(const struct io_handler *handler);

// Lets the guest reach the machine's ports, where no handler has them, or
// keeps it from them, which then read all ones and drop writes.
void io_pass(uint16_t base, size_t count);
void io_block(uint16_t base, size_t count);

// Runs an access the guest made: through the handler of its first port, or a
// byte at a time where that is an 8-bit device's or nobody's, each byte going
// to its port's handler, else to the machine for a port the guest is given,
// else nowhere, reading all ones.
void io_dispatch(struct vcpu *v, uint16_t port, size_t size, bool in, uint32_t *value);

#endif
