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

#ifndef VIRT__INSN_H
#define VIRT__INSN_H

#include <stdbool.h>
#include <virt/vcpu.h>

#define INSN_MAX_LENGTH 15
// The opcodes of instructions outside the one-byte map: the legacy 0F map's
// as INSN_OPCODE_0F | the opcode byte, others' as INSN_OPCODE_OTHER.
#define INSN_OPCODE_0F 0xf00
#define INSN_OPCODE_OTHER 0x100

// Opcode spaces: an encoding ORed with the number of its opcode map.
#define INSN_SPACE_LEGACY 0x00
#define INSN_SPACE_VEX 0x10
#define INSN_SPACE_EVEX 0x20
#define INSN_SPACE_XOP 0x30

// Mandatory prefixes, numbered as VEX's pp field numbers them.
#define INSN_PREFIX_NONE 0
#define INSN_PREFIX_66 1
#define INSN_PREFIX_F3 2
#define INSN_PREFIX_F2 3

// What an instruction does with a memory operand. A masked access is of only
// the elements a mask selects.
#define INSN_MEM_READ 0x01
#define INSN_MEM_WRITE 0x02
#define INSN_MEM_MASKED 0x04

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
    // The opcode space and the opcode byte in it.
    uint8_t space;
    uint8_t opcode_byte;
    // The mandatory prefix, W (REX.W in legacy encodings), and L.
    uint8_t prefix;
    bool w;
    uint8_t vector_length;
    // EVEX's broadcast bit and opmask register.
    bool evex_b;
    uint8_t evex_aaa;
};

struct insn_mem {
    uint64_t linear;
    unsigned size;
    // INSN_MEM_*.
    unsigned access;
};

// The length of the instruction at the guest's CS:RIP, for intercepted
// instructions that only need skipping, on processors that do not report it.
// Returns 0 if it cannot be read or is not one of those instructions.
unsigned insn_length_intercepted(struct vcpu *v);

// Decodes any instruction at the guest's CS:RIP: its length, sizes, prefixes,
// and ModRM operand. Returns false if it cannot be read or is not a valid
// instruction.
bool insn_decode(struct vcpu *v, struct insn *insn);

// The linear address of a decoded instruction's ModRM memory operand, or of
// MOV's offset operand: false where it has neither. VEX, EVEX, and XOP forms
// are not handled.
bool insn_mem_linear(struct vcpu *v, const struct insn *insn, uint64_t *linear);
// Where an instruction's ModRM memory operand is, and which of its bytes the
// instruction reads and writes, as the Intel SDM and the AMD APM describe the
// instruction: none where it only uses the operand's address, as CLFLUSH.
// Returns false where the instruction has no such operand, or they do not say.
bool insn_mem_operand(struct vcpu *v, const struct insn *insn, struct insn_mem *mem);
// A string instruction's linear address: its destination at ES:rDI, or its
// source at DS:rSI, or the segment an override names.
uint64_t insn_string_linear(struct vcpu *v, const struct insn *insn, bool destination);

// The guest's general register, as the encoding numbers them.
uint64_t insn_gpr(struct vcpu *v, unsigned reg);
void insn_set_gpr(struct vcpu *v, unsigned reg, uint64_t value);

#endif
