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
#include <virt/alu.h>
#include <virt/emulate.h>
#include <virt/insn.h>
#include <virt/vcpu.h>

#define REX_R 0x04

// Iterations of a repeated string instruction emulated at a time: the
// instruction runs again for the rest, letting interrupts in between, as the
// processor does.
#define STRING_BATCH 1024

struct ctx {
    struct vcpu *v;
    const struct insn *insn;
    const struct emulate_mem *mem;
};

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

static unsigned group(const struct insn *insn) {
    return (insn->modrm >> 3) & 7;
}

static unsigned modrm_reg(const struct insn *insn) {
    return group(insn) | ((insn->rex & REX_R) ? 8 : 0);
}

// Without a REX prefix, byte registers 4 to 7 are AH, CH, DH, and BH.
static bool high_byte(const struct insn *insn, unsigned reg, unsigned size) {
    return size == 1 && insn->rex == 0 && reg >= 4 && reg < 8;
}

static uint64_t reg_get(struct ctx *c, unsigned reg, unsigned size) {
    if (high_byte(c->insn, reg, size)) {
        return (insn_gpr(c->v, reg - 4) >> 8) & 0xff;
    }
    return insn_gpr(c->v, reg) & size_mask(size);
}

static void reg_set(struct ctx *c, unsigned reg, unsigned size, uint64_t value) {
    if (high_byte(c->insn, reg, size)) {
        uint64_t old = insn_gpr(c->v, reg - 4);
        insn_set_gpr(c->v, reg - 4, (old & ~0xff00ull) | (value & 0xff) << 8);
    } else if (size == 4) {
        // 32-bit results are zero-extended.
        insn_set_gpr(c->v, reg, value & 0xffffffff);
    } else {
        uint64_t mask = size_mask(size);
        insn_set_gpr(c->v, reg, (insn_gpr(c->v, reg) & ~mask) | (value & mask));
    }
}

static bool mem_read(struct ctx *c, uint64_t linear, unsigned size, uint64_t *value) {
    uint64_t buf = 0;
    if (!c->mem->read(c->v, linear, &buf, size)) {
        return false;
    }
    *value = buf;
    return true;
}

static bool mem_write(struct ctx *c, uint64_t linear, unsigned size, uint64_t value) {
    return c->mem->write(c->v, linear, &value, size);
}

static uint64_t imm(const struct insn *insn) {
    return sign_extend(insn->imm, insn->imm_size);
}

static bool condition(uint64_t rflags, unsigned cc) {
    bool cf = (rflags & RFLAGS_CF) != 0;
    bool pf = (rflags & RFLAGS_PF) != 0;
    bool zf = (rflags & RFLAGS_ZF) != 0;
    bool sf = (rflags & RFLAGS_SF) != 0;
    bool of = (rflags & RFLAGS_OF) != 0;
    bool result;
    switch (cc >> 1) {
        case 0: {
            result = of;
            break;
        }
        case 1: {
            result = cf;
            break;
        }
        case 2: {
            result = zf;
            break;
        }
        case 3: {
            result = cf || zf;
            break;
        }
        case 4: {
            result = sf;
            break;
        }
        case 5: {
            result = pf;
            break;
        }
        case 6: {
            result = sf != of;
            break;
        }
        default: {
            result = zf || sf != of;
            break;
        }
    }
    return (cc & 1) ? !result : result;
}

// Memory op= src: the flags are only the guest's once the write is done.
static enum emulate_result alu_mem(struct ctx *c, unsigned op, unsigned size, uint64_t linear,
                                   uint64_t src) {
    uint64_t value;
    if (!mem_read(c, linear, size, &value)) {
        return EMULATE_FAULT;
    }
    uint64_t rflags = vcpu_rflags(c->v);
    uint64_t result = alu(op, size, value, src, &rflags) & size_mask(size);
    bool writes = op != ALU_CMP && op != ALU_TEST && op != ALU_BT;
    if (writes && !mem_write(c, linear, size, result)) {
        return EMULATE_FAULT;
    }
    vcpu_set_rflags(c->v, rflags);
    return EMULATE_DONE;
}

