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
#include <virt/insn.h>
#include <memory.h>
#include <sys/x86.h>
#include <virt/guest_mem.h>
#include <virt/insn_mem_table.h>
#include <virt/vcpu.h>

static bool is_legacy_prefix(uint8_t b) {
    switch (b) {
        case 0x26:
        case 0x2e:
        case 0x36:
        case 0x3e:
        case 0x64:
        case 0x65:
        case 0x66:
        case 0x67:
        case 0xf0:
        case 0xf2:
        case 0xf3: {
            return true;
        }
        default: {
            return false;
        }
    }
}

unsigned int insn_length_intercepted(struct vcpu *v) {
    uint32_t cs = vcpu_seg_attrib(v, VCPU_SEG_CS);
    bool long_mode = (vcpu_efer(v) & EFER_LMA) != 0 && (cs & VCPU_SEG_L) != 0;
    uintptr_t linear = vcpu_rip(v);
    if (!long_mode) {
        linear = (vcpu_seg_base(v, VCPU_SEG_CS) + linear) & 0xffffffff;
    }

    uint8_t bytes[15];
    if (!guest_mem_linear_read(v, linear, bytes, sizeof(bytes), true)) {
        // The instruction may end before a page that is not mapped.
        unsigned int avail = 4096 - (linear & 4095);
        if (avail >= sizeof(bytes) || !guest_mem_linear_read(v, linear, bytes, avail, true)) {
            return 0;
        }
    }

    size_t i = 0;
    while (i < sizeof(bytes) && is_legacy_prefix(bytes[i])) {
        i++;
    }
    if (long_mode && i < sizeof(bytes) && (bytes[i] & 0xf0) == 0x40) {
        i++;
    }
    if (i + 3 > sizeof(bytes)) {
        return 0;
    }

    if (bytes[i] == 0xf4) {
        return i + 1;
    }
    if (bytes[i] != 0x0f) {
        return 0;
    }
    switch (bytes[i + 1]) {
        case 0x08:
        case 0x09:
        case 0x30:
        case 0x32:
        case 0xa2: {
            return i + 2;
        }
        case 0x01: {
            uint8_t modrm = bytes[i + 2];
            if (modrm == 0xc1 || modrm == 0xd1 || (modrm >= 0xd8 && modrm <= 0xdf)) {
                return i + 3;
            }
            return 0;
        }
        default: {
            return 0;
        }
    }
}

// What follows an opcode: whether a ModRM byte does, and which immediate.
#define OP_MODRM 0x10
#define OP_IMM_MASK 0x0f
#define IMM_NONE 0
#define IMM_B 1
#define IMM_W 2
// 2 bytes with 16-bit operands, 4 otherwise.
#define IMM_Z 3
// As large as the operand.
#define IMM_V 4
// A far pointer: an offset as IMM_Z, then a selector.
#define IMM_AP 5
// An offset as large as an address.
#define IMM_O 6
// ENTER's 16-bit size and 8-bit level.
#define IMM_WB 7
// A near branch's displacement: 4 bytes in 64-bit mode, else as IMM_Z.
#define IMM_J 8
// Escapes and prefixes, which the decoder handles before the tables.
#define OP_SPECIAL 0x20

#define M OP_MODRM
#define B IMM_B
#define W IMM_W
#define Z IMM_Z
#define V IMM_V
#define AP IMM_AP
#define O IMM_O
#define WB IMM_WB
#define J IMM_J
#define S OP_SPECIAL

static const uint8_t one_byte[256] = {
    M, M, M, M, B, Z, 0, 0, M, M, M, M, B, Z, 0, S,
    M, M, M, M, B, Z, 0, 0, M, M, M, M, B, Z, 0, 0,
    M, M, M, M, B, Z, S, 0, M, M, M, M, B, Z, S, 0,
    M, M, M, M, B, Z, S, 0, M, M, M, M, B, Z, S, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, M, M, S, S, S, S, Z, M | Z, B, M | B, 0, 0, 0, 0,
    B, B, B, B, B, B, B, B, B, B, B, B, B, B, B, B,
    M | B, M | Z, M | B, M | B, M, M, M, M, M, M, M, M, M, M, M, M,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, AP, 0, 0, 0, 0, 0,
    O, O, O, O, 0, 0, 0, 0, B, Z, 0, 0, 0, 0, 0, 0,
    B, B, B, B, B, B, B, B, V, V, V, V, V, V, V, V,
    M | B, M | B, W, 0, M, M, M | B, M | Z, WB, 0, W, 0, 0, B, 0, 0,
    M, M, M, M, B, B, 0, 0, M, M, M, M, M, M, M, M,
    B, B, B, B, B, B, B, B, J, J, AP, B, 0, 0, 0, 0,
    S, 0, S, S, 0, 0, M, M, 0, 0, 0, 0, 0, 0, M, M
};

