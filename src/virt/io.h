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

#include <stdint.h>
#include <stdbool.h>

struct vcpu;

struct io_handler {
    uint16_t base;
    uint16_t count;
    // offset is from base. value is in and out.
    void (*access)(struct vcpu *v, void *ctx, uint16_t offset, unsigned size, bool in, uint32_t *value);
    void *ctx;
};

// Traps the ports and routes them to the handler, for every vCPU.
void io_register(const struct io_handler *handler);

bool io_dispatch(struct vcpu *v, uint16_t port, unsigned size, bool in, uint32_t *value);

#endif
