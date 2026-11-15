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

#include <stddef.h>
#include <stdint.h>
#include <virt/alu.h>

uint64_t alu_run(unsigned int stub, uint64_t dst, uint64_t src, uint64_t *rflags);
uint64_t alu_run_wide(
    unsigned int stub,
    uint64_t rax,
    uint64_t src,
    uint64_t *rdx,
    uint64_t *rflags
);
uint64_t alu_run_double(
    unsigned int stub,
    uint64_t dst,
    uint64_t src,
    uint64_t count,
    uint64_t *rflags
);

// An operand size's index among each operation's four stubs, for 1, 2, 4, and
// 8 bytes.
static size_t size_index(size_t size) {
    switch (size) {
        case 1: {
            return 0;
        }
        case 2: {
            return 1;
        }
        case 4: {
            return 2;
        }
        default: {
            return 3;
        }
    }
}

uint64_t alu(unsigned int op, size_t size, uint64_t dst, uint64_t src, uint64_t *rflags) {
    return alu_run(op * 4 + size_index(size), dst, src, rflags);
}

uint64_t alu_double(
    unsigned int op,
    size_t size,
    uint64_t dst,
    uint64_t src,
    uint8_t count,
    uint64_t *rflags
) {
    return alu_run_double(op * 4 + size_index(size), dst, src, count, rflags);
}

uint64_t alu_wide(
    unsigned int op,
    size_t size,
    uint64_t rax,
    uint64_t *rdx,
    uint64_t src,
    uint64_t *rflags
) {
    return alu_run_wide(op * 4 + size_index(size), rax, src, rdx, rflags);
}