// The 0F map. 0F 38 and 0F 3A escape to maps of their own.
static const uint8_t two_byte[256] = {
    M, M, M, M, 0, 0, 0, 0, 0, 0, 0, 0, 0, M, 0, M | B,
    M, M, M, M, M, M, M, M, M, M, M, M, M, M, M, M,
    M, M, M, M, M, M, M, M, M, M, M, M, M, M, M, M,
    0, 0, 0, 0, 0, 0, 0, 0, S, 0, S, 0, 0, 0, 0, 0,
    M, M, M, M, M, M, M, M, M, M, M, M, M, M, M, M,
    M, M, M, M, M, M, M, M, M, M, M, M, M, M, M, M,
    M, M, M, M, M, M, M, M, M, M, M, M, M, M, M, M,
    M | B, M | B, M | B, M | B, M, M, M, 0, M, M, M, M, M, M, M, M,
    J, J, J, J, J, J, J, J, J, J, J, J, J, J, J, J,
    M, M, M, M, M, M, M, M, M, M, M, M, M, M, M, M,
    0, 0, 0, M, M | B, M, M, M, 0, 0, 0, M, M | B, M, M, M,
    M, M, M, M, M, M, M, M, M, M, M | B, M, M, M, M, M,
    M, M, M | B, M, M | B, M | B, M | B, M, 0, 0, 0, 0, 0, 0, 0, 0,
    M, M, M, M, M, M, M, M, M, M, M, M, M, M, M, M,
    M, M, M, M, M, M, M, M, M, M, M, M, M, M, M, M,
    M, M, M, M, M, M, M, M, M, M, M, M, M, M, M, M
};

#undef M
#undef B
#undef W
#undef Z
#undef V
#undef AP
#undef O
#undef WB
#undef J
#undef S

#define MAP_ONE 0
#define MAP_0F 1
#define MAP_0F38 2
#define MAP_0F3A 3
#define MAP_EVEX4 4

// EVEX map 4's immediates, those of its instructions' map 0 opcodes, and of
// SHLD and SHRD at 24 and 2C (Intel APX 355828-009, 3.1.5).
static unsigned int evex4_imm(uint8_t op) {
    switch (op) {
        case 0x24:
        case 0x2c:
        case 0x6b:
        case 0x80:
        case 0x83:
        case 0xc0:
        case 0xc1: {
            return IMM_B;
        }
        case 0x69:
        case 0x81: {
            return IMM_Z;
        }
        default: {
            return IMM_NONE;
        }
    }
}

// Whether REX2 may prefix an opcode: not those of the rows with no register to
// extend, JMPABS at A1 aside, nor the prefixes that cannot follow it, as none
// is an opcode in 64-bit mode (Intel APX 355828-009, 3.1.2.1).
static bool rex2_allowed(unsigned int map, uint8_t op) {
    unsigned int row = op >> 4;
    if (map == MAP_0F) {
        return row != 3 && row != 8;
    }
    if (op == 0x62 || op == 0xc4 || op == 0xc5 || op == 0xd5) {
        return false;
    }
    return (row != 4 && row != 7 && row != 0xa && row != 0xe) || op == 0xa1;
}

static size_t imm_size(unsigned int kind, const struct insn *insn, bool long_mode) {
    switch (kind) {
        case IMM_B: {
            return 1;
        }
        case IMM_W: {
            return 2;
        }
        case IMM_Z: {
            return insn->operand_size == 2 ? 2 : 4;
        }
        case IMM_V: {
            return insn->operand_size;
        }
        case IMM_AP: {
            return (insn->operand_size == 2 ? 2 : 4) + 2;
        }
        case IMM_O: {
            return insn->address_size;
        }
        case IMM_WB: {
            return 3;
        }
        case IMM_J: {
            return long_mode || insn->operand_size != 2 ? 4 : 2;
        }
        default: {
            return 0;
        }
    }
}

static int64_t read_signed(const uint8_t *bytes, size_t size) {
    uint64_t value = 0;
    for (size_t n = 0; n < size; n++) {
        value |= (uint64_t)bytes[n] << (n * 8);
    }
    unsigned int shift = 64 - size * 8;
    return (int64_t)(value << shift) >> shift;
}

// Decodes a ModRM byte at bytes[i] and what it brings with it: returns their
// size, the ModRM byte included.
static unsigned int decode_modrm(const uint8_t *bytes, size_t i, struct insn *insn) {
    uint8_t modrm = bytes[i];
    unsigned int mod = modrm >> 6;
    unsigned int rm = modrm & 7;
    insn->has_modrm = true;
    insn->modrm = modrm;
    if (mod == 3) {
        return 1;
    }
    size_t size = 1;
    size_t disp_size;
    if (insn->address_size == 2) {
        disp_size = mod == 1 ? 1 : mod == 2 || rm == 6 ? 2 : 0;
    } else {
        if (rm == 4) {
            insn->has_sib = true;
            insn->sib = bytes[i + 1];
            size++;
        }
        bool no_base = mod == 0 && (rm == 5 || (rm == 4 && (insn->sib & 7) == 5));
        disp_size = mod == 1 ? 1 : mod == 2 || no_base ? 4 : 0;
    }
    insn->disp_size = disp_size;
    insn->disp = read_signed(bytes + i + size, disp_size);
    return size + disp_size;
}

// VEX's, EVEX's, and XOP's inverted R, X, and B, as the REX prefix they stand
// in for. Outside 64-bit mode, addresses only use the first 8 registers.
static void vex_rex(struct insn *insn, uint8_t rxb, bool w, bool long_mode) {
    insn->w = w;
    if (long_mode) {
        insn->rex = 0x40 | (w ? 0x08 : 0) | ((~rxb >> 5) & 0x07);
    }
}

