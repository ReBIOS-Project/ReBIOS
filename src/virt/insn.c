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

#include <stdint.h>
#include <stdbool.h>
#include <sys/x86.h>
#include <virt/guest_mem.h>
#include <virt/insn.h>
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

unsigned insn_length_intercepted(struct vcpu *v) {
    struct segment cs;
    vcpu_get_seg(v, SEG_CS, &cs);
    bool long_mode = (vcpu_efer(v) & EFER_LMA) && (cs.attrib & SEG_L);
    uint64_t linear = vcpu_rip(v);
    if (!long_mode) {
        linear = (cs.base + linear) & 0xffffffff;
    }

    uint8_t bytes[15];
    if (!guest_linear_read(v, linear, bytes, sizeof(bytes), true)) {
        // The instruction may end before a page that is not mapped.
        unsigned avail = 4096 - (linear & 4095);
        if (avail >= sizeof(bytes) || !guest_linear_read(v, linear, bytes, avail, true)) {
            return 0;
        }
    }

    unsigned i = 0;
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
    S, 0, S, S, 0, 0, M, M, 0, 0, 0, 0, 0, 0, M, M,
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
    M, M, M, M, M, M, M, M, M, M, M, M, M, M, M, M,
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

static unsigned imm_size(unsigned kind, const struct insn *insn, bool long_mode) {
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

static int64_t read_signed(const uint8_t *bytes, unsigned size) {
    uint64_t value = 0;
    for (unsigned n = 0; n < size; n++) {
        value |= (uint64_t)bytes[n] << (n * 8);
    }
    unsigned shift = 64 - size * 8;
    return (int64_t)(value << shift) >> shift;
}

// Decodes a ModRM byte at bytes[i] and what it brings with it: returns their
// size, the ModRM byte included.
static unsigned decode_modrm(const uint8_t *bytes, unsigned i, struct insn *insn) {
    uint8_t modrm = bytes[i];
    unsigned mod = modrm >> 6;
    unsigned rm = modrm & 7;
    insn->has_modrm = true;
    insn->modrm = modrm;
    if (mod == 3) {
        return 1;
    }
    unsigned size = 1;
    unsigned disp_size;
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
    struct segment cs;
    vcpu_get_seg(v, SEG_CS, &cs);
    bool long_mode = (vcpu_efer(v) & EFER_LMA) && (cs.attrib & SEG_L);
    bool big = !(vcpu_rflags(v) & RFLAGS_VM) && (vcpu_cr0(v) & CR0_PE) && (cs.attrib & SEG_DB);
    uint64_t linear = vcpu_rip(v);
    if (!long_mode) {
        linear = (cs.base + linear) & 0xffffffff;
    }

    // Zeroes past what can be read make the decoding stop short, as the
    // instruction then cannot be complete.
    uint8_t bytes[INSN_MAX_LENGTH + 4] = {0};
    if (!guest_linear_read(v, linear, bytes, INSN_MAX_LENGTH, true)) {
        unsigned avail = 4096 - (linear & 4095);
        if (avail >= INSN_MAX_LENGTH || !guest_linear_read(v, linear, bytes, avail, true)) {
            return false;
        }
    }

    unsigned operand = long_mode || big ? 4 : 2;
    unsigned address = long_mode ? 8 : big ? 4 : 2;
    bool operand_prefix = false;
    bool address_prefix = false;
    bool rex_w = false;
    uint8_t rex = 0;
    *insn = (struct insn){.long_mode = long_mode, .segment = -1};

    unsigned i = 0;
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
            int segment = b == 0x26 ? SEG_ES : b == 0x2e ? SEG_CS : b == 0x36 ? SEG_SS
                          : b == 0x3e ? SEG_DS : b == 0x64 ? SEG_FS : b == 0x65 ? SEG_GS : -1;
            if (segment >= 0 && (!long_mode || segment == SEG_FS || segment == SEG_GS)) {
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

    unsigned map = MAP_ONE;
    uint8_t op = bytes[i];
    // VEX, EVEX, and XOP take the place of LES, LDS, BOUND, and POP Ev
    // where a ModRM byte could not follow.
    bool vex = long_mode || (bytes[i + 1] & 0xc0) == 0xc0;
    bool vex_like = false;
    if (op == 0xc5 && vex) {
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
        vex_rex(insn, bytes[i + 1], bytes[i + 2] >> 7, long_mode);
        insn->vector_length = (bytes[i + 2] >> 2) & 1;
        insn->prefix = bytes[i + 2] & 3;
        map = bytes[i + 1] & 0x1f;
        insn->space = INSN_SPACE_VEX | map;
        i += 3;
        vex_like = true;
    } else if (op == 0x62 && vex) {
        // R X B R' 0 mmm, W vvvv 1 pp, then z L'L b V' aaa.
        vex_rex(insn, bytes[i + 1], bytes[i + 2] >> 7, long_mode);
        insn->prefix = bytes[i + 2] & 3;
        insn->vector_length = (bytes[i + 3] >> 5) & 3;
        insn->evex_b = (bytes[i + 3] >> 4) & 1;
        insn->evex_aaa = bytes[i + 3] & 7;
        map = bytes[i + 1] & 0x07;
        insn->space = INSN_SPACE_EVEX | map;
        i += 4;
        vex_like = true;
    } else if (op == 0x8f && (bytes[i + 1] & 0x1f) >= 8) {
        // XOP, laid out as VEX's three bytes: map 8 takes an 8-bit
        // immediate, map 10 a 32-bit one.
        unsigned xop_map = bytes[i + 1] & 0x1f;
        vex_rex(insn, bytes[i + 1], bytes[i + 2] >> 7, long_mode);
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
    } else {
        // 0F 38, and the EVEX maps 5 and 6.
        entry = OP_MODRM;
        insn->opcode = INSN_OPCODE_OTHER;
    }
    if (entry & OP_SPECIAL) {
        return false;
    }

    insn->vex = vex_like;
    unsigned imm = entry & OP_IMM_MASK;
    if (entry & OP_MODRM) {
        // TEST in group 3 is the only member with an immediate.
        if (map == MAP_ONE && (op == 0xf6 || op == 0xf7) && ((bytes[i] >> 3) & 7) < 2) {
            imm = op == 0xf6 ? IMM_B : IMM_Z;
        }
        i += decode_modrm(bytes, i, insn);
    }
    insn->imm_size = imm_size(imm, insn, long_mode);
    for (unsigned n = 0; n < insn->imm_size && n < sizeof(insn->imm); n++) {
        insn->imm |= (uint64_t)bytes[i + n] << (n * 8);
    }
    i += insn->imm_size;
    insn->length = i;
    return i <= INSN_MAX_LENGTH;
}

#define REX_B 0x01
#define REX_X 0x02

static uint64_t address_mask(unsigned size) {
    return size == 8 ? UINT64_MAX : (1ull << (size * 8)) - 1;
}

// Outside 64-bit mode the linear address space wraps at 4 GiB, and in it only
// FS and GS have bases.
static uint64_t segment_linear(struct vcpu *v, const struct insn *insn, int reg, uint64_t ea) {
    struct segment segment;
    if (insn->long_mode) {
        if (reg != SEG_FS && reg != SEG_GS) {
            return ea;
        }
        vcpu_get_seg(v, reg, &segment);
        return segment.base + ea;
    }
    vcpu_get_seg(v, reg, &segment);
    return (segment.base + ea) & 0xffffffff;
}

static bool mem_linear(struct vcpu *v, const struct insn *insn, int64_t disp, uint64_t *linear) {
    int segment = SEG_DS;
    uint64_t ea;
    if (insn->opcode >= 0xa0 && insn->opcode <= 0xa3) {
        ea = insn->imm;
    } else if (!insn->has_modrm || insn->modrm >> 6 == 3) {
        return false;
    } else if (insn->address_size == 2) {
        unsigned mod = insn->modrm >> 6;
        unsigned rm = insn->modrm & 7;
        uint64_t bx = insn_gpr(v, 3);
        uint64_t bp = insn_gpr(v, 5);
        uint64_t si = insn_gpr(v, 6);
        uint64_t di = insn_gpr(v, 7);
        static const uint8_t base_is_bp = 1u << 2 | 1u << 3 | 1u << 6;
        uint64_t sums[8] = {bx + si, bx + di, bp + si, bp + di, si, di, bp, bx};
        ea = (uint64_t)disp;
        if (rm != 6 || mod != 0) {
            ea += sums[rm];
            if ((base_is_bp >> rm) & 1) {
                segment = SEG_SS;
            }
        }
        ea &= 0xffff;
    } else {
        unsigned mod = insn->modrm >> 6;
        unsigned rm = insn->modrm & 7;
        ea = (uint64_t)disp;
        unsigned base = rm;
        if (insn->has_sib) {
            base = insn->sib & 7;
            unsigned index = ((insn->sib >> 3) & 7) | ((insn->rex & REX_X) ? 8 : 0);
            if (index != 4) {
                ea += insn_gpr(v, index) << (insn->sib >> 6);
            }
        }
        if (mod == 0 && base == 5) {
            // No base: in 64-bit mode, without a SIB byte, RIP-relative.
            if (!insn->has_sib && insn->long_mode) {
                ea += vcpu_rip(v) + insn->length;
            }
        } else {
            ea += insn_gpr(v, base | ((insn->rex & REX_B) ? 8 : 0));
            if (base == 4 || base == 5) {
                segment = SEG_SS;
            }
        }
        ea &= address_mask(insn->address_size);
    }
    if (insn->segment >= 0) {
        segment = insn->segment;
    }
    *linear = segment_linear(v, insn, segment, ea);
    return true;
}

bool insn_mem_linear(struct vcpu *v, const struct insn *insn, uint64_t *linear) {
    if (insn->vex) {
        return false;
    }
    return mem_linear(v, insn, insn->disp, linear);
}

// The first form of the instruction's opcode it matches, if any.
static const struct insn_mem_form *mem_form(const struct insn *insn) {
    unsigned key = (unsigned)insn->space << 8 | insn->opcode_byte;
    unsigned low = 0;
    unsigned high = insn_mem_form_count;
    while (low < high) {
        unsigned mid = (low + high) / 2;
        if (((unsigned)insn_mem_forms[mid].space << 8 | insn_mem_forms[mid].opcode) < key) {
            low = mid + 1;
        } else {
            high = mid;
        }
    }
    unsigned reg = (insn->modrm >> 3) & 7;
    unsigned mode = insn->long_mode ? INSN_MODE_64 : INSN_MODE_OTHER;
    for (unsigned i = low; i < insn_mem_form_count; i++) {
        const struct insn_mem_form *f = &insn_mem_forms[i];
        if (((unsigned)f->space << 8 | f->opcode) != key) {
            break;
        }
        if ((f->prefix == INSN_PREFIX_ANY || f->prefix == insn->prefix)
            && (f->reg == INSN_REG_ANY || f->reg == reg) && (f->w == INSN_W_ANY || f->w == insn->w)
            && (f->l == INSN_L_ANY || f->l == insn->vector_length) && (f->modes & mode) != 0) {
            return f;
        }
    }
    return NULL;
}

static unsigned mem_size(const struct insn *insn, const struct insn_mem_form *f) {
    switch (f->size_kind) {
        case INSN_SIZE_FIXED: {
            return f->size;
        }
        case INSN_SIZE_VECTOR: {
            return f->size << insn->vector_length;
        }
        case INSN_SIZE_OPERAND: {
            return insn->operand_size;
        }
        case INSN_SIZE_STACK: {
            return insn->long_mode && insn->operand_size == 4 ? 8 : insn->operand_size;
        }
        case INSN_SIZE_FAR: {
            return insn->operand_size + 2;
        }
        case INSN_SIZE_PAIR: {
            return insn->operand_size * 2;
        }
        case INSN_SIZE_FPU_ENV: {
            return insn->operand_size == 2 ? 14 : 28;
        }
        case INSN_SIZE_FPU_STATE: {
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
    const struct insn_mem_form *f = mem_form(insn);
    if (f == NULL || f->size_kind == INSN_SIZE_UNKNOWN) {
        return false;
    }
    if (f->size_kind == INSN_SIZE_NONE) {
        *mem = (struct insn_mem){0};
        return true;
    }
    bool evex = (insn->space & 0xf0) == INSN_SPACE_EVEX;
    unsigned size = mem_size(insn, f);
    unsigned scale = f->disp8_scale != 0 ? f->disp8_scale : size;
    if (evex && insn->evex_b && f->broadcast != 0) {
        size = f->broadcast;
        scale = size;
    }
    // EVEX scales an 8-bit displacement: SDM 325462-092 Vol. 2A, 2.7.5.
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

uint64_t insn_string_linear(struct vcpu *v, const struct insn *insn, bool destination) {
    uint64_t mask = address_mask(insn->address_size);
    if (destination) {
        return segment_linear(v, insn, SEG_ES, v->gprs.rdi & mask);
    }
    int segment = insn->segment >= 0 ? insn->segment : SEG_DS;
    return segment_linear(v, insn, segment, v->gprs.rsi & mask);
}

uint64_t insn_gpr(struct vcpu *v, unsigned reg) {
    if (reg == 4) {
        return vcpu_rsp(v);
    }
    return ((uint64_t *)&v->gprs)[reg];
}

void insn_set_gpr(struct vcpu *v, unsigned reg, uint64_t value) {
    if (reg == 4) {
        vcpu_set_rsp(v, value);
        return;
    }
    ((uint64_t *)&v->gprs)[reg] = value;
}