// The ALU's ModRM forms: memory op= register, or register op= memory.
static enum emulate_result alu_modrm(struct ctx *c, unsigned op, bool to_reg, unsigned size,
                                     uint64_t linear) {
    unsigned reg = modrm_reg(c->insn);
    if (!to_reg) {
        return alu_mem(c, op, size, linear, reg_get(c, reg, size));
    }
    uint64_t value;
    if (!mem_read(c, linear, size, &value)) {
        return EMULATE_FAULT;
    }
    uint64_t rflags = vcpu_rflags(c->v);
    uint64_t result = alu(op, size, reg_get(c, reg, size), value, &rflags);
    if (op != ALU_CMP) {
        reg_set(c, reg, size, result);
    }
    vcpu_set_rflags(c->v, rflags);
    return EMULATE_DONE;
}

// A register bit offset reaches beyond the operand, in either direction.
static enum emulate_result bit_test_reg(struct ctx *c, unsigned op, uint64_t linear) {
    unsigned size = c->insn->operand_size;
    unsigned shift = size == 2 ? 4 : size == 4 ? 5 : 6;
    int64_t offset = (int64_t)sign_extend(reg_get(c, modrm_reg(c->insn), size), size);
    linear += (uint64_t)((offset >> shift) * (int64_t)size);
    if (!c->insn->long_mode) {
        linear &= 0xffffffff;
    }
    return alu_mem(c, op, size, linear, (uint64_t)offset & (size * 8 - 1));
}

static enum emulate_result load(struct ctx *c, unsigned reg, unsigned size, uint64_t linear) {
    uint64_t value;
    if (!mem_read(c, linear, size, &value)) {
        return EMULATE_FAULT;
    }
    reg_set(c, reg, size, value);
    return EMULATE_DONE;
}

static enum emulate_result store(struct ctx *c, unsigned size, uint64_t linear, uint64_t value) {
    return mem_write(c, linear, size, value) ? EMULATE_DONE : EMULATE_FAULT;
}

static enum emulate_result extend(struct ctx *c, unsigned from, bool sign, uint64_t linear) {
    uint64_t value;
    if (!mem_read(c, linear, from, &value)) {
        return EMULATE_FAULT;
    }
    if (sign) {
        value = sign_extend(value, from);
    }
    reg_set(c, modrm_reg(c->insn), c->insn->operand_size, value);
    return EMULATE_DONE;
}

static enum emulate_result exchange(struct ctx *c, unsigned size, uint64_t linear, bool add) {
    unsigned reg = modrm_reg(c->insn);
    uint64_t value;
    if (!mem_read(c, linear, size, &value)) {
        return EMULATE_FAULT;
    }
    uint64_t src = reg_get(c, reg, size);
    uint64_t rflags = vcpu_rflags(c->v);
    uint64_t result = add ? alu(ALU_ADD, size, value, src, &rflags) & size_mask(size) : src;
    if (!mem_write(c, linear, size, result)) {
        return EMULATE_FAULT;
    }
    reg_set(c, reg, size, value);
    vcpu_set_rflags(c->v, rflags);
    return EMULATE_DONE;
}

