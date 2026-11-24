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
        struct vcpu *v,
        void *ctx,
        uint16_t offset,
        size_t size,
        bool in,
        uint32_t *value
    );
    void *ctx;
    // An 8-bit device's, which takes a wider access as an ISA bus runs it, one
    // byte at a time, each to the port it is for.
    bool bytes;
};

// Keeps the guest from the ports below 1000h, the ISA bus's and the chipset's
// legacy devices', but where io_register() or io_pass() say: the others read
// all ones and drop writes, as on a board without those devices. Before
// either.
void io_init(void);

// Traps the ports and routes them to the handler, for every vCPU.
void io_register(const struct io_handler *handler);

// Lets the guest reach the machine's ports, where no handler has them and
// io_block() did not keep it from them; undoes that, for a device whose ports
// moved; or keeps it from them for good, so that they read all ones and drop
// writes. While the guest runs, only between vcpu_pause_others() and
// vcpu_resume_others(), as the I/O bitmaps may change only with no vCPU in the
// guest (SDM 325462-093 Vol. 3C, 27.11.4).
void io_pass(uint16_t base, size_t count);
void io_unpass(uint16_t base, size_t count);
void io_block(uint16_t base, size_t count);
// Whether the guest reaches each of the machine's ports from base on, count of
// them, as io_pass() gave them, with no handler and no io_block() between.
bool io_passed(uint16_t base, size_t count);

// Traps the ports until the guest's first access to any of them, which calls
// seen() before it goes on as it would without the watch: seen() ends it with
// io_unwatch(). False where there are too many watches. As io_pass(), only
// between vcpu_pause_others() and vcpu_resume_others() while the guest runs.
bool io_watch(uint16_t base, size_t count, void (*seen)(void *ctx), void *ctx);
void io_unwatch(uint16_t base, size_t count);

// Runs an access the guest made: through the handler of its first port, or a
// byte at a time where that is an 8-bit device's or nobody's, each byte going
// to its port's handler, else to the machine for a port the guest is given,
// else nowhere, reading all ones.
void io_dispatch(struct vcpu *v, uint16_t port, size_t size, bool in, uint32_t *value);

// A handler that takes a string of accesses to its ports at once, each as its
// io_handler's access would take it, in order: data holds count elements of
// size bytes, little-endian, which accesses that read fill.
struct io_block_handler {
    uint16_t base;
    size_t count;
    void (*access)(
        struct vcpu *v,
        void *ctx,
        uint16_t offset,
        size_t size,
        bool in,
        uint8_t *data,
        size_t count
    );
    void *ctx;
};

// Adds one, for ports an io_handler has too.
void io_register_block(const struct io_block_handler *handler);

// count accesses to one port, as io_dispatch() runs each, through a block
// handler where one has the port's.
void io_dispatch_block(
    struct vcpu *v,
    uint16_t port,
    size_t size,
    bool in,
    uint8_t *data,
    size_t count
);

#endif