bool insn_decode(struct vcpu *v, struct insn *insn) {
    uint32_t cs = vcpu_seg_attrib(v, VCPU_SEG_CS);
    bool long_mode = (vcpu_efer(v) & EFER_LMA) != 0 && (cs & VCPU_SEG_L) != 0;
    // CS's descriptor gives the defaults, in real mode too, where its cached D
    // bit outlives protected mode, as Linux 7.3's KVM decodes (emulate.c,
    // x86_decode_insn()); V86 mode's are 16-bit.
    bool big = (vcpu_rflags(v) & RFLAGS_VM) == 0 && (cs & VCPU_SEG_DB) != 0;
    uintptr_t linear = vcpu_rip(v);
    if (!long_mode) {
        linear = (vcpu_seg_base(v, VCPU_SEG_CS) + linear) & 0xffffffff;
    }

    // Zeroes past what can be read make the decoding stop short, as the
    // instruction then cannot be complete.
    uint8_t bytes[INSN_MAX_LENGTH + 4] = { 0 };
    if (v->fetched_count == INSN_MAX_LENGTH && v->fetched_rip == vcpu_rip(v)) {
        memcpy(bytes, v->fetched, INSN_MAX_LENGTH);
    } else if (!guest_mem_linear_read(v, linear, bytes, INSN_MAX_LENGTH, true)) {
        unsigned int avail = 4096 - (linear & 4095);
        if (avail >= INSN_MAX_LENGTH || !guest_mem_linear_read(v, linear, bytes, avail, true)) {
            return false;
        }
    }

    unsigned int operand = long_mode || big ? 4 : 2;
    size_t address = long_mode ? 8 : big ? 4 : 2;
    bool operand_prefix = false;
    bool address_prefix = false;
    bool rex_w = false;
    uint8_t rex = 0;
    *insn = (struct insn){ .long_mode = long_mode, .segment = -1 };

    size_t i = 0;
    for (;;) {
        uint8_t b = bytes[i];
        if (is_legacy_prefix(b)) {
            operand_prefix |= b == 0x66;
            address_prefix |= b == 0x67;
            insn->lock |= b == 0xf0;
            if (b == 0xf2 || b == 0xf3) {
                insn->rep = true;
                insn->rep_prefix = b;
            }
            // In 64-bit mode, only FS and GS overrides have an effect.
            int segment = -1;
            switch (b) {
                case 0x26: {
                    segment = VCPU_SEG_ES;
                    break;
                }
                case 0x2e: {
                    segment = VCPU_SEG_CS;
                    break;
                }
                case 0x36: {
                    segment = VCPU_SEG_SS;
                    break;
                }
                case 0x3e: {
                    segment = VCPU_SEG_DS;
                    break;
                }
                case 0x64: {
                    segment = VCPU_SEG_FS;
                    break;
                }
                case 0x65: {
                    segment = VCPU_SEG_GS;
                    break;
                }
            }
            if (segment >= 0 && (!long_mode || segment == VCPU_SEG_FS || segment == VCPU_SEG_GS)) {
                insn->segment = segment;
            }
            // A REX prefix only counts right before the opcode.
            rex_w = false;
            rex = 0;
            i++;
        } else if (long_mode && (b & 0xf0) == 0x40) {
            rex_w = (b & 0x08) != 0;
            rex = b;
            i++;
        } else {
            break;
        }
        if (i >= INSN_MAX_LENGTH) {
            return false;
        }
    }
    // REX2 is the last prefix, which D5 is in 64-bit mode, where it is no AAD,
    // and a REX prefix cannot come right before it; the opcode follows it in
    // the map its M0 bit gives, with no escape (Intel APX 355828-009,
    // 3.1.2.1).
    bool rex2 = long_mode && bytes[i] == 0xd5;
    unsigned int rex2_map = MAP_ONE;
    if (rex2) {
        if (rex != 0) {
            return false;
        }
        uint8_t payload = bytes[i + 1];
        rex = 0x40 | (payload & 0x0f);
        rex_w = (payload & 0x08) != 0;
        insn->rex_upper = (payload >> 4) & 0x07;
        rex2_map = (payload & 0x80) != 0 ? MAP_0F : MAP_ONE;
        i += 2;
    }
    if (rex_w) {
        operand = 8;
    } else if (operand_prefix) {
        operand = operand == 2 ? 4 : 2;
    }
    if (address_prefix) {
        address = address == 4 ? 2 : 4;
    }
    insn->operand_size = operand;
    insn->address_size = address;
    insn->rex = rex;
    insn->w = rex_w;
    if (insn->rep) {
        insn->prefix = insn->rep_prefix == 0xf2 ? INSN_PREFIX_F2 : INSN_PREFIX_F3;
    } else if (operand_prefix) {
        insn->prefix = INSN_PREFIX_66;
    }

    unsigned int map = MAP_ONE;
    uint8_t op = bytes[i];
    // VEX, EVEX, and XOP take the place of LES, LDS, BOUND, and POP Ev where a
    // ModRM byte could not follow.
    bool vex = long_mode || (bytes[i + 1] & 0xc0) == 0xc0;
    bool vex_like = false;
    if (rex2) {
        map = rex2_map;
        insn->space = INSN_SPACE_LEGACY | map;
    } else if (op == 0xc5 && vex) {
        // R vvvv L pp, with X and B clear and W 0.
        uint8_t b = bytes[i + 1];
        vex_rex(insn, b | 0x60, false, long_mode);
        insn->vector_length = (b >> 2) & 1;
        insn->prefix = b & 3;
        map = MAP_0F;
        insn->space = INSN_SPACE_VEX | map;
        i += 2;
        vex_like = true;
    } else if (op == 0xc4 && vex) {
        // R X B mmmmm, then W vvvv L pp.
        vex_rex(insn, bytes[i + 1], (bytes[i + 2] >> 7) != 0, long_mode);
        insn->vector_length = (bytes[i + 2] >> 2) & 1;
        insn->prefix = bytes[i + 2] & 3;
        map = bytes[i + 1] & 0x1f;
        insn->space = INSN_SPACE_VEX | map;
        i += 3;
        vex_like = true;
    } else if (op == 0x62 && vex) {
        // R X B R' B4 mmm, W vvvv U pp, then z L'L b V' aaa, where in 64-bit
        // mode B4, and R' and U inverted, are APX's B4, R4, and X4, and map 4
        // has ND for b and NF for aaa's bit 2, its W and pp giving the operand
        // size as REX.W and 66 do (Intel APX 355828-009, 3.1.2.3).
        uint8_t p0 = bytes[i + 1];
        uint8_t p1 = bytes[i + 2];
        uint8_t p2 = bytes[i + 3];
        vex_rex(insn, p0, (p1 >> 7) != 0, long_mode);
        if (long_mode) {
            insn->rex_upper = ((p0 & 0x10) == 0 ? INSN_REX_R : 0)
                            | ((p1 & 0x04) == 0 ? INSN_REX_X : 0)
                            | ((p0 & 0x08) != 0 ? INSN_REX_B : 0);
        }
        insn->prefix = p1 & 3;
        insn->vector_length = (p2 >> 5) & 3;
        map = p0 & 0x07;
        if (map == MAP_EVEX4) {
            insn->nd = (p2 & 0x10) != 0;
            insn->nf = (p2 & 0x04) != 0;
            if (insn->w) {
                insn->operand_size = 8;
            } else {
                insn->operand_size = insn->prefix == INSN_PREFIX_66 ? 2 : 4;
            }
        } else {
            insn->evex_b = (p2 & 0x10) != 0;
            insn->evex_aaa = p2 & 7;
        }
        insn->space = INSN_SPACE_EVEX | map;
        i += 4;
        vex_like = true;
    } else if (op == 0x8f && (bytes[i + 1] & 0x1f) >= 8) {
        // XOP, laid out as VEX's three bytes: map 8 takes an 8-bit immediate,
        // map 10 a 32-bit one.
        unsigned int xop_map = bytes[i + 1] & 0x1f;
        vex_rex(insn, bytes[i + 1], (bytes[i + 2] >> 7) != 0, long_mode);
        insn->vector_length = (bytes[i + 2] >> 2) & 1;
        insn->prefix = bytes[i + 2] & 3;
        insn->space = INSN_SPACE_XOP | xop_map;
        i += 3;
        insn->opcode_byte = bytes[i++];
        i += decode_modrm(bytes, i, insn);
        i += xop_map == 8 ? 1 : xop_map == 10 ? 4 : 0;
        insn->opcode = INSN_OPCODE_OTHER;
        insn->vex = true;
        insn->length = i;
        return i <= INSN_MAX_LENGTH;
    } else if (op == 0x0f) {
        i++;
        op = bytes[i];
        map = MAP_0F;
        if (op == 0x38 || op == 0x3a) {
            map = op == 0x38 ? MAP_0F38 : MAP_0F3A;
            i++;
        }
        insn->space = INSN_SPACE_LEGACY | map;
    }
    if (i >= INSN_MAX_LENGTH) {
        return false;
    }
    op = bytes[i++];
    insn->opcode_byte = op;
    bool evex4 = (insn->space & 0xf0) == INSN_SPACE_EVEX && map == MAP_EVEX4;

    uint8_t entry;
    if (map == MAP_ONE) {
        entry = one_byte[op];
        insn->opcode = op;
    } else if (map == MAP_0F) {
        entry = two_byte[op];
        // VZEROUPPER and VZEROALL have no ModRM byte.
        if (vex_like && op == 0x77) {
            entry = 0;
        } else if (vex_like) {
            entry |= OP_MODRM;
        }
        insn->opcode = vex_like ? INSN_OPCODE_OTHER : INSN_OPCODE_0F | op;
    } else if (map == MAP_0F3A) {
        entry = OP_MODRM | IMM_B;
        insn->opcode = INSN_OPCODE_OTHER;
    } else if (evex4) {
        // Each instruction there has a ModRM byte (Intel APX 355828-009,
        // 3.1.2.3.1).
        entry = OP_MODRM | evex4_imm(op);
        insn->opcode = INSN_OPCODE_OTHER;
    } else {
        // 0F 38, and the EVEX maps 5 and 6.
        entry = OP_MODRM;
        insn->opcode = INSN_OPCODE_OTHER;
    }
    if ((entry & OP_SPECIAL) != 0 || (rex2 && !rex2_allowed(map, op))) {
        return false;
    }
    // JMPABS's 64-bit target is no offset of MOV's (Intel APX 355828-009,
    // 7.1).
    if (rex2 && map == MAP_ONE && op == 0xa1) {
        insn->opcode = INSN_OPCODE_OTHER;
    }

    insn->vex = vex_like;
    unsigned int imm = entry & OP_IMM_MASK;
    if ((entry & OP_MODRM) != 0) {
        // TEST in group 3 is the only member with an immediate, as CTEST is in
        // EVEX map 4's.
        if ((map == MAP_ONE || evex4) && (op == 0xf6 || op == 0xf7) && ((bytes[i] >> 3) & 7) < 2) {
            imm = op == 0xf6 ? IMM_B : IMM_Z;
        }
        i += decode_modrm(bytes, i, insn);
    }
    insn->imm_size = imm_size(imm, insn, long_mode);
    for (size_t n = 0; n < insn->imm_size && n < sizeof(uint64_t); n++) {
        insn->imm |= (uint64_t)bytes[i + n] << (n * 8);
    }
    i += insn->imm_size;
    insn->length = i;
    return i <= INSN_MAX_LENGTH;
}