static enum emulate_result memory_op(struct ctx *c, uint64_t linear) {
    const struct insn *insn = c->insn;
    unsigned op = insn->opcode;
    // The byte forms are the even opcodes.
    unsigned size = (op & 1) ? insn->operand_size : 1;
    if (op < 0x40 && (op & 7) < 4) {
        return alu_modrm(c, op >> 3, (op & 2) != 0, size, linear);
    }
    switch (op) {
        case 0x80:
        case 0x81:
        case 0x82:
        case 0x83: {
            return alu_mem(c, group(insn), op == 0x82 ? 1 : size, linear, imm(insn));
        }
        case 0x84:
        case 0x85: {
            return alu_mem(c, ALU_TEST, size, linear, reg_get(c, modrm_reg(insn), size));
        }
        case 0x86:
        case 0x87: {
            return exchange(c, size, linear, false);
        }
        case 0x88:
        case 0x89: {
            return store(c, size, linear, reg_get(c, modrm_reg(insn), size));
        }
        case 0x8a:
        case 0x8b: {
            return load(c, modrm_reg(insn), size, linear);
        }
        case 0x8c: {
            // Segment registers are numbered as the encoding numbers them.
            if (group(insn) > SEG_GS) {
                return EMULATE_UNSUPPORTED;
            }
            struct segment segment;
            vcpu_get_seg(c->v, (enum seg_reg)group(insn), &segment);
            return store(c, 2, linear, segment.selector);
        }
        case 0x63: {
            // ARPL outside 64-bit mode.
            if (!insn->long_mode) {
                return EMULATE_UNSUPPORTED;
            }
            return extend(c, insn->operand_size == 2 ? 2 : 4, true, linear);
        }
        case 0xa0:
        case 0xa1: {
            return load(c, 0, size, linear);
        }
        case 0xa2:
        case 0xa3: {
            return store(c, size, linear, reg_get(c, 0, size));
        }
        case 0xc0:
        case 0xc1: {
            return alu_mem(c, ALU_ROL + group(insn), size, linear, insn->imm);
        }
        case 0xd0:
        case 0xd1: {
            return alu_mem(c, ALU_ROL + group(insn), size, linear, 1);
        }
        case 0xd2:
        case 0xd3: {
            return alu_mem(c, ALU_ROL + group(insn), size, linear, c->v->gprs.rcx & 0xff);
        }
        case 0xc6:
        case 0xc7: {
            if (group(insn) != 0) {
                return EMULATE_UNSUPPORTED;
            }
            return store(c, size, linear, imm(insn));
        }
        case 0xf6:
        case 0xf7: {
            switch (group(insn)) {
                case 0:
                case 1: {
                    return alu_mem(c, ALU_TEST, size, linear, imm(insn));
                }
                case 2: {
                    return alu_mem(c, ALU_NOT, size, linear, 0);
                }
                case 3: {
                    return alu_mem(c, ALU_NEG, size, linear, 0);
                }
                default: {
                    return EMULATE_UNSUPPORTED;
                }
            }
        }
        case 0xfe:
        case 0xff: {
            if (group(insn) > 1) {
                return EMULATE_UNSUPPORTED;
            }
            return alu_mem(c, group(insn) == 0 ? ALU_INC : ALU_DEC, size, linear, 0);
        }
        case INSN_OPCODE_0F | 0xa3:
        case INSN_OPCODE_0F | 0xab:
        case INSN_OPCODE_0F | 0xb3:
        case INSN_OPCODE_0F | 0xbb: {
            return bit_test_reg(c, ALU_BT + ((op >> 3) & 3), linear);
        }
        case INSN_OPCODE_0F | 0xba: {
            if (group(insn) < 4) {
                return EMULATE_UNSUPPORTED;
            }
            unsigned bits = insn->operand_size * 8;
            return alu_mem(c, ALU_BT + group(insn) - 4, insn->operand_size, linear,
                           insn->imm & (bits - 1));
        }
        case INSN_OPCODE_0F | 0xb6:
        case INSN_OPCODE_0F | 0xb7: {
            return extend(c, (op & 1) ? 2 : 1, false, linear);
        }
        case INSN_OPCODE_0F | 0xbe:
        case INSN_OPCODE_0F | 0xbf: {
            return extend(c, (op & 1) ? 2 : 1, true, linear);
        }
        case INSN_OPCODE_0F | 0xc0:
        case INSN_OPCODE_0F | 0xc1: {
            return exchange(c, (op & 1) ? insn->operand_size : 1, linear, true);
        }
        default: {
            break;
        }
    }
    if (op >= (INSN_OPCODE_0F | 0x90) && op <= (INSN_OPCODE_0F | 0x9f)) {
        return store(c, 1, linear, condition(vcpu_rflags(c->v), op & 0xf));
    }
    return EMULATE_UNSUPPORTED;
}

// A register a string instruction steps, as wide as its addresses: the upper
// bits stay put but for 32-bit addresses, whose results are zero-extended.
static uint64_t string_step(uint64_t reg, int64_t delta, unsigned address_size) {
    if (address_size == 8) {
        return reg + (uint64_t)delta;
    }
    if (address_size == 4) {
        return (uint32_t)(reg + (uint64_t)delta);
    }
    return (reg & ~0xffffull) | ((reg + (uint64_t)delta) & 0xffff);
}

