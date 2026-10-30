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

#ifndef VIRT__ALU_H
#define VIRT__ALU_H

#include <stddef.h>
#include <stdint.h>

// Operations the processor runs for emulated instructions, so that results and
// flags, undefined ones included, are exactly its own. Group 1 and group 2 are
// in their ModRM reg field orders, and BT to BTC in group 8's from /4.
#define ALU_ADD 0
#define ALU_OR 1
#define ALU_ADC 2
#define ALU_SBB 3
#define ALU_AND 4
#define ALU_SUB 5
#define ALU_XOR 6
#define ALU_CMP 7
#define ALU_ROL 8
#define ALU_ROR 9
#define ALU_RCL 10
#define ALU_RCR 11
#define ALU_SHL 12
#define ALU_SHR 13
#define ALU_SAL 14
#define ALU_SAR 15
#define ALU_TEST 16
#define ALU_NOT 17
#define ALU_NEG 18
#define ALU_INC 19
#define ALU_DEC 20
#define ALU_BT 21
#define ALU_BTS 22
#define ALU_BTR 23
#define ALU_BTC 24

// Runs operation op, of size 1, 2, 4, or 8 bytes but for BT to BTC, on dst and
// src, src being the count of shifts and the bit offset of bit tests, with the
// status flags in *rflags, which it updates. Returns the result, which only
// the low size bytes of hold.
uint64_t alu(
    unsigned int op, size_t size, uint64_t dst, uint64_t src, uint64_t *rflags
);

#endif