static uint64_t address_mask(size_t size) {
    return size == 8 ? UINT64_MAX : ((uint64_t)1 << (size * 8)) - 1;
}

// Outside 64-bit mode the linear address space wraps at 4 GiB, and in it only
// FS and GS have bases.
static uintptr_t segment_linear(struct vcpu *v, bool long_mode, int reg, uintptr_t ea) {
    if (long_mode) {
        if (reg != VCPU_SEG_FS && reg != VCPU_SEG_GS) {
            return vcpu_untagged(v, (size_t)reg, ea);
        }
        return vcpu_untagged(v, (size_t)reg, vcpu_seg_base(v, (size_t)reg) + ea);
    }
    return (vcpu_seg_base(v, (size_t)reg) + ea) & 0xffffffff;
}

// The memory operand's effective address, and the segment it is in.
static bool mem_ea(
    struct vcpu *v,
    const struct insn *insn,
    int64_t disp,
    int *segment_out,
    uint64_t *ea_out
) {
    int segment = VCPU_SEG_DS;
    uintptr_t ea;
    if (insn->opcode >= 0xa0 && insn->opcode <= 0xa3) {
        ea = insn->imm;
    } else if (!insn->has_modrm || insn->modrm >> 6 == 3) {
        return false;
    } else if (insn->address_size == 2) {
        unsigned int mod = insn->modrm >> 6;
        unsigned int rm = insn->modrm & 7;
        uint64_t bx = vcpu_gpr(v, 3);
        uint64_t bp = vcpu_gpr(v, 5);
        uint64_t si = vcpu_gpr(v, 6);
        uint64_t di = vcpu_gpr(v, 7);
        static const uint8_t base_is_bp = (uint32_t)1 << 2 | (uint32_t)1 << 3 | (uint32_t)1 << 6;
        uint64_t sums[8] = { bx + si, bx + di, bp + si, bp + di, si, di, bp, bx };
        ea = (uint64_t)disp;
        if (rm != 6 || mod != 0) {
            ea += sums[rm];
            if (((base_is_bp >> rm) & 1) != 0) {
                segment = VCPU_SEG_SS;
            }
        }
        ea &= 0xffff;
    } else {
        unsigned int mod = insn->modrm >> 6;
        unsigned int rm = insn->modrm & 7;
        ea = (uint64_t)disp;
        unsigned int base = rm;
        if (insn->has_sib) {
            base = insn->sib & 7;
            uint8_t index = ((insn->sib >> 3) & 7) | ((insn->rex & INSN_REX_X) != 0 ? 8 : 0)
                          | ((insn->rex_upper & INSN_REX_X) != 0 ? 16 : 0);
            if (index != 4) {
                ea += vcpu_gpr(v, index) << (insn->sib >> 6);
            }
        }
        if (mod == 0 && base == 5) {
            // No base: in 64-bit mode, without a SIB byte, RIP-relative.
            if (!insn->has_sib && insn->long_mode) {
                ea += vcpu_rip(v) + insn->length;
            }
        } else {
            unsigned int reg = base | ((insn->rex & INSN_REX_B) != 0 ? 8 : 0)
                             | ((insn->rex_upper & INSN_REX_B) != 0 ? 16 : 0);
            ea += vcpu_gpr(v, reg);
            if (base == 4 || base == 5) {
                segment = VCPU_SEG_SS;
            }
        }
        ea &= address_mask(insn->address_size);
    }
    if (insn->segment >= 0) {
        segment = insn->segment;
    }
    *segment_out = segment;
    *ea_out = ea;
    return true;
}

