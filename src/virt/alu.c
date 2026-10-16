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

#include <stdint.h>
#include <virt/alu.h>

uint64_t alu_run(unsigned stub, uint64_t dst, uint64_t src, uint64_t *rflags);

uint64_t alu(unsigned op, unsigned size, uint64_t dst, uint64_t src, uint64_t *rflags) {
    unsigned index = size == 1 ? 0 : size == 2 ? 1 : size == 4 ? 2 : 3;
    return alu_run(op * 4 + index, dst, src, rflags);
}