// One iteration. Returns false where it faulted or must not be emulated,
// setting *result.
static bool string_once(struct ctx *c, unsigned size, enum emulate_result *result) {
    struct vcpu *v = c->v;
    const struct insn *insn = c->insn;
    uint64_t src = insn_string_linear(v, insn, false);
    uint64_t dst = insn_string_linear(v, insn, true);
    uint16_t port = (uint16_t)v->gprs.rdx;
    uint64_t a;
    uint64_t b;
    uint32_t data;
    *result = EMULATE_FAULT;
    switch (insn->opcode) {
        case 0xa4:
        case 0xa5: {
            return mem_read(c, src, size, &a) && mem_write(c, dst, size, a);
        }
        case 0xaa:
        case 0xab: {
            return mem_write(c, dst, size, reg_get(c, 0, size));
        }
        case 0xac:
        case 0xad: {
            if (!mem_read(c, src, size, &a)) {
                return false;
            }
            reg_set(c, 0, size, a);
            return true;
        }
        case 0xa6:
        case 0xa7: {
            if (!mem_read(c, src, size, &a) || !mem_read(c, dst, size, &b)) {
                return false;
            }
            uint64_t rflags = vcpu_rflags(v);
            alu(ALU_CMP, size, a, b, &rflags);
            vcpu_set_rflags(v, rflags);
            return true;
        }
        case 0xae:
        case 0xaf: {
            if (!mem_read(c, dst, size, &b)) {
                return false;
            }
            uint64_t rflags = vcpu_rflags(v);
            alu(ALU_CMP, size, reg_get(c, 0, size), b, &rflags);
            vcpu_set_rflags(v, rflags);
            return true;
        }
        case 0x6c:
        case 0x6d: {
            if (!c->mem->ins(v, dst, port, size, &data)) {
                *result = EMULATE_UNSUPPORTED;
                return false;
            }
            return mem_write(c, dst, size, data);
        }
        default: {
            if (!mem_read(c, src, size, &a)) {
                return false;
            }
            c->mem->outs(v, port, size, (uint32_t)a);
            return true;
        }
    }
}

static enum emulate_result string_op(struct ctx *c) {
    struct vcpu *v = c->v;
    const struct insn *insn = c->insn;
    unsigned op = insn->opcode;
    bool io = op >= 0x6c && op <= 0x6f;
    // Port I/O is at most 32 bits wide.
    unsigned size = !(op & 1) ? 1 : io && insn->operand_size == 8 ? 4 : insn->operand_size;
    int64_t delta = (vcpu_rflags(v) & RFLAGS_DF) ? -(int64_t)size : (int64_t)size;
    bool uses_si = op == 0xa4 || op == 0xa5 || op == 0xa6 || op == 0xa7 || op == 0xac
                   || op == 0xad || op == 0x6e || op == 0x6f;
    bool uses_di = op != 0xac && op != 0xad && op != 0x6e && op != 0x6f;
    bool compares = op == 0xa6 || op == 0xa7 || op == 0xae || op == 0xaf;
    unsigned address_size = insn->address_size;
    uint64_t mask = address_size == 8 ? UINT64_MAX : (1ull << (address_size * 8)) - 1;
    uint64_t count = insn->rep ? v->gprs.rcx & mask : 1;
    // A single-stepping guest traps after each iteration.
    bool step = vcpu_guest_tf(v);

    for (unsigned n = 0; count != 0; n++) {
        if (n == (step ? 1u : STRING_BATCH)) {
            // The rest when the instruction runs again.
            if (step) {
                vcpu_step_trap(v);
            }
            return EMULATE_DONE;
        }
        enum emulate_result result;
        if (!string_once(c, size, &result)) {
            // Where only part of the iterations happened, the instruction runs
            // again for the rest.
            return n != 0 && result == EMULATE_UNSUPPORTED ? EMULATE_DONE : result;
        }
        if (uses_si) {
            v->gprs.rsi = string_step(v->gprs.rsi, delta, address_size);
        }
        if (uses_di) {
            v->gprs.rdi = string_step(v->gprs.rdi, delta, address_size);
        }
        if (!insn->rep) {
            break;
        }
        v->gprs.rcx = string_step(v->gprs.rcx, -1, address_size);
        count--;
        // REPE ends on a mismatch, REPNE on a match.
        if (compares) {
            bool zf = (vcpu_rflags(v) & RFLAGS_ZF) != 0;
            if (zf != (insn->rep_prefix == 0xf3)) {
                break;
            }
        }
    }
    vcpu_advance(v, insn->length);
    return EMULATE_DONE;
}

enum emulate_result emulate(struct vcpu *v, const struct insn *insn, const struct emulate_mem *mem) {
    struct ctx c = {v, insn, mem};
    if (insn->vex) {
        return EMULATE_UNSUPPORTED;
    }
    unsigned op = insn->opcode;
    if ((op >= 0x6c && op <= 0x6f) || (op >= 0xa4 && op <= 0xa7) || (op >= 0xaa && op <= 0xaf)) {
        return string_op(&c);
    }
    uint64_t linear;
    if (!insn_mem_linear(v, insn, &linear)) {
        return EMULATE_UNSUPPORTED;
    }
    enum emulate_result result = memory_op(&c, linear);
    if (result == EMULATE_DONE) {
        vcpu_advance(v, insn->length);
    }
    return result;
}