static bool mem_linear(struct vcpu *v, const struct insn *insn, int64_t disp, uint64_t *linear) {
    int segment;
    uint64_t ea;
    if (!mem_ea(v, insn, disp, &segment, &ea)) {
        return false;
    }
    *linear = segment_linear(v, insn->long_mode, segment, ea);
    return true;
}

bool insn_mem_linear(struct vcpu *v, const struct insn *insn, uint64_t *linear) {
    if (insn->vex) {
        return false;
    }
    return mem_linear(v, insn, insn->disp, linear);
}

bool insn_mem_linear_moved(
    struct vcpu *v,
    const struct insn *insn,
    int64_t delta,
    uint64_t *linear
) {
    int segment;
    uint64_t ea;
    if (insn->vex || !mem_ea(v, insn, insn->disp, &segment, &ea)) {
        return false;
    }
    ea = (ea + (uint64_t)delta) & address_mask(insn->address_size);
    *linear = segment_linear(v, insn->long_mode, segment, ea);
    return true;
}

// The first form of the instruction's opcode it matches, if any.
static const struct insn_mem_table_form *mem_form(const struct insn *insn) {
    unsigned int key = (unsigned int)insn->space << 8 | insn->opcode_byte;
    unsigned int low = 0;
    unsigned int high = insn_mem_table_form_count;
    while (low < high) {
        unsigned int mid = (low + high) / 2;
        const struct insn_mem_table_form *f = &insn_mem_table_forms[mid];
        if (((unsigned int)f->space << 8 | f->opcode) < key) {
            low = mid + 1;
        } else {
            high = mid;
        }
    }
    unsigned int reg = (insn->modrm >> 3) & 7;
    unsigned int mode = insn->long_mode ? INSN_MEM_TABLE_MODE_64 : INSN_MEM_TABLE_MODE_OTHER;
    for (size_t i = low; i < insn_mem_table_form_count; i++) {
        const struct insn_mem_table_form *f = &insn_mem_table_forms[i];
        if (((unsigned int)f->space << 8 | f->opcode) != key) {
            break;
        }
        if ((f->prefix == INSN_MEM_TABLE_PREFIX_ANY || f->prefix == insn->prefix)
         && (f->reg == INSN_MEM_TABLE_REG_ANY || f->reg == reg)
         && (f->w == INSN_MEM_TABLE_W_ANY || f->w == insn->w)
         && (f->l == INSN_MEM_TABLE_L_ANY || f->l == insn->vector_length)
         && (f->nd == INSN_MEM_TABLE_ND_ANY || f->nd == insn->nd)
         && (f->nf == INSN_MEM_TABLE_NF_ANY || f->nf == insn->nf) && (f->modes & mode) != 0) {
            return f;
        }
    }
    return NULL;
}

