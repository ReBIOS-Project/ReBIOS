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
#include <stdbool.h>
#include <sys/x86.h>
#include <virt/guest_mem.h>
#include <virt/insn.h>
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

// The bytes a ModRM byte at bytes[i] brings with it, itself included.
static unsigned modrm_size(const uint8_t *bytes, unsigned i, unsigned address_size) {
    uint8_t modrm = bytes[i];
    unsigned mod = modrm >> 6;
    unsigned rm = modrm & 7;
    if (mod == 3) {
        return 1;
    }
    if (address_size == 2) {
        if (mod == 1) {
            return 2;
        }
        return mod == 2 || rm == 6 ? 3 : 1;
    }
    unsigned size = 1;
    if (rm == 4) {
        size++;
        if (mod == 0 && (bytes[i + 1] & 7) == 5) {
            size += 4;
        }
    }
    if (mod == 0 && rm == 5) {
        size += 4;
    } else if (mod == 1) {
        size += 1;
    } else if (mod == 2) {
        size += 4;
    }
    return size;
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
    insn->rep = false;
    insn->modrm = 0;
    insn->has_modrm = false;
    insn->imm = 0;
    insn->imm_size = 0;

    unsigned i = 0;
    for (;;) {
        uint8_t b = bytes[i];
        if (is_legacy_prefix(b)) {
            operand_prefix |= b == 0x66;
            address_prefix |= b == 0x67;
            insn->rep |= b == 0xf2 || b == 0xf3;
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

    unsigned map = MAP_ONE;
    uint8_t op = bytes[i];
    // VEX, EVEX, and XOP take the place of LES, LDS, BOUND, and POP Ev
    // where a ModRM byte could not follow.
    bool vex = long_mode || (bytes[i + 1] & 0xc0) == 0xc0;
    bool vex_like = false;
    if ((op == 0xc4 || op == 0xc5) && vex) {
        map = op == 0xc5 ? MAP_0F : bytes[i + 1] & 0x1f;
        i += op == 0xc5 ? 2 : 3;
        vex_like = true;
    } else if (op == 0x62 && vex) {
        map = bytes[i + 1] & 0x07;
        i += 4;
        vex_like = true;
    } else if (op == 0x8f && (bytes[i + 1] & 0x1f) >= 8) {
        // XOP: map 8 takes an 8-bit immediate, map 10 a 32-bit one.
        unsigned xop_map = bytes[i + 1] & 0x1f;
        i += 3;
        i++;
        i += modrm_size(bytes, i, address);
        i += xop_map == 8 ? 1 : xop_map == 10 ? 4 : 0;
        insn->opcode = INSN_OPCODE_OTHER;
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
    }
    if (i >= INSN_MAX_LENGTH) {
        return false;
    }
    op = bytes[i++];

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

    unsigned imm = entry & OP_IMM_MASK;
    if (entry & OP_MODRM) {
        insn->has_modrm = true;
        insn->modrm = bytes[i];
        // TEST in group 3 is the only member with an immediate.
        if (map == MAP_ONE && (op == 0xf6 || op == 0xf7) && ((bytes[i] >> 3) & 7) < 2) {
            imm = op == 0xf6 ? IMM_B : IMM_Z;
        }
        i += modrm_size(bytes, i, address);
    }
    insn->imm_size = imm_size(imm, insn, long_mode);
    for (unsigned n = 0; n < insn->imm_size && n < sizeof(insn->imm); n++) {
        insn->imm |= (uint64_t)bytes[i + n] << (n * 8);
    }
    i += insn->imm_size;
    insn->length = i;
    return i <= INSN_MAX_LENGTH;
}

#define REX_R 0x04

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

static uint64_t size_mask(unsigned size) {
    return size >= 8 ? UINT64_MAX : (1ull << (size * 8)) - 1;
}

static uint64_t sign_extend(uint64_t value, unsigned size) {
    if (size == 0 || size >= 8) {
        return value;
    }
    unsigned shift = 64 - size * 8;
    return (uint64_t)((int64_t)(value << shift) >> shift);
}

bool insn_store(struct vcpu *v, const struct insn *insn, struct insn_store *store) {
    unsigned op = insn->opcode;
    unsigned group = (insn->modrm >> 3) & 7;
    switch (op) {
        case 0x88:
        case 0x89:
        case 0xa2:
        case 0xa3: {
            store->op = INSN_STORE_MOV;
            break;
        }
        case 0xc6:
        case 0xc7: {
            if (group != 0) {
                return false;
            }
            store->op = INSN_STORE_MOV;
            break;
        }
        case 0x86:
        case 0x87: {
            store->op = INSN_STORE_XCHG;
            break;
        }
        case 0x08:
        case 0x09: {
            store->op = INSN_STORE_OR;
            break;
        }
        case 0x20:
        case 0x21: {
            store->op = INSN_STORE_AND;
            break;
        }
        case 0x80:
        case 0x81:
        case 0x83: {
            if (group == 1) {
                store->op = INSN_STORE_OR;
            } else if (group == 4) {
                store->op = INSN_STORE_AND;
            } else {
                return false;
            }
            break;
        }
        default: {
            return false;
        }
    }

    // The byte forms are the even opcodes.
    bool byte = !(op & 1);
    store->size = byte ? 1 : insn->operand_size;
    store->reg = 0;
    store->high_byte = false;
    if (op == 0xc6 || op == 0xc7 || op == 0x80 || op == 0x81 || op == 0x83) {
        store->value = sign_extend(insn->imm, insn->imm_size);
    } else if (op == 0xa2 || op == 0xa3) {
        store->value = v->gprs.rax;
    } else {
        store->reg = group | ((insn->rex & REX_R) ? 8 : 0);
        // Without a REX prefix, byte registers 4 to 7 are AH, CH, DH, and BH.
        if (byte && insn->rex == 0 && store->reg >= 4) {
            store->high_byte = true;
            store->value = insn_gpr(v, store->reg - 4) >> 8;
        } else {
            store->value = insn_gpr(v, store->reg);
        }
    }
    store->value &= size_mask(store->size);
    return true;
}

static void set_reg(struct vcpu *v, unsigned reg, bool high_byte, unsigned size, uint64_t value) {
    if (high_byte) {
        reg -= 4;
        insn_set_gpr(v, reg, (insn_gpr(v, reg) & ~0xff00ull) | (value & 0xff) << 8);
    } else if (size == 4) {
        // 32-bit results are zero-extended.
        insn_set_gpr(v, reg, value & 0xffffffffu);
    } else {
        uint64_t mask = size_mask(size);
        insn_set_gpr(v, reg, (insn_gpr(v, reg) & ~mask) | (value & mask));
    }
}

static void logic_flags(struct vcpu *v, uint64_t result, unsigned size) {
    uint64_t flags = vcpu_rflags(v)
                     & ~(uint64_t)(RFLAGS_CF | RFLAGS_OF | RFLAGS_SF | RFLAGS_ZF | RFLAGS_PF);
    if (result == 0) {
        flags |= RFLAGS_ZF;
    }
    if ((result >> (size * 8 - 1)) & 1) {
        flags |= RFLAGS_SF;
    }
    if (!__builtin_parity((unsigned)(result & 0xff))) {
        flags |= RFLAGS_PF;
    }
    vcpu_set_rflags(v, flags);
}

uint64_t insn_store_apply(struct vcpu *v, const struct insn_store *store, uint64_t old) {
    uint64_t mask = size_mask(store->size);
    switch (store->op) {
        case INSN_STORE_XCHG: {
            set_reg(v, store->reg, store->high_byte, store->size, old);
            return store->value;
        }
        case INSN_STORE_OR: {
            uint64_t result = (old | store->value) & mask;
            logic_flags(v, result, store->size);
            return result;
        }
        case INSN_STORE_AND: {
            uint64_t result = old & store->value & mask;
            logic_flags(v, result, store->size);
            return result;
        }
        default: {
            return store->value;
        }
    }
}

bool insn_load(const struct insn *insn, struct insn_load *load) {
    unsigned op = insn->opcode;
    unsigned group = (insn->modrm >> 3) & 7;
    *load = (struct insn_load){.reg = group | ((insn->rex & REX_R) ? 8 : 0)};
    // The byte forms are the even opcodes.
    bool byte = !(op & 1);
    switch (op) {
        case 0x8a:
        case 0x8b: {
            load->op = INSN_LOAD_MOV;
            break;
        }
        case 0xa0:
        case 0xa1: {
            load->op = INSN_LOAD_MOV;
            load->reg = 0;
            break;
        }
        case 0x84:
        case 0x85: {
            load->op = INSN_LOAD_TEST;
            break;
        }
        case 0x38:
        case 0x39:
        case 0x3a:
        case 0x3b: {
            load->op = INSN_LOAD_CMP;
            load->memory_second = op >= 0x3a;
            break;
        }
        case 0xf6:
        case 0xf7: {
            // Group 3's /1 is an alias of TEST.
            if (group > 1) {
                return false;
            }
            load->op = INSN_LOAD_TEST;
            load->immediate = true;
            break;
        }
        case 0x80:
        case 0x81:
        case 0x83: {
            if (group != 7) {
                return false;
            }
            load->op = INSN_LOAD_CMP;
            load->immediate = true;
            break;
        }
        case INSN_OPCODE_0F | 0xb6:
        case INSN_OPCODE_0F | 0xb7:
        case INSN_OPCODE_0F | 0xbe:
        case INSN_OPCODE_0F | 0xbf: {
            load->op = (op & 8) ? INSN_LOAD_MOVSX : INSN_LOAD_MOVZX;
            load->size = byte ? 1 : 2;
            load->reg_size = insn->operand_size;
            break;
        }
        case INSN_OPCODE_0F | 0xa3: {
            load->op = INSN_LOAD_BT;
            load->size = insn->operand_size;
            load->reg_size = insn->operand_size;
            break;
        }
        case INSN_OPCODE_0F | 0xba: {
            if (group != 4) {
                return false;
            }
            load->op = INSN_LOAD_BT;
            load->size = insn->operand_size;
            load->immediate = true;
            break;
        }
        default: {
            return false;
        }
    }
    if (load->size == 0) {
        load->size = byte ? 1 : insn->operand_size;
        load->reg_size = load->size;
    }
    if (load->immediate) {
        load->imm = sign_extend(insn->imm, insn->imm_size);
    } else {
        // Without a REX prefix, byte registers 4 to 7 are AH, CH, DH, and BH.
        load->high_byte = load->reg_size == 1 && insn->rex == 0 && load->reg >= 4;
    }
    return true;
}

static void sub_flags(struct vcpu *v, uint64_t a, uint64_t b, unsigned size) {
    uint64_t mask = size_mask(size);
    a &= mask;
    b &= mask;
    uint64_t result = (a - b) & mask;
    unsigned top = size * 8 - 1;
    uint64_t flags = vcpu_rflags(v)
                     & ~(uint64_t)(RFLAGS_CF | RFLAGS_OF | RFLAGS_SF | RFLAGS_ZF | RFLAGS_AF
                                   | RFLAGS_PF);
    if (a < b) {
        flags |= RFLAGS_CF;
    }
    if ((((a ^ b) & (a ^ result)) >> top) & 1) {
        flags |= RFLAGS_OF;
    }
    if ((result >> top) & 1) {
        flags |= RFLAGS_SF;
    }
    if (result == 0) {
        flags |= RFLAGS_ZF;
    }
    if ((a ^ b ^ result) & 0x10) {
        flags |= RFLAGS_AF;
    }
    if (!__builtin_parity((unsigned)(result & 0xff))) {
        flags |= RFLAGS_PF;
    }
    vcpu_set_rflags(v, flags);
}

void insn_load_complete(struct vcpu *v, const struct insn_load *load, uint64_t value) {
    value &= size_mask(load->size);
    uint64_t other = load->imm;
    if (!load->immediate) {
        other = load->high_byte ? insn_gpr(v, load->reg - 4) >> 8 : insn_gpr(v, load->reg);
        other &= size_mask(load->reg_size);
    }
    switch (load->op) {
        case INSN_LOAD_MOVSX: {
            set_reg(v, load->reg, load->high_byte, load->reg_size,
                    sign_extend(value, load->size));
            break;
        }
        case INSN_LOAD_TEST: {
            logic_flags(v, value & other & size_mask(load->size), load->size);
            break;
        }
        case INSN_LOAD_CMP: {
            if (load->memory_second) {
                sub_flags(v, other, value, load->size);
            } else {
                sub_flags(v, value, other, load->size);
            }
            break;
        }
        case INSN_LOAD_BT: {
            // The bit within what was read: the processor picked that part
            // of memory by the offset.
            unsigned bit = (unsigned)other & (load->size * 8 - 1);
            uint64_t flags = vcpu_rflags(v) & ~(uint64_t)RFLAGS_CF;
            if ((value >> bit) & 1) {
                flags |= RFLAGS_CF;
            }
            vcpu_set_rflags(v, flags);
            break;
        }
        default: {
            set_reg(v, load->reg, load->high_byte, load->reg_size, value);
            break;
        }
    }
}
