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

#ifndef VIRT__INSN_H
#define VIRT__INSN_H

#include <stdbool.h>
#include <virt/vcpu.h>

#define INSN_MAX_LENGTH 15
// The opcodes of instructions outside the one-byte map: the legacy 0F map's
// as INSN_OPCODE_0F | the opcode byte, others' as INSN_OPCODE_OTHER.
#define INSN_OPCODE_0F 0xf00
#define INSN_OPCODE_OTHER 0x100

struct insn {
    unsigned length;
    unsigned opcode;
    // In bytes.
    unsigned operand_size;
    unsigned address_size;
    // Whether the code is 64-bit.
    bool long_mode;
    // An F2 or F3 prefix, and the last of them: F2 is REPNE, F3 REP or REPE.
    bool rep;
    uint8_t rep_prefix;
    bool lock;
    // The segment register an override prefix names, or -1.
    int segment;
    // The REX prefix, or 0.
    uint8_t rex;
    // Whether it is VEX, EVEX, or XOP encoded.
    bool vex;
    bool has_modrm;
    uint8_t modrm;
    bool has_sib;
    uint8_t sib;
    // Sign-extended, disp_size bytes of it encoded.
    int64_t disp;
    unsigned disp_size;
    // As it is encoded, imm_size bytes of it.
    uint64_t imm;
    unsigned imm_size;
};

// What a write to memory does, for those it can be emulated for.
#define INSN_STORE_MOV 0
#define INSN_STORE_XCHG 1
#define INSN_STORE_OR 2
#define INSN_STORE_AND 3

struct insn_store {
    unsigned op;
    // In bytes.
    unsigned size;
    // What is written, or combined with memory.
    uint64_t value;
    // The register the instruction names, which XCHG loads.
    unsigned reg;
    // Whether reg is AH, CH, DH, or BH.
    bool high_byte;
};

// What a read from memory does, for those it can be emulated for: MOV,
// MOVZX, and MOVSX load a register, TEST and CMP set the flags from memory
// and a register or immediate, and BT sets CF from a bit of memory.
#define INSN_LOAD_MOV 0
#define INSN_LOAD_MOVZX 1
#define INSN_LOAD_MOVSX 2
#define INSN_LOAD_TEST 3
#define INSN_LOAD_CMP 4
#define INSN_LOAD_BT 5

struct insn_load {
    unsigned op;
    // What is read, in bytes.
    unsigned size;
    // The register the instruction names, and its size in bytes.
    unsigned reg;
    unsigned reg_size;
    // Whether reg is AH, CH, DH, or BH.
    bool high_byte;
    // Whether an immediate takes the register's place.
    bool immediate;
    uint64_t imm;
    // Whether CMP subtracts memory from the register, not the other way.
    bool memory_second;
};

// The length of the instruction at the guest's CS:RIP, for intercepted
// instructions that only need skipping, on processors that do not report it.
// Returns 0 if it cannot be read or is not one of those instructions.
unsigned insn_length_intercepted(struct vcpu *v);

// Decodes any instruction at the guest's CS:RIP: its length, sizes, prefixes,
// and ModRM operand. Returns false if it cannot be read or is not a valid
// instruction.
bool insn_decode(struct vcpu *v, struct insn *insn);

// What a decoded instruction writes to memory, for MOV, XCHG, OR, and AND.
// Returns false for other instructions.
bool insn_store(struct vcpu *v, const struct insn *insn, struct insn_store *store);

// What a decoded instruction does with what it reads from memory, for MOV,
// MOVZX, MOVSX, TEST, CMP, and BT. Returns false for other instructions.
bool insn_load(const struct insn *insn, struct insn_load *load);
// Completes it, memory having held value.
void insn_load_complete(struct vcpu *v, const struct insn_load *load, uint64_t value);

// The linear address of a decoded instruction's ModRM memory operand, or of
// MOV's offset operand: false where it has neither. VEX, EVEX, and XOP forms
// are not handled.
bool insn_mem_linear(struct vcpu *v, const struct insn *insn, uint64_t *linear);
// A string instruction's linear address: its destination at ES:rDI, or its
// source at DS:rSI, or the segment an override names.
uint64_t insn_string_linear(struct vcpu *v, const struct insn *insn, bool destination);

// The guest's general register, as the encoding numbers them.
uint64_t insn_gpr(struct vcpu *v, unsigned reg);
void insn_set_gpr(struct vcpu *v, unsigned reg, uint64_t value);

// Completes a store to memory that held old: returns what it leaves there,
// and loads the register or sets the flags as the instruction does.
uint64_t insn_store_apply(struct vcpu *v, const struct insn_store *store, uint64_t old);

#endif