static size_t mem_size(const struct insn *insn, const struct insn_mem_table_form *f) {
    switch (f->size_kind) {
        case INSN_MEM_TABLE_SIZE_FIXED: {
            return f->size;
        }
        case INSN_MEM_TABLE_SIZE_VECTOR: {
            return f->size << insn->vector_length;
        }
        case INSN_MEM_TABLE_SIZE_OPERAND: {
            return insn->operand_size;
        }
        case INSN_MEM_TABLE_SIZE_STACK: {
            return insn->long_mode && insn->operand_size == 4 ? 8 : insn->operand_size;
        }
        case INSN_MEM_TABLE_SIZE_FAR: {
            return insn->operand_size + 2;
        }
        case INSN_MEM_TABLE_SIZE_PAIR: {
            return insn->operand_size * 2;
        }
        case INSN_MEM_TABLE_SIZE_FPU_ENV: {
            return insn->operand_size == 2 ? 14 : 28;
        }
        case INSN_MEM_TABLE_SIZE_FPU_STATE: {
            return insn->operand_size == 2 ? 94 : 108;
        }
        default: {
            return 0;
        }
    }
}

bool insn_mem_operand(struct vcpu *v, const struct insn *insn, struct insn_mem *mem) {
    if (!insn->has_modrm || insn->modrm >> 6 == 3) {
        return false;
    }
    const struct insn_mem_table_form *f = mem_form(insn);
    if (f == NULL || f->size_kind == INSN_MEM_TABLE_SIZE_UNKNOWN) {
        return false;
    }
    if (f->size_kind == INSN_MEM_TABLE_SIZE_NONE) {
        *mem = (struct insn_mem){ 0 };
        return true;
    }
    bool evex = (insn->space & 0xf0) == INSN_SPACE_EVEX;
    size_t size = mem_size(insn, f);
    unsigned int scale = f->disp8_scale != 0 ? f->disp8_scale : size;
    if (evex && insn->evex_b && f->broadcast != 0) {
        size = f->broadcast;
        scale = size;
    }
    // EVEX scales an 8-bit displacement: SDM 325462-093 Vol. 2A, 2.7.5.
    int64_t disp = insn->disp;
    if (evex && insn->disp_size == 1) {
        disp *= scale;
    }
    if (!mem_linear(v, insn, disp, &mem->linear)) {
        return false;
    }
    mem->size = size;
    mem->access = f->access;
    // EVEX masks with any opmask register but k0.
    if (evex && insn->evex_aaa == 0) {
        mem->access &= ~INSN_MEM_MASKED;
    }
    return true;
}

