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

#ifndef VIRT__INSN_MEM_TABLE_H
#define VIRT__INSN_MEM_TABLE_H

#include <stdint.h>
#include <virt/insn.h>

#define INSN_PREFIX_ANY 4
#define INSN_REG_ANY 8
#define INSN_W_ANY 2
#define INSN_L_ANY 4

#define INSN_MODE_64 0x01
#define INSN_MODE_OTHER 0x02

// How large a memory operand is: size bytes; size bytes at a vector length
// of 128 bits, doubling with each step of L; the operand size; the operand
// size, 64 bits by default in 64-bit mode (PUSH and POP); a far pointer, a
// selector after an offset of the operand size; BOUND's two bounds of the
// operand size; the x87 environment and state, 14 and 94 bytes with a 16-bit
// operand size, else 28 and 108; nothing, as only its address is used; or
// anything anywhere (XSAVE, gathers and scatters, AMX tiles).
#define INSN_SIZE_FIXED 0
#define INSN_SIZE_VECTOR 1
#define INSN_SIZE_OPERAND 2
#define INSN_SIZE_STACK 3
#define INSN_SIZE_FAR 4
#define INSN_SIZE_PAIR 5
#define INSN_SIZE_FPU_ENV 6
#define INSN_SIZE_FPU_STATE 7
#define INSN_SIZE_NONE 8
#define INSN_SIZE_UNKNOWN 9

// Instruction forms with a ModRM memory operand, sorted by space and opcode,
// the more specific forms of an opcode first.
struct insn_mem_form {
    uint8_t space;
    uint8_t opcode;
    uint8_t prefix;
    uint8_t reg;
    uint8_t w;
    uint8_t l;
    uint8_t modes;
    // INSN_MEM_*.
    uint8_t access;
    uint8_t size_kind;
    uint16_t size;
    // The element EVEX.b broadcasts, in bytes, or 0.
    uint8_t broadcast;
    // EVEX's disp8 scale where the operand's size is not it.
    uint8_t disp8_scale;
};

extern const struct insn_mem_form insn_mem_forms[];
extern const unsigned insn_mem_form_count;

#endif