// The stack's slots an instruction of the one-byte or 0F map pushes or pops,
// the bytes of each, and where it goes once done.
static bool stack_kind(
    struct vcpu *v,
    const struct insn *insn,
    size_t *bytes,
    bool *push,
    bool *from_bp,
    struct insn_stack *stack
) {
    bool map0 = insn->space == INSN_SPACE_LEGACY;
    uint8_t op = insn->opcode_byte;
    unsigned int reg = (insn->modrm >> 3) & 7;
    bool real = (vcpu_cr0(v) & CR0_PE) == 0;
    // Stack operations in 64-bit mode are 64-bit but with 66h (SDM 325462-093
    // Vol. 1, 6.2.5). Its near branches stay 64-bit there on Intel's
    // processors (7.3.8.3) but not on AMD's (APM 40332 rev. 4.11, Vol. 3,
    // 2.5.6), so those are left out.
    size_t size = insn->operand_size;
    if (insn->long_mode) {
        size = size == 2 ? 2 : 8;
    }
    bool near_branch = !insn->long_mode || insn->operand_size != 2;
    uint64_t ip_mask = insn->long_mode ? UINT64_MAX : size == 2 ? 0xffff : 0xffffffff;
    uint64_t next = vcpu_rip(v) + insn->length;
    int64_t rel = insn->imm_size == 2 ? (int16_t)insn->imm : (int32_t)insn->imm;
    *bytes = size;
    *push = false;
    *from_bp = false;
    stack->end = INSN_END_NEXT;
    stack->slot = size;
    stack->loads_flags = false;
    stack->stores_flags = false;
    // A segment register's push fills its slot but with 32-bit operands, where
    // processors either zero-extend the selector or leave the slot's upper
    // bytes as they were (SDM 325462-093 Vol. 2B, PUSH).
    bool sreg_push = size != 4;
    if (!map0) {
        // POP FS and POP GS, and PUSH FS and PUSH GS.
        *push = op == 0xa0 || op == 0xa8;
        return insn->space == (INSN_SPACE_LEGACY | 1)
            && (op == 0xa1 || op == 0xa9 || (*push && sreg_push));
    }
    if (op >= 0x50 && op <= 0x57) {
        *push = true;
    } else if (!insn->long_mode && (op == 0x06 || op == 0x0e || op == 0x16 || op == 0x1e)) {
        *push = true;
        if (!sreg_push) {
            return false;
        }
    } else if ((op >= 0x58 && op <= 0x5f) || (!insn->long_mode && (op == 0x07 || op == 0x1f))) {
        // POP of a register, of ES, or of DS.
    } else if (op == 0x68 || op == 0x6a || op == 0x9c || (op == 0xff && reg == 6)) {
        *push = true;
        stack->stores_flags = op == 0x9c;
    } else if (op == 0x9d) {
        stack->loads_flags = true;
    } else if (op == 0x8f && reg == 0) {
        // Where RSP is the destination's base, its address follows the pop.
        unsigned int base = insn->has_sib ? insn->sib & 7 : insn->modrm & 7;
        if (insn->address_size != 2 && base == 4 && insn->modrm >> 6 != 3) {
            return false;
        }
    } else if (!insn->long_mode && (op == 0x60 || op == 0x61)) {
        *bytes = 8 * size;
        *push = op == 0x60;
    } else if (op == 0xc9) {
        *from_bp = true;
    } else if (op == 0xc8) {
        // ENTER of nesting level 0 only pushes the frame pointer.
        *push = true;
        if (((insn->imm >> 16) & 0x1f) != 0) {
            return false;
        }
    } else if (op == 0xe8 && near_branch) {
        *push = true;
        stack->end = INSN_END_TARGET;
        stack->target = (next + (uint64_t)rel) & ip_mask;
    } else if (op == 0xff && reg == 2 && insn->modrm >> 6 == 3 && near_branch) {
        unsigned int rm = (insn->modrm & 7) | ((insn->rex & INSN_REX_B) != 0 ? 8 : 0)
                        | ((insn->rex_upper & INSN_REX_B) != 0 ? 16 : 0);
        *push = true;
        stack->end = INSN_END_TARGET;
        stack->target = vcpu_gpr(v, rm) & ip_mask;
    } else if ((op == 0xc3 || op == 0xc2) && near_branch) {
        stack->end = INSN_END_POP_NEAR;
    } else if (real && op == 0x9a) {
        *bytes = 2 * size;
        *push = true;
        stack->end = INSN_END_FAR;
        stack->target = insn->imm & (size == 2 ? 0xffff : 0xffffffff);
        stack->selector = (uint16_t)(insn->imm >> (size * 8));
    } else if (real && (op == 0xcb || op == 0xca)) {
        *bytes = 2 * size;
        stack->end = INSN_END_POP_FAR;
    } else if (real && op == 0xcf) {
        *bytes = 3 * size;
        stack->end = INSN_END_POP_FAR;
        stack->loads_flags = true;
    } else {
        return false;
    }
    return true;
}

bool insn_stack_operand(struct vcpu *v, const struct insn *insn, struct insn_stack *stack) {
    size_t bytes;
    bool push;
    bool from_bp;
    if (!stack_kind(v, insn, &bytes, &push, &from_bp, stack)) {
        return false;
    }
    // In 64-bit mode SS has no base (SDM 325462-093 Vol. 1, 6.2.5); elsewhere
    // its B flag makes the stack pointer ESP rather than SP.
    uint64_t mask = UINT64_MAX;
    uint64_t base = 0;
    if (!insn->long_mode) {
        mask = (vcpu_seg_attrib(v, VCPU_SEG_SS) & VCPU_SEG_DB) != 0 ? 0xffffffff : 0xffff;
        base = vcpu_seg_base(v, VCPU_SEG_SS);
    }
    uint64_t sp = (from_bp ? vcpu_gpr(v, 5) : vcpu_rsp(v)) & mask;
    uint64_t start = push ? (sp - bytes) & mask : sp;
    // The slots wrap round the stack's end.
    if (mask != UINT64_MAX && start + bytes - 1 > mask) {
        return false;
    }
    stack->mem.linear = insn->long_mode ? start : (base + start) & 0xffffffff;
    stack->mem.size = bytes;
    stack->mem.access = push ? INSN_MEM_WRITE : INSN_MEM_READ;
    return true;
}

#define SEG_PRESENT ((uint32_t)1 << 7)
#define SEG_CODE ((uint32_t)1 << 3)
#define SEG_EXPAND_DOWN ((uint32_t)1 << 2)
// Writable in a data segment, readable in a code segment.
#define SEG_RW ((uint32_t)1 << 1)

static bool canonical(uintptr_t linear, unsigned int bits) {
    uint64_t upper = linear >> (bits - 1);
    return upper == 0 || upper == (UINT64_MAX >> (bits - 1));
}

// Whether an explicit data access to la is a LASS violation (SDM 325462-093
// Vol. 3A, 4.3.2): in IA-32e mode, a user-mode one with bit 63 set, a
// supervisor-mode one with it clear where SMAP is on and RFLAGS.AC off; bit 63
// of 32-bit addresses, as in compatibility mode, is clear.
static bool lass_violation(struct vcpu *v, bool long_mode, uint64_t la) {
    if ((vcpu_efer(v) & EFER_LMA) == 0 || (vcpu_cr4(v) & CR4_LASS) == 0) {
        return false;
    }
    bool high = long_mode && (la >> 63) != 0;
    if (vcpu_cpl(v) == 3) {
        return high;
    }
    return !high && (vcpu_cr4(v) & CR4_SMAP) != 0 && (vcpu_rflags(v) & RFLAGS_AC) == 0;
}

// The checks an access of size bytes at offset ea of a segment passes (SDM
// 325462-093 Vol. 3A, 3.3.7.1, 4.3.2, 5.3, and 5.4), as Linux 7.3's KVM
// emulator makes them (emulate.c, __linearize()): in 64-bit mode the address's
// canonical form; elsewhere, compatibility mode included, the segment's
// usability, type, and limit, an expand-down one's from above it; and LASS.
static bool linear_allowed(
    struct vcpu *v,
    bool long_mode,
    int reg,
    uintptr_t ea,
    size_t size,
    bool write,
    uint64_t *linear
) {
    if (long_mode) {
        uintptr_t la = segment_linear(v, long_mode, reg, ea);
        unsigned int bits = (vcpu_cr4(v) & CR4_LA57) != 0 ? 57 : 48;
        if (canonical(la, bits) && canonical(la + size - 1, bits)
         && !lass_violation(v, long_mode, la)) {
            *linear = la;
            return true;
        }
    } else {
        uint32_t attrib = vcpu_seg_attrib(v, (size_t)reg);
        bool code = (attrib & SEG_CODE) != 0;
        bool real = (vcpu_cr0(v) & CR0_PE) == 0;
        uint64_t limit = vcpu_seg_limit(v, (size_t)reg);
        bool allowed = (attrib & VCPU_SEG_UNUSABLE) == 0 && (attrib & SEG_PRESENT) != 0;
        if (write && ((code && !real) || (attrib & SEG_RW) == 0)) {
            allowed = false;
        }
        if (!write && code && (attrib & SEG_RW) == 0) {
            allowed = false;
        }
        if (!code && (attrib & SEG_EXPAND_DOWN) != 0) {
            allowed &= ea > limit;
            limit = (attrib & VCPU_SEG_DB) != 0 ? 0xffffffff : 0xffff;
        }
        uint64_t la = (vcpu_seg_base(v, (size_t)reg) + ea) & 0xffffffff;
        if (allowed && ea <= limit && (limit == 0xffffffff || size <= limit + 1 - ea)
         && !lass_violation(v, long_mode, la)) {
            *linear = la;
            return true;
        }
    }
    return false;
}

// Where they fail, #SS(0) for SS or #GP(0) is raised.
static bool linearise(
    struct vcpu *v,
    bool long_mode,
    int reg,
    uintptr_t ea,
    size_t size,
    bool write,
    uint64_t *linear
) {
    if (linear_allowed(v, long_mode, reg, ea, size, write, linear)) {
        return true;
    }
    vcpu_inject_exception(v, reg == VCPU_SEG_SS ? 12 : 13, true, 0);
    return false;
}

bool insn_string_linear(
    struct vcpu *v,
    const struct insn *insn,
    bool destination,
    size_t size,
    bool write,
    uint64_t *linear
) {
    uint64_t mask = address_mask(insn->address_size);
    if (destination) {
        return linearise(v, insn->long_mode, VCPU_SEG_ES, v->gprs.rdi & mask, size, write, linear);
    }
    int segment = insn->segment >= 0 ? insn->segment : VCPU_SEG_DS;
    return linearise(v, insn->long_mode, segment, v->gprs.rsi & mask, size, write, linear);
}

bool insn_string_run(
    struct vcpu *v,
    const struct insn *insn,
    bool destination,
    size_t size,
    int64_t step,
    size_t count,
    bool write,
    uint64_t *linear
) {
    uint64_t mask = address_mask(insn->address_size);
    uint64_t first = (destination ? v->gprs.rdi : v->gprs.rsi) & mask;
    uint64_t span = (uint64_t)(count - 1) * size;
    uint64_t last;
    if (step > 0) {
        if (span > mask - first) {
            return false;
        }
        last = first + span;
    } else {
        if (span > first) {
            return false;
        }
        last = first - span;
    }
    int segment = destination ? VCPU_SEG_ES : insn->segment >= 0 ? insn->segment : VCPU_SEG_DS;
    // The checks hold for every operand between two that pass them.
    uint64_t last_linear;
    return linear_allowed(v, insn->long_mode, segment, first, size, write, linear)
        && linear_allowed(v, insn->long_mode, segment, last, size, write, &last_linear);
}

bool insn_segment_linear(
    struct vcpu *v,
    int reg,
    uintptr_t ea,
    size_t size,
    bool write,
    uint64_t *linear
) {
    uint32_t cs = vcpu_seg_attrib(v, VCPU_SEG_CS);
    bool long_mode = (vcpu_efer(v) & EFER_LMA) != 0 && (cs & VCPU_SEG_L) != 0;
    return linearise(v, long_mode, reg, ea, size, write, linear);
}
