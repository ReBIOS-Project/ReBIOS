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

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <virt/run.h>
#include <lib/heap.h>
#include <lib/log.h>
#include <lib/time.h>
#include <lib/util.h>
#include <memory.h>
#include <sys/cpu.h>
#include <sys/mm.h>
#include <sys/x86.h>
#include <virt/alu.h>
#include <virt/emulate.h>
#include <virt/event.h>
#include <virt/guest_mem.h>
#include <virt/hv_timer.h>
#include <virt/insn.h>
#include <virt/io.h>
#include <virt/mmio.h>
#include <virt/nsvm.h>
#include <virt/nvmx.h>
#include <virt/profile.h>
#include <virt/vcpu.h>
#include <virt/vga.h>
#include <virt/vgabios.h>

// A run ends after this many instructions in a row that reach no region that
// begins runs and none of the emulated VGA's ports, and at the end of the
// processor's time slice: more where exits take long, as under another
// hypervisor, as each run saves one.
#define RUN_IDLE 64
#define RUN_IDLE_SLOW_EXITS 1024
// What the guest is to take, and the hypervisor's timers, are looked at every
// this many instructions, and a repeated string instruction's iterations.
#define RUN_CHECK 16

// The pages a run reaches, which it looks up in the guest's paging and the
// hypervisor's once each, as a processor's TLB holds them; and the decoded
// instructions it keeps, from run to run, with the bytes they were decoded
// from.
#define RUN_PAGES 16
#define RUN_DECODED 4096

#define NEED_READ 1
#define NEED_WRITE 2
#define NEED_FETCH 4

// Segment attributes, in the packed form struct vcpu_segment has.
#define SEG_PRESENT ((uint32_t)1 << 7)
#define SEG_CODE ((uint32_t)1 << 3)
#define SEG_EXPAND_DOWN ((uint32_t)1 << 2)
#define SEG_RW ((uint32_t)1 << 1)

// The FLAGS bits that IRET and POPF load in real mode, and EFLAGS' that POPF
// loads at CPL 0 in protected mode (SDM 325462-093 Vol. 2B, POPF).
#define FLAGS_LOADED 0x7fd5
#define EFLAGS_LOADED 0x257fd5
#define FLAGS_IOPL 0x3000

// The ports whose permission a run looked up in the TSS's bitmap.
#define RUN_PORTS 4

// An STI that set IF, or a load of SS by MOV or POP, which keep interrupts off
// until the next instruction is done (SDM 325462-093 Vol. 2B, STI; Vol. 3A,
// 7.8.3).
#define SHADOW_NONE 0
#define SHADOW_STI 1
#define SHADOW_SS 2

// Why a run ended, before the instruction at the guest's RIP, but for
// STOP_PARTIAL, where it is part done.
#define STOP_NONE 0
#define STOP_IDLE 1
#define STOP_TIME 2
#define STOP_EVENT 3
#define STOP_CODE 4
#define STOP_UNSUPPORTED 5
#define STOP_OPERAND 6
#define STOP_FAULT 7
#define STOP_DEVICE 8
#define STOP_RETRY 9
#define STOP_PARTIAL 10
#define STOP_FLAGS 11
#define STOP_COUNT 12

static const char *const stop_names[STOP_COUNT] = {
    "none", "idle", "time", "event", "code", "unsupported", "operand", "fault", "device", "retry",
    "partial", "flags"
};

struct run_page {
    uintptr_t linear;
    uintptr_t gpa;
    uint8_t *host;
    const void *region;
    int place;
    // NEED_*, as walks have checked them.
    unsigned int rights;
};

struct run_decoded {
    uintptr_t linear;
    // CS's attributes, RFLAGS.VM, and 64-bit mode, which the decoding depends
    // on.
    uint32_t mode;
    uint8_t bytes[INSN_MAX_LENGTH];
    struct insn insn;
};

struct run_segment {
    uint64_t base;
    uint64_t limit;
    uint32_t attrib;
    uint16_t selector;
    // As the run found them, which it writes back only where they differ.
    uint16_t loaded_selector;
    uint64_t loaded_base;
    // Whether an access's offset and size are all there is to check: a usable,
    // present, writable data segment that does not expand down, outside IA-32e
    // mode.
    bool plain;
};

// The guest's registers while a run carries out its instructions, written back
// as it ends, and what does not change meanwhile.
struct run_state {
    struct run_page pages[RUN_PAGES];
    size_t page_count;
    size_t victim;
    uint64_t gpr[16];
    uint64_t rip;
    uint64_t rflags;
    bool lma;
    bool long_mode;
    bool real;
    bool v86;
    uint32_t mode;
    // The segments read so far, and those that real-mode and virtual-8086
    // loads changed.
    uint32_t segs_loaded;
    uint32_t segs_changed;
    struct run_segment segs[VCPU_SEG_GS + 1];
    // The page the last instruction came from.
    uintptr_t code_linear;
    const uint8_t *code_host;
    // Whether the instruction reached a region that begins runs or the VGA's
    // ports.
    bool reached;
    // SHADOW_*: what keeps interrupts off until the instruction at hand is
    // done.
    int shadow;
    // Whether events are to be looked at before the next instruction, as IF
    // was set.
    bool recheck;
    // Whether the instruction at RIP is part done, as a repeated string
    // instruction can be: RF is then set.
    bool partial;
    // Ports and sizes the TSS's bitmap lets through, the port in bits 15:0.
    uint32_t ports[RUN_PORTS];
    size_t port_count;
    struct run_decoded decoded[RUN_DECODED];
};

// The opcodes that ended runs as unsupported: the one-byte map's, the 0F map's
// from 100h on, and the others' at 200h.
#define OPCODE_SLOTS 0x201
#define TOP_OPCODES 8

static _Atomic(uint64_t) run_count;
static _Atomic(uint64_t) insn_count;
static _Atomic(uint64_t) access_count;
static _Atomic(uint64_t) decode_count;
static _Atomic(uint64_t) run_ticks;
static _Atomic(uint64_t) stop_counts[STOP_COUNT];
static _Atomic(uint64_t) opcode_counts[OPCODE_SLOTS];

static void store_state(struct vcpu *v, struct run_state *rs, bool last);
static void load_state(struct vcpu *v, struct run_state *rs);

static uint64_t size_mask(size_t size) {
    return size >= 8 ? UINT64_MAX : ((uint64_t)1 << (size * 8)) - 1;
}

static uint64_t sign_extend(uint64_t value, size_t size) {
    if (size == 0 || size >= 8) {
        return value;
    }
    unsigned int shift = 64 - (unsigned int)size * 8;
    return (uint64_t)((int64_t)(value << shift) >> shift);
}

static unsigned int reg_field(const struct insn *insn) {
    return ((insn->modrm >> 3) & 7) | ((insn->rex & INSN_REX_R) != 0 ? 8 : 0);
}

static unsigned int rm_field(const struct insn *insn) {
    return (insn->modrm & 7) | ((insn->rex & INSN_REX_B) != 0 ? 8 : 0);
}

// The register in an opcode's low bits, as MOV's B8h + r has it.
static unsigned int opcode_reg(const struct insn *insn) {
    return (insn->opcode_byte & 7) | ((insn->rex & INSN_REX_B) != 0 ? 8 : 0);
}

// Without a REX prefix, byte registers 4 to 7 are AH, CH, DH, and BH.
static uint64_t get(
    const struct run_state *rs,
    const struct insn *insn,
    unsigned int reg,
    size_t size
) {
    if (size == 1 && insn->rex == 0 && reg >= 4 && reg < 8) {
        return (rs->gpr[reg - 4] >> 8) & 0xff;
    }
    return rs->gpr[reg] & size_mask(size);
}

static void set(
    struct run_state *rs,
    const struct insn *insn,
    unsigned int reg,
    size_t size,
    uint64_t value
) {
    if (size == 1 && insn->rex == 0 && reg >= 4 && reg < 8) {
        uint64_t *r = &rs->gpr[reg - 4];
        *r = (*r & ~(uint64_t)0xff00) | (value & 0xff) << 8;
    } else if (size == 4) {
        // 32-bit results are zero-extended.
        rs->gpr[reg] = value & 0xffffffff;
    } else {
        uint64_t mask = size_mask(size);
        rs->gpr[reg] = (rs->gpr[reg] & ~mask) | (value & mask);
    }
}

// Real mode's is 0, and virtual-8086 mode's 3.
static unsigned int cpl(struct vcpu *v, const struct run_state *rs) {
    return rs->real ? 0 : rs->v86 ? 3 : vcpu_cpl(v);
}

static bool canonical(struct vcpu *v, uint64_t address) {
    unsigned int bits = (vcpu_cr4(v) & CR4_LA57) != 0 ? 57 : 48;
    uint64_t upper = address >> (bits - 1);
    return upper == 0 || upper == UINT64_MAX >> (bits - 1);
}

// A segment register as the vCPU has it, read as the run first uses it: in
// virtual-8086 mode, where the selector gives all (SDM 325462-093 Vol. 3C,
// 29.3.1.2), the selector alone.
static struct run_segment *segment(struct vcpu *v, struct run_state *rs, int reg) {
    struct run_segment *s = &rs->segs[reg];
    if ((rs->segs_loaded & ((uint32_t)1 << reg)) == 0 && rs->v86) {
        s->selector = vcpu_seg_selector(v, (size_t)reg);
        s->base = (uint64_t)s->selector << 4;
        s->limit = 0xffff;
        s->attrib = 0xf3;
        s->plain = true;
        s->loaded_selector = s->selector;
        s->loaded_base = s->base;
        rs->segs_loaded |= (uint32_t)1 << reg;
    } else if ((rs->segs_loaded & ((uint32_t)1 << reg)) == 0) {
        uint32_t attrib = vcpu_seg_attrib(v, (size_t)reg);
        s->base = vcpu_seg_base(v, (size_t)reg);
        s->limit = vcpu_seg_limit(v, (size_t)reg);
        s->attrib = attrib;
        s->selector = vcpu_seg_selector(v, (size_t)reg);
        s->loaded_selector = s->selector;
        s->loaded_base = s->base;
        s->plain = !rs->lma && (attrib & VCPU_SEG_UNUSABLE) == 0 && (attrib & SEG_PRESENT) != 0
                && (attrib & SEG_CODE) == 0 && (attrib & SEG_RW) != 0
                && (attrib & SEG_EXPAND_DOWN) == 0;
        rs->segs_loaded |= (uint32_t)1 << reg;
    }
    return s;
}

// A segment register load in real or virtual-8086 mode: the base is the
// selector's sixteenfold, and the limit and attributes stay.
static void load_segment(struct vcpu *v, struct run_state *rs, int reg, uint16_t selector) {
    struct run_segment *s = segment(v, rs, reg);
    s->selector = selector;
    s->base = (uint64_t)selector << 4;
    rs->segs_changed |= (uint32_t)1 << reg;
}

static size_t stack_size(struct vcpu *v, struct run_state *rs) {
    if (rs->long_mode) {
        return 8;
    }
    return (segment(v, rs, VCPU_SEG_SS)->attrib & VCPU_SEG_DB) != 0 ? 4 : 2;
}

// The page holding linear, for accesses needing need: NULL where the processor
// would fault there, *fault then true, or reach anything but memory or a
// region that begins runs.
static struct run_page *page_get(
    struct vcpu *v,
    struct run_state *rs,
    uintptr_t linear,
    unsigned int need,
    bool *fault
) {
    uintptr_t page = linear & ~(uintptr_t)(PAGE_SIZE - 1);
    struct run_page *p = NULL;
    for (size_t i = 0; i < rs->page_count; i++) {
        if (rs->pages[i].linear == page) {
            p = &rs->pages[i];
            break;
        }
    }
    if (p != NULL && (p->rights & need) == need) {
        return p;
    }
    bool write = (need & NEED_WRITE) != 0;
    bool fetch = (need & NEED_FETCH) != 0;
    uint64_t gpa;
    uint32_t pf_error;
    if (!guest_mem_linear_to_phys(v, linear, write, fetch, &gpa, &pf_error)) {
        *fault = true;
        return NULL;
    }
    uint8_t *host = NULL;
    const void *region = NULL;
    int place = mmio_page(gpa, write, &host, &region);
    if (place == MMIO_PLACE_OTHER || (fetch && place != MMIO_PLACE_RAM)) {
        return NULL;
    }
    gpa = page_down(gpa);
    if (p == NULL || p->gpa != gpa || p->place != place) {
        if (p == NULL) {
            if (rs->page_count < RUN_PAGES) {
                p = &rs->pages[rs->page_count++];
            } else {
                p = &rs->pages[rs->victim++ % RUN_PAGES];
            }
        }
        *p = (struct run_page){ page, gpa, host, region, place, 0 };
    }
    // A walk for a write checked the access's reads too.
    p->rights |= need | (write ? NEED_READ : 0);
    return p;
}

// Whether size bytes at linear all reach the same place with the access:
// STOP_NONE, with that place, or why not.
static int reach(
    struct vcpu *v,
    struct run_state *rs,
    uintptr_t linear,
    size_t size,
    unsigned int need,
    int *place
) {
    // Outside 64-bit mode the linear address space wraps at 4 GiB.
    if (!rs->long_mode && linear + size > ((uint64_t)1 << 32)) {
        return STOP_DEVICE;
    }
    *place = MMIO_PLACE_OTHER;
    for (uintptr_t at = linear; at < linear + size; at = page_down(at) + PAGE_SIZE) {
        bool fault = false;
        struct run_page *p = page_get(v, rs, at, need, &fault);
        if (p == NULL) {
            return fault ? STOP_FAULT : STOP_DEVICE;
        }
        if (*place != MMIO_PLACE_OTHER && p->place != *place) {
            return STOP_DEVICE;
        }
        *place = p->place;
    }
    if (*place == MMIO_PLACE_RUN) {
        rs->reached = true;
    }
    return STOP_NONE;
}

// RAM the guest reaches, by the access of the size that its instruction makes
// there, as atomic as the guest's own would be, which only the processor's
// guarantees bound (SDM 325462-093 Vol. 3A, 11.1.1); a part of one that
// crosses a page, which none of them covers, a byte at a time.
static uint64_t ram_load(const uint8_t *p, size_t size) {
    switch (size) {
        case 1: {
            return *(const volatile uint8_t *)p;
        }
        case 2: {
            return *(const volatile uint16_t *)p;
        }
        case 4: {
            return *(const volatile uint32_t *)p;
        }
        case 8: {
            return *(const volatile uint64_t *)p;
        }
        default: {
            uint64_t value = 0;
            for (size_t i = 0; i < size; i++) {
                value |= (uint64_t)((const volatile uint8_t *)p)[i] << (i * 8);
            }
            return value;
        }
    }
}

static void ram_store(uint8_t *p, size_t size, uint64_t value) {
    switch (size) {
        case 1: {
            *(volatile uint8_t *)p = (uint8_t)value;
            break;
        }
        case 2: {
            *(volatile uint16_t *)p = (uint16_t)value;
            break;
        }
        case 4: {
            *(volatile uint32_t *)p = (uint32_t)value;
            break;
        }
        case 8: {
            *(volatile uint64_t *)p = value;
            break;
        }
        default: {
            for (size_t i = 0; i < size; i++) {
                ((volatile uint8_t *)p)[i] = (uint8_t)(value >> (i * 8));
            }
            break;
        }
    }
}

// An access of size bytes at linear, whose pages reach() found: what it reads,
// or 0 where it writes value.
static uint64_t linear_access(
    struct vcpu *v,
    struct run_state *rs,
    uintptr_t linear,
    size_t size,
    bool write,
    uint64_t value
) {
    uint64_t result = 0;
    for (size_t done = 0; done < size; ) {
        bool fault = false;
        struct run_page *page = page_get(v, rs, linear, write ? NEED_WRITE : NEED_READ, &fault);
        if (page == NULL) {
            panic("A run lost its page at %#llx\n", (unsigned long long)linear);
        }
        size_t offset = linear & (PAGE_SIZE - 1);
        size_t chunk = PAGE_SIZE - offset < size - done ? PAGE_SIZE - offset : size - done;
        uint64_t part = value >> (done * 8);
        if (page->place == MMIO_PLACE_RUN) {
            uint8_t buf[8];
            write_le64(buf, part);
            mmio_region_access(v, page->region, page->gpa + offset, buf, chunk, write);
            part = read_le64(buf);
        } else if (write) {
            ram_store(page->host + offset, chunk, part);
        } else {
            part = ram_load(page->host + offset, chunk);
        }
        if (!write) {
            result |= (part & size_mask(chunk)) << (done * 8);
        }
        linear += chunk;
        done += chunk;
    }
    return result;
}

static uint64_t read_linear(struct vcpu *v, struct run_state *rs, uintptr_t linear, size_t size) {
    return linear_access(v, rs, linear, size, false, 0);
}

static void write_linear(
    struct vcpu *v,
    struct run_state *rs,
    uintptr_t linear,
    size_t size,
    uint64_t value
) {
    linear_access(v, rs, linear, size, true, value);
}

// Offset ea of a segment, for size bytes, with the checks of SDM 325462-093
// Vol. 3A, 3.3.7.1, 5.3, and 5.4 that insn.c's linear_allowed() makes, on the
// run's segments: STOP_NONE, with the linear address, or STOP_FAULT where the
// processor would fault.
static int segment_check(
    struct vcpu *v,
    struct run_state *rs,
    int reg,
    uint64_t ea,
    size_t size,
    bool write,
    uintptr_t *linear
) {
    // In IA-32e mode, where the run loads no segments, insn.c checks.
    if (rs->lma) {
        uint64_t la;
        if (!insn_segment_allowed(v, reg, ea, size, write, &la)) {
            return STOP_FAULT;
        }
        *linear = la;
        return STOP_NONE;
    }
    const struct run_segment *s = segment(v, rs, reg);
    if (s->plain && ea <= s->limit && size - 1 <= s->limit - ea) {
        *linear = (s->base + ea) & 0xffffffff;
        return STOP_NONE;
    }
    uint32_t attrib = s->attrib;
    bool code = (attrib & SEG_CODE) != 0;
    uint64_t limit = s->limit;
    bool allowed = (attrib & VCPU_SEG_UNUSABLE) == 0 && (attrib & SEG_PRESENT) != 0;
    if (write && ((code && !rs->real) || (attrib & SEG_RW) == 0)) {
        allowed = false;
    }
    if (!write && code && (attrib & SEG_RW) == 0) {
        allowed = false;
    }
    if (!code && (attrib & SEG_EXPAND_DOWN) != 0) {
        allowed &= ea > limit;
        limit = (attrib & VCPU_SEG_DB) != 0 ? 0xffffffff : 0xffff;
    }
    if (!allowed || ea > limit || (limit != 0xffffffff && size > limit + 1 - ea)) {
        return STOP_FAULT;
    }
    *linear = (s->base + ea) & 0xffffffff;
    return STOP_NONE;
}

// The effective address of the ModRM or offset operand, and its segment, as
// insn.c's mem_ea() forms it, from the run's registers.
static bool effective_address(
    const struct run_state *rs,
    const struct insn *insn,
    int *segment_out,
    uint64_t *ea_out
) {
    int seg = VCPU_SEG_DS;
    uint64_t ea;
    unsigned int mod = insn->modrm >> 6;
    unsigned int rm = insn->modrm & 7;
    if (insn->opcode >= 0xa0 && insn->opcode <= 0xa3) {
        ea = insn->imm;
    } else if (!insn->has_modrm || mod == 3) {
        return false;
    } else if (insn->address_size == 2) {
        const uint64_t *g = rs->gpr;
        uint64_t sums[8] = {
            g[3] + g[6], g[3] + g[7], g[5] + g[6], g[5] + g[7], g[6], g[7], g[5], g[3]
        };
        ea = (uint64_t)insn->disp;
        if (rm != 6 || mod != 0) {
            ea += sums[rm];
            if (rm == 2 || rm == 3 || rm == 6) {
                seg = VCPU_SEG_SS;
            }
        }
        ea &= 0xffff;
    } else {
        ea = (uint64_t)insn->disp;
        unsigned int base = rm;
        if (insn->has_sib) {
            base = insn->sib & 7;
            size_t index = ((insn->sib >> 3) & 7) | ((insn->rex & INSN_REX_X) != 0 ? 8 : 0);
            if (index != 4) {
                ea += rs->gpr[index] << (insn->sib >> 6);
            }
        }
        if (mod == 0 && base == 5) {
            // No base: in 64-bit mode, without a SIB byte, RIP-relative.
            if (!insn->has_sib && insn->long_mode) {
                ea += rs->rip + insn->length;
            }
        } else {
            ea += rs->gpr[base | ((insn->rex & INSN_REX_B) != 0 ? 8 : 0)];
            if (base == 4 || base == 5) {
                seg = VCPU_SEG_SS;
            }
        }
        ea &= size_mask(insn->address_size);
    }
    if (insn->segment >= 0) {
        seg = insn->segment;
    }
    *segment_out = seg;
    *ea_out = ea;
    return true;
}

// A register, or memory the run reaches.
struct operand {
    bool memory;
    unsigned int reg;
    uintptr_t linear;
    size_t size;
    int place;
};

// The ModRM memory operand, or MOV's offset one, of size bytes at a delta from
// its effective address, which wraps at the address size, as the second word
// of a far pointer does.
static int memory_operand_at(
    struct vcpu *v,
    struct run_state *rs,
    const struct insn *insn,
    uint64_t delta,
    size_t size,
    unsigned int need,
    struct operand *o
) {
    int seg;
    uint64_t ea;
    if (!effective_address(rs, insn, &seg, &ea)) {
        return STOP_OPERAND;
    }
    ea = (ea + delta) & size_mask(insn->address_size);
    uintptr_t linear;
    int stop = segment_check(v, rs, seg, ea, size, (need & NEED_WRITE) != 0, &linear);
    if (stop != STOP_NONE) {
        return stop;
    }
    int place;
    stop = reach(v, rs, linear, size, need, &place);
    if (stop != STOP_NONE) {
        return stop;
    }
    *o = (struct operand){ .memory = true, .linear = linear, .size = size, .place = place };
    return STOP_NONE;
}

static int memory_operand(
    struct vcpu *v,
    struct run_state *rs,
    const struct insn *insn,
    size_t size,
    unsigned int need,
    struct operand *o
) {
    return memory_operand_at(v, rs, insn, 0, size, need, o);
}

// The ModRM r/m operand of size bytes, for accesses needing need.
static int rm_operand(
    struct vcpu *v,
    struct run_state *rs,
    const struct insn *insn,
    size_t size,
    unsigned int need,
    struct operand *o
) {
    if (insn->modrm >> 6 == 3) {
        *o = (struct operand){ .reg = rm_field(insn), .size = size };
        return STOP_NONE;
    }
    return memory_operand(v, rs, insn, size, need, o);
}

static uint64_t load(
    struct vcpu *v,
    struct run_state *rs,
    const struct insn *insn,
    const struct operand *o
) {
    if (!o->memory) {
        return get(rs, insn, o->reg, o->size);
    }
    return read_linear(v, rs, o->linear, o->size);
}

static void store(
    struct vcpu *v,
    struct run_state *rs,
    const struct insn *insn,
    const struct operand *o,
    uint64_t value
) {
    if (!o->memory) {
        set(rs, insn, o->reg, o->size, value);
        return;
    }
    write_linear(v, rs, o->linear, o->size, value);
}

// Outside 64-bit mode RIP wraps at 4 GiB, as vcpu_advance() has it.
static int next(struct run_state *rs, const struct insn *insn) {
    rs->rip += insn->length;
    if (!rs->long_mode) {
        rs->rip &= 0xffffffff;
    }
    return STOP_NONE;
}

// A near branch's target, which the processor checks against the code
// segment's limit, or in 64-bit mode for being canonical. In 64-bit mode, near
// branches stay 64-bit with 66h on Intel's processors (SDM 325462-093 Vol. 1,
// 7.3.8.3) but not on AMD's (APM 40332 rev. 4.11, Vol. 3, 2.5.6), so those are
// left to the processor.
static int near_target(
    struct vcpu *v,
    struct run_state *rs,
    const struct insn *insn,
    uint64_t *target
) {
    if (insn->long_mode) {
        if (insn->operand_size == 2) {
            return STOP_UNSUPPORTED;
        }
        return canonical(v, *target) ? STOP_NONE : STOP_FAULT;
    }
    *target &= size_mask(insn->operand_size);
    return *target <= segment(v, rs, VCPU_SEG_CS)->limit ? STOP_NONE : STOP_FAULT;
}

static int go_to(struct vcpu *v, struct run_state *rs, const struct insn *insn, uint64_t target) {
    int stop = near_target(v, rs, insn, &target);
    if (stop != STOP_NONE) {
        return stop;
    }
    rs->rip = target;
    return STOP_NONE;
}

// Jcc and JMP with a relative target, which they go to where taken is true.
static int relative(struct vcpu *v, struct run_state *rs, const struct insn *insn, bool taken) {
    if (insn->long_mode && insn->operand_size == 2) {
        return STOP_UNSUPPORTED;
    }
    if (!taken) {
        return next(rs, insn);
    }
    uint64_t target = rs->rip + insn->length + sign_extend(insn->imm, insn->imm_size);
    return go_to(v, rs, insn, target);
}

// A register an instruction steps, as wide as its addresses: the upper bits
// stay put but for 32-bit addresses, whose results are zero-extended.
static uint64_t address_step(uint64_t reg, int64_t delta, size_t address_size) {
    if (address_size == 8) {
        return reg + (uint64_t)delta;
    }
    if (address_size == 4) {
        return (uint32_t)(reg + (uint64_t)delta);
    }
    return (reg & ~(uint64_t)0xffff) | ((reg + (uint64_t)delta) & 0xffff);
}

// LOOP, LOOPE, LOOPNE, and JCXZ, on the counter the address size gives.
static int loop(struct vcpu *v, struct run_state *rs, const struct insn *insn) {
    if (insn->long_mode && insn->operand_size == 2) {
        return STOP_UNSUPPORTED;
    }
    uint64_t mask = size_mask(insn->address_size);
    uint64_t rcx = rs->gpr[1];
    bool taken;
    if (insn->opcode == 0xe3) {
        taken = (rcx & mask) == 0;
    } else {
        rcx = address_step(rcx, -1, insn->address_size);
        bool zf = (rs->rflags & RFLAGS_ZF) != 0;
        bool condition = insn->opcode == 0xe2 || (insn->opcode == 0xe1) == zf;
        taken = (rcx & mask) != 0 && condition;
    }
    uint64_t target = rs->rip + insn->length + sign_extend(insn->imm, insn->imm_size);
    if (taken) {
        int stop = near_target(v, rs, insn, &target);
        if (stop != STOP_NONE) {
            return stop;
        }
    }
    rs->gpr[1] = rcx;
    if (taken) {
        rs->rip = target;
        return STOP_NONE;
    }
    return next(rs, insn);
}

// dst op= src, with the status flags.
static int alu_operand(
    struct vcpu *v,
    struct run_state *rs,
    const struct insn *insn,
    unsigned int op,
    const struct operand *dst,
    uint64_t src
) {
    uint64_t result = alu(op, dst->size, load(v, rs, insn, dst), src, &rs->rflags);
    if (op != ALU_CMP && op != ALU_TEST && op != ALU_BT) {
        store(v, rs, insn, dst, result);
    }
    return next(rs, insn);
}

// Whether DIV or IDIV of size bytes divides into a quotient that fits: one
// that does not, or a zero divisor, raises #DE, which the processor does
// itself. IDIV's most negative quotient is left to it too.
static bool quotient_fits(bool is_signed, size_t size, uint64_t rax, uint64_t rdx, uint64_t src) {
    uint64_t mask = size_mask(size);
    unsigned int bits = (unsigned int)size * 8;
    src &= mask;
    if (src == 0) {
        return false;
    }
    // The byte forms divide AX alone.
    unsigned __int128 dividend = rax & 0xffff;
    if (size != 1) {
        dividend = ((unsigned __int128)(rdx & mask) << bits) | (rax & mask);
    }
    if (!is_signed) {
        return (dividend / src) >> bits == 0;
    }
    __int128 signed_dividend = (__int128)dividend;
    if (size != 8) {
        signed_dividend = (__int128)(int64_t)sign_extend((uint64_t)dividend, size * 2);
    }
    __int128 divisor = (int64_t)sign_extend(src, size);
    __int128 lowest = -((__int128)1 << (bits - 1));
    if (divisor == -1) {
        return signed_dividend > lowest && -signed_dividend > lowest;
    }
    __int128 quotient = signed_dividend / divisor;
    return quotient > lowest && quotient < -lowest;
}

// MUL, IMUL, DIV, and IDIV with one operand, src, on rAX and rDX.
static int wide(
    struct run_state *rs,
    const struct insn *insn,
    unsigned int group,
    size_t size,
    uint64_t src
) {
    unsigned int op = group - 4;
    uint64_t rax = rs->gpr[0];
    uint64_t rdx = rs->gpr[2];
    if (op >= ALU_DIV && !quotient_fits(op == ALU_IDIV, size, rax, rdx, src)) {
        return STOP_FAULT;
    }
    uint64_t high = rdx;
    uint64_t low = alu_wide(op, size, rax, &high, src, &rs->rflags);
    // The byte forms' results are AH and AL.
    if (size == 1) {
        set(rs, insn, 0, 2, low);
    } else {
        set(rs, insn, 0, size, low);
        set(rs, insn, 2, size, high);
    }
    return next(rs, insn);
}

// What PUSH, POP, CALL, and RET move: in 64-bit mode, 64 bits but with 66h
// (SDM 325462-093 Vol. 1, 6.2.5).
static size_t stack_slot(const struct insn *insn) {
    if (insn->long_mode) {
        return insn->operand_size == 2 ? 2 : 8;
    }
    return insn->operand_size;
}

// bytes at offset sp of SS, all in RAM the run reaches with the access, and
// their linear address. The stack pointer wrapping is left to the processor.
static int stack_reach(
    struct vcpu *v,
    struct run_state *rs,
    uint64_t sp,
    size_t bytes,
    bool write,
    uintptr_t *linear
) {
    uint64_t mask = size_mask(stack_size(v, rs));
    if (sp > mask || bytes - 1 > mask - sp) {
        return STOP_UNSUPPORTED;
    }
    int stop = segment_check(v, rs, VCPU_SEG_SS, sp, bytes, write, linear);
    if (stop != STOP_NONE) {
        return stop;
    }
    int place;
    stop = reach(v, rs, *linear, bytes, write ? NEED_WRITE : NEED_READ, &place);
    if (stop != STOP_NONE) {
        return stop;
    }
    return place == MMIO_PLACE_RAM ? STOP_NONE : STOP_DEVICE;
}

// Pushes count slots, values[0] first, which ends up highest.
static int push(
    struct vcpu *v,
    struct run_state *rs,
    const uint64_t *values,
    size_t count,
    size_t slot
) {
    size_t address_size = stack_size(v, rs);
    uint64_t sp = rs->gpr[4] & size_mask(address_size);
    size_t bytes = count * slot;
    if (sp < bytes) {
        return STOP_UNSUPPORTED;
    }
    uint64_t top = sp - bytes;
    uintptr_t linear;
    int stop = stack_reach(v, rs, top, bytes, true, &linear);
    if (stop != STOP_NONE) {
        return stop;
    }
    for (size_t i = 0; i < count; i++) {
        write_linear(v, rs, linear + (count - 1 - i) * slot, slot, values[i]);
    }
    uint64_t mask = size_mask(address_size);
    rs->gpr[4] = address_size == 4 ? top : (rs->gpr[4] & ~mask) | top;
    return STOP_NONE;
}

// Reads count slots from the top of the stack, values[0] the lowest, and the
// stack pointer past them and extra bytes more, for the caller to set.
static int pop(
    struct vcpu *v,
    struct run_state *rs,
    uint64_t *values,
    size_t count,
    size_t slot,
    size_t extra,
    uint64_t *after
) {
    uint64_t mask = size_mask(stack_size(v, rs));
    uint64_t sp = rs->gpr[4] & mask;
    size_t bytes = count * slot;
    uintptr_t linear;
    int stop = stack_reach(v, rs, sp, bytes, false, &linear);
    if (stop != STOP_NONE) {
        return stop;
    }
    if (bytes + extra > mask - sp) {
        return STOP_UNSUPPORTED;
    }
    for (size_t i = 0; i < count; i++) {
        values[i] = read_linear(v, rs, linear + i * slot, slot);
    }
    *after = sp + bytes + extra;
    return STOP_NONE;
}

static void set_stack_pointer(struct vcpu *v, struct run_state *rs, uint64_t sp) {
    size_t address_size = stack_size(v, rs);
    uint64_t mask = size_mask(address_size);
    rs->gpr[4] = address_size == 4 ? sp & mask : (rs->gpr[4] & ~mask) | (sp & mask);
}

// CALL and JMP to a near target in a register or in RAM, as wide as the
// operand, all 64 bits of it in 64-bit mode.
static int indirect(struct vcpu *v, struct run_state *rs, const struct insn *insn, bool call) {
    if (insn->long_mode && insn->operand_size == 2) {
        return STOP_UNSUPPORTED;
    }
    size_t size = insn->long_mode ? 8 : insn->operand_size;
    struct operand o;
    int stop = rm_operand(v, rs, insn, size, NEED_READ, &o);
    if (stop != STOP_NONE) {
        return stop;
    }
    if (o.memory && o.place != MMIO_PLACE_RAM) {
        return STOP_DEVICE;
    }
    uint64_t target = load(v, rs, insn, &o);
    stop = near_target(v, rs, insn, &target);
    if (stop != STOP_NONE) {
        return stop;
    }
    if (call) {
        uint64_t back = rs->rip + insn->length;
        back = insn->long_mode ? back : back & size_mask(insn->operand_size);
        stop = push(v, rs, &back, 1, stack_slot(insn));
        if (stop != STOP_NONE) {
            return stop;
        }
    }
    rs->rip = target;
    return STOP_NONE;
}

// A far transfer's target in real mode, offset within CS's limit, which stays.
static int far_go_to(
    struct vcpu *v,
    struct run_state *rs,
    const struct insn *insn,
    uint16_t selector,
    uint64_t offset
) {
    offset &= size_mask(insn->operand_size);
    if (offset > segment(v, rs, VCPU_SEG_CS)->limit) {
        return STOP_FAULT;
    }
    load_segment(v, rs, VCPU_SEG_CS, selector);
    rs->rip = offset;
    return STOP_NONE;
}

// CALL and JMP far, to a pointer in the instruction or in RAM, and RET far, in
// real mode alone: the 16-bit CALL, as a 32-bit one pushes CS in a way that
// differs from processor to processor (SDM 325462-093 Vol. 2A, CALL).
static int far(struct vcpu *v, struct run_state *rs, const struct insn *insn) {
    unsigned int op = insn->opcode;
    size_t size = insn->operand_size;
    if (!rs->real) {
        return STOP_UNSUPPORTED;
    }
    bool call = op == 0x9a || (op == 0xff && ((insn->modrm >> 3) & 7) == 3);
    if (call && size != 2) {
        return STOP_UNSUPPORTED;
    }
    uint64_t offset;
    uint16_t selector;
    if (op == 0xca || op == 0xcb) {
        uint64_t values[2];
        uint64_t after;
        size_t extra = op == 0xca ? (uint16_t)insn->imm : 0;
        int stop = pop(v, rs, values, 2, size, extra, &after);
        if (stop != STOP_NONE) {
            return stop;
        }
        offset = values[0] & size_mask(size);
        if (offset > segment(v, rs, VCPU_SEG_CS)->limit) {
            return STOP_FAULT;
        }
        set_stack_pointer(v, rs, after);
        return far_go_to(v, rs, insn, (uint16_t)values[1], offset);
    }
    if (op == 0x9a || op == 0xea) {
        offset = insn->imm & size_mask(size);
        selector = (uint16_t)(insn->imm >> (size * 8));
    } else {
        // FF /3 and FF /5: the offset, then the selector.
        if (insn->modrm >> 6 == 3) {
            return STOP_UNSUPPORTED;
        }
        struct operand o;
        int stop = memory_operand(v, rs, insn, size, NEED_READ, &o);
        if (stop != STOP_NONE) {
            return stop;
        }
        struct operand s;
        stop = memory_operand_at(v, rs, insn, size, 2, NEED_READ, &s);
        if (stop != STOP_NONE) {
            return stop;
        }
        if (o.place != MMIO_PLACE_RAM || s.place != MMIO_PLACE_RAM) {
            return STOP_DEVICE;
        }
        offset = load(v, rs, insn, &o);
        selector = (uint16_t)load(v, rs, insn, &s);
    }
    if ((offset & size_mask(size)) > segment(v, rs, VCPU_SEG_CS)->limit) {
        return STOP_FAULT;
    }
    if (call) {
        uint64_t back[2] = { segment(v, rs, VCPU_SEG_CS)->selector, rs->rip + insn->length };
        int stop = push(v, rs, back, 2, size);
        if (stop != STOP_NONE) {
            return stop;
        }
    }
    return far_go_to(v, rs, insn, selector, offset);
}

// FLAGS as IRET or POPF loads them in real mode, or EFLAGS POPF loads at CPL 0
// in protected mode: a trap flag or an alignment check flag that changes ends
// the run after the instruction, which the processor then follows, and an
// interrupt flag that gets set lets interrupts in before the next.
static int load_flags(struct run_state *rs, uint64_t value, uint64_t loaded) {
    uint64_t before = rs->rflags;
    rs->rflags = (before & ~loaded) | (value & loaded) | RFLAGS_FIXED;
    if (((before ^ rs->rflags) & RFLAGS_IF) != 0 && (rs->rflags & RFLAGS_IF) != 0) {
        rs->recheck = true;
    }
    if ((rs->rflags & RFLAGS_TF) != 0 || ((before ^ rs->rflags) & RFLAGS_AC) != 0) {
        return STOP_FLAGS;
    }
    return STOP_NONE;
}

// INT n, INT3, and INTO in real mode, through the interrupt vector table.
static int interrupt(
    struct vcpu *v,
    struct run_state *rs,
    const struct insn *insn,
    uint8_t vector
) {
    if (!rs->real) {
        return STOP_UNSUPPORTED;
    }
    if ((uint32_t)vector * 4 + 3 > vcpu_seg_limit(v, VCPU_SEG_IDTR)) {
        return STOP_FAULT;
    }
    uintptr_t entry = (vcpu_seg_base(v, VCPU_SEG_IDTR) + (uint32_t)vector * 4) & 0xffffffff;
    int place;
    int stop = reach(v, rs, entry, 4, NEED_READ, &place);
    if (stop != STOP_NONE) {
        return stop;
    }
    if (place != MMIO_PLACE_RAM) {
        return STOP_DEVICE;
    }
    uint32_t pointer = (uint32_t)read_linear(v, rs, entry, 4);
    uint16_t offset = (uint16_t)pointer;
    if (offset > segment(v, rs, VCPU_SEG_CS)->limit) {
        return STOP_FAULT;
    }
    uint64_t frame[3] = {
        rs->rflags & 0xffff, segment(v, rs, VCPU_SEG_CS)->selector,
        (rs->rip + insn->length) & 0xffff
    };
    stop = push(v, rs, frame, 3, 2);
    if (stop != STOP_NONE) {
        return stop;
    }
    rs->rflags &= ~(uint64_t)(RFLAGS_IF | RFLAGS_TF | RFLAGS_AC);
    load_segment(v, rs, VCPU_SEG_CS, (uint16_t)(pointer >> 16));
    rs->rip = offset;
    return STOP_NONE;
}

// IRET of the 16-bit operand size in real mode, and in virtual-8086 mode at
// IOPL 3, where it leaves IOPL as it is (SDM 325462-093 Vol. 2A, IRET).
static int interrupt_return(struct vcpu *v, struct run_state *rs, const struct insn *insn) {
    unsigned int iopl = (rs->rflags >> 12) & 3;
    if (!(rs->real || (rs->v86 && iopl == 3)) || insn->operand_size != 2) {
        return STOP_UNSUPPORTED;
    }
    uint64_t frame[3];
    uint64_t after;
    int stop = pop(v, rs, frame, 3, 2, 0, &after);
    if (stop != STOP_NONE) {
        return stop;
    }
    if (frame[0] > segment(v, rs, VCPU_SEG_CS)->limit) {
        return STOP_FAULT;
    }
    set_stack_pointer(v, rs, after);
    load_segment(v, rs, VCPU_SEG_CS, (uint16_t)frame[1]);
    rs->rip = frame[0];
    return load_flags(rs, frame[2], rs->v86 ? FLAGS_LOADED & ~FLAGS_IOPL : FLAGS_LOADED);
}

// Whether CLI, STI, POPF's interrupt flag, and PUSHF are the instructions'
// own: in real mode, and where the CPL is within IOPL, which in virtual-8086
// mode it is at IOPL 3 alone; elsewhere VME and #GP(0) handle them.
static bool flags_own(struct vcpu *v, const struct run_state *rs) {
    if (rs->real) {
        return true;
    }
    return cpl(v, rs) <= ((rs->rflags >> 12) & 3);
}

// Whether size bytes of ports from port on are the program's, as the processor
// checks where the CPL is above IOPL and in virtual-8086 mode: in a 32-bit
// TSS's I/O permission bitmap, the two bytes that hold the first port's bit
// within the TSS's limit, and the ports' bits clear (SDM 325462-093 Vol. 1,
// 19.5.2).
static bool io_permitted(struct vcpu *v, struct run_state *rs, uint16_t port, size_t size) {
    if (rs->real || (!rs->v86 && cpl(v, rs) <= ((rs->rflags >> 12) & 3))) {
        return true;
    }
    uint32_t key = (uint32_t)size << 16 | port;
    for (size_t i = 0; i < rs->port_count; i++) {
        if (rs->ports[i] == key) {
            return true;
        }
    }
    struct vcpu_segment tr = {
        .attrib = vcpu_seg_attrib(v, VCPU_SEG_TR),
        .limit = vcpu_seg_limit(v, VCPU_SEG_TR),
        .base = vcpu_seg_base(v, VCPU_SEG_TR)
    };
    // An available or busy 32-bit TSS, types 9 and 11.
    if ((tr.attrib & 0xd) != 0x9 || (tr.attrib & SEG_PRESENT) == 0 || tr.limit < 0x67) {
        return false;
    }
    uint16_t base;
    uint32_t pf_error;
    uint64_t pf_linear;
    bool read = guest_mem_system_read(
        v, tr.base + 0x66, &base, sizeof(uint16_t), &pf_error, &pf_linear
    );
    if (!read) {
        return false;
    }
    uint32_t offset = (uint32_t)base + port / 8;
    if (offset + 1 > tr.limit) {
        return false;
    }
    uint16_t bits;
    read = guest_mem_system_read(
        v, tr.base + offset, &bits, sizeof(uint16_t), &pf_error, &pf_linear
    );
    if (!read) {
        return false;
    }
    if (((bits >> (port % 8)) & (((uint32_t)1 << size) - 1)) != 0) {
        return false;
    }
    if (rs->port_count < RUN_PORTS) {
        rs->ports[rs->port_count++] = key;
    }
    return true;
}

// IN and OUT, to the emulated VGA's ports, and the video BIOS's, which the
// hypervisor runs with the vCPU's registers as the run has them.
static int port(struct vcpu *v, struct run_state *rs, const struct insn *insn) {
    unsigned int op = insn->opcode;
    bool in = op == 0xe4 || op == 0xe5 || op == 0xec || op == 0xed;
    size_t size = (op & 1) == 0 ? 1 : insn->operand_size == 2 ? 2 : 4;
    uint16_t number = op < 0xe8 ? (uint16_t)insn->imm : (uint16_t)rs->gpr[2];
    bool bios = vgabios_port(number, size);
    if (!bios && !vga_port_emulated(number, size)) {
        return STOP_DEVICE;
    }
    if (!io_permitted(v, rs, number, size)) {
        return STOP_FAULT;
    }
    uint32_t value = (uint32_t)rs->gpr[0];
    if (bios) {
        store_state(v, rs, false);
    }
    io_dispatch(v, number, size, in, &value);
    if (bios) {
        // It may have changed what the guest's memory maps to.
        load_state(v, rs);
    }
    if (v->io_retry) {
        v->io_retry = false;
        return STOP_RETRY;
    }
    if (in) {
        set(rs, insn, 0, size, value);
    }
    rs->reached = true;
    return next(rs, insn);
}

// MOVS, CMPS, STOS, LODS, and SCAS, an iteration at a time, RUN_CHECK of them
// at most before the run looks at events again: meanwhile the instruction is
// part done, at RIP with RF set, as an event between iterations leaves it.
static int string(struct vcpu *v, struct run_state *rs, const struct insn *insn) {
    unsigned int op = insn->opcode;
    size_t size = (op & 1) != 0 ? insn->operand_size : 1;
    size_t address_size = insn->address_size;
    uint64_t mask = size_mask(address_size);
    int64_t delta = (rs->rflags & RFLAGS_DF) != 0 ? -(int64_t)size : (int64_t)size;
    bool from_source = op <= 0xa7 || op == 0xac || op == 0xad;
    bool at_destination = op != 0xac && op != 0xad;
    bool writes = op == 0xa4 || op == 0xa5 || op == 0xaa || op == 0xab;
    bool compares = op == 0xa6 || op == 0xa7 || op == 0xae || op == 0xaf;
    int source = insn->segment >= 0 ? insn->segment : VCPU_SEG_DS;
    uint64_t count = insn->rep ? rs->gpr[1] & mask : 1;
    for (size_t done = 0; count != 0; done++) {
        if (done == RUN_CHECK) {
            rs->partial = true;
            return STOP_NONE;
        }
        uintptr_t src = 0;
        uintptr_t dst = 0;
        int place;
        int stop = STOP_NONE;
        if (from_source) {
            stop = segment_check(v, rs, source, rs->gpr[6] & mask, size, false, &src);
            if (stop == STOP_NONE) {
                stop = reach(v, rs, src, size, NEED_READ, &place);
            }
        }
        if (stop == STOP_NONE && at_destination) {
            stop = segment_check(v, rs, VCPU_SEG_ES, rs->gpr[7] & mask, size, writes, &dst);
            if (stop == STOP_NONE) {
                stop = reach(v, rs, dst, size, writes ? NEED_WRITE : NEED_READ, &place);
            }
        }
        if (stop != STOP_NONE) {
            if (done == 0) {
                return stop;
            }
            rs->partial = true;
            return STOP_PARTIAL;
        }
        uint64_t a = 0;
        uint64_t b = 0;
        if (from_source) {
            a = read_linear(v, rs, src, size);
        }
        if (op == 0xa4 || op == 0xa5) {
            write_linear(v, rs, dst, size, a);
        } else if (op == 0xaa || op == 0xab) {
            write_linear(v, rs, dst, size, rs->gpr[0]);
        } else if (op == 0xac || op == 0xad) {
            set(rs, insn, 0, size, a);
        } else {
            b = read_linear(v, rs, dst, size);
            alu(ALU_CMP, size, compares && from_source ? a : rs->gpr[0], b, &rs->rflags);
        }
        if (from_source) {
            rs->gpr[6] = address_step(rs->gpr[6], delta, address_size);
        }
        if (at_destination) {
            rs->gpr[7] = address_step(rs->gpr[7], delta, address_size);
        }
        if (!insn->rep) {
            break;
        }
        rs->gpr[1] = address_step(rs->gpr[1], -1, address_size);
        count--;
        // REPE ends on a mismatch, REPNE on a match.
        if (compares && ((rs->rflags & RFLAGS_ZF) != 0) != (insn->rep_prefix == 0xf3)) {
            break;
        }
    }
    return next(rs, insn);
}

// The group 3 instructions: TEST, NOT, NEG, MUL, IMUL, DIV, and IDIV.
static int group3(struct vcpu *v, struct run_state *rs, const struct insn *insn, size_t size) {
    unsigned int group = (insn->modrm >> 3) & 7;
    bool writes = group == 2 || group == 3;
    struct operand o;
    int stop = rm_operand(v, rs, insn, size, NEED_READ | (writes ? NEED_WRITE : 0), &o);
    if (stop != STOP_NONE) {
        return stop;
    }
    if (group < 2) {
        return alu_operand(v, rs, insn, ALU_TEST, &o, sign_extend(insn->imm, insn->imm_size));
    }
    if (writes) {
        return alu_operand(v, rs, insn, group == 2 ? ALU_NOT : ALU_NEG, &o, 0);
    }
    return wide(rs, insn, group, size, load(v, rs, insn, &o));
}

// The segment register a PUSH or POP of one names, -1 for others.
static int segment_operand(unsigned int op) {
    switch (op) {
        case 0x06:
        case 0x07: {
            return VCPU_SEG_ES;
        }
        case 0x0e: {
            return VCPU_SEG_CS;
        }
        case 0x16:
        case 0x17: {
            return VCPU_SEG_SS;
        }
        case 0x1e:
        case 0x1f: {
            return VCPU_SEG_DS;
        }
        case INSN_OPCODE_0F | 0xa0:
        case INSN_OPCODE_0F | 0xa1: {
            return VCPU_SEG_FS;
        }
        case INSN_OPCODE_0F | 0xa8:
        case INSN_OPCODE_0F | 0xa9: {
            return VCPU_SEG_GS;
        }
        default: {
            return -1;
        }
    }
}

// Segment registers, which runs load in real and virtual-8086 mode alone, but
// for CS: PUSH, POP, MOV both ways, and LES, LDS, LFS, and LGS. A 32-bit PUSH
// stores either 16 or 32 bits, from processor to processor (SDM 325462-093
// Vol. 2B, PUSH).
static int segment_form(struct vcpu *v, struct run_state *rs, const struct insn *insn) {
    unsigned int op = insn->opcode;
    bool loads_real = rs->real || rs->v86;
    int reg = segment_operand(op);
    // Only FS's and GS's are in 64-bit mode.
    if (reg >= 0 && insn->long_mode && reg != VCPU_SEG_FS && reg != VCPU_SEG_GS) {
        return STOP_UNSUPPORTED;
    }
    if (reg >= 0) {
        bool pushes = op == 0x06 || op == 0x0e || op == 0x16 || op == 0x1e
                   || op == (INSN_OPCODE_0F | 0xa0) || op == (INSN_OPCODE_0F | 0xa8);
        size_t slot = stack_slot(insn);
        if (pushes) {
            if (slot != 2) {
                return STOP_UNSUPPORTED;
            }
            uint64_t selector = segment(v, rs, reg)->selector;
            int stop = push(v, rs, &selector, 1, slot);
            return stop != STOP_NONE ? stop : next(rs, insn);
        }
        if (!loads_real) {
            return STOP_UNSUPPORTED;
        }
        uint64_t selector;
        uint64_t after;
        int stop = pop(v, rs, &selector, 1, slot, 0, &after);
        if (stop != STOP_NONE) {
            return stop;
        }
        set_stack_pointer(v, rs, after);
        load_segment(v, rs, reg, (uint16_t)selector);
        if (reg == VCPU_SEG_SS) {
            rs->shadow = SHADOW_SS;
        }
        return next(rs, insn);
    }
    unsigned int group = (insn->modrm >> 3) & 7;
    if (op == 0x8c) {
        if (group > VCPU_SEG_GS) {
            return STOP_UNSUPPORTED;
        }
        // To a register, 32-bit and 64-bit ones are zero-extended, as
        // processors since the Pentium Pro have it; to memory, 16 bits.
        struct operand o;
        size_t size = insn->modrm >> 6 == 3 ? insn->operand_size : 2;
        int stop = rm_operand(v, rs, insn, size, NEED_WRITE, &o);
        if (stop != STOP_NONE) {
            return stop;
        }
        store(v, rs, insn, &o, segment(v, rs, (int)group)->selector);
        return next(rs, insn);
    }
    if (op == 0x8e) {
        if (!loads_real || group == VCPU_SEG_CS || group > VCPU_SEG_GS) {
            return STOP_UNSUPPORTED;
        }
        struct operand o;
        int stop = rm_operand(v, rs, insn, 2, NEED_READ, &o);
        if (stop != STOP_NONE) {
            return stop;
        }
        load_segment(v, rs, (int)group, (uint16_t)load(v, rs, insn, &o));
        if (group == VCPU_SEG_SS) {
            rs->shadow = SHADOW_SS;
        }
        return next(rs, insn);
    }
    // LES, LDS, LFS, and LGS: the offset, then the selector.
    int target;
    switch (op) {
        case 0xc4: {
            target = VCPU_SEG_ES;
            break;
        }
        case 0xc5: {
            target = VCPU_SEG_DS;
            break;
        }
        case INSN_OPCODE_0F | 0xb4: {
            target = VCPU_SEG_FS;
            break;
        }
        default: {
            target = VCPU_SEG_GS;
            break;
        }
    }
    if (!loads_real || insn->modrm >> 6 == 3) {
        return STOP_UNSUPPORTED;
    }
    size_t size = insn->operand_size;
    struct operand o;
    int stop = memory_operand(v, rs, insn, size, NEED_READ, &o);
    if (stop != STOP_NONE) {
        return stop;
    }
    struct operand s;
    stop = memory_operand_at(v, rs, insn, size, 2, NEED_READ, &s);
    if (stop != STOP_NONE) {
        return stop;
    }
    uint64_t offset = load(v, rs, insn, &o);
    load_segment(v, rs, target, (uint16_t)load(v, rs, insn, &s));
    set(rs, insn, reg_field(insn), size, offset);
    return next(rs, insn);
}

// Carries out the instruction, or changes nothing and says why not.
static int carry_out(struct vcpu *v, struct run_state *rs, const struct insn *insn) {
    unsigned int op = insn->opcode;
    bool string_op = (op >= 0xa4 && op <= 0xa7) || (op >= 0xaa && op <= 0xaf);
    // Mandatory prefixes make other instructions of most opcodes they come
    // before.
    if (insn->vex || insn->lock || (insn->rep && !string_op) || insn->rex_upper != 0
     || (insn->space & 0xf0) != INSN_SPACE_LEGACY) {
        return STOP_UNSUPPORTED;
    }
    if (string_op) {
        return string(v, rs, insn);
    }
    size_t operand_size = insn->operand_size;
    // The byte forms are the even opcodes.
    size_t size = (op & 1) != 0 ? operand_size : 1;
    unsigned int reg = reg_field(insn);
    unsigned int group = (insn->modrm >> 3) & 7;
    uint64_t imm = sign_extend(insn->imm, insn->imm_size);
    struct operand o;
    int stop;
    if (segment_operand(op) >= 0 || op == 0x8c || op == 0x8e
     || ((op == 0xc4 || op == 0xc5) && !insn->long_mode) || op == (INSN_OPCODE_0F | 0xb4)
     || op == (INSN_OPCODE_0F | 0xb5)) {
        return segment_form(v, rs, insn);
    }
    if (op < 0x40 && (op & 7) < 4) {
        bool to_reg = (op & 2) != 0;
        unsigned int alu_op = op >> 3;
        unsigned int need = NEED_READ | (to_reg || alu_op == ALU_CMP ? 0 : NEED_WRITE);
        stop = rm_operand(v, rs, insn, size, need, &o);
        if (stop != STOP_NONE) {
            return stop;
        }
        if (!to_reg) {
            return alu_operand(v, rs, insn, alu_op, &o, get(rs, insn, reg, size));
        }
        struct operand dst = { .reg = reg, .size = size };
        return alu_operand(v, rs, insn, alu_op, &dst, load(v, rs, insn, &o));
    }
    if (op < 0x40 && (op & 7) < 6) {
        struct operand dst = { .reg = 0, .size = size };
        return alu_operand(v, rs, insn, op >> 3, &dst, imm);
    }
    // INC and DEC with the register in the opcode, outside 64-bit mode.
    if (op >= 0x40 && op < 0x50) {
        struct operand dst = { .reg = op & 7, .size = operand_size };
        return alu_operand(v, rs, insn, op < 0x48 ? ALU_INC : ALU_DEC, &dst, 0);
    }
    if (op >= 0x50 && op <= 0x57) {
        uint64_t value = get(rs, insn, opcode_reg(insn), stack_slot(insn));
        stop = push(v, rs, &value, 1, stack_slot(insn));
        return stop != STOP_NONE ? stop : next(rs, insn);
    }
    if (op >= 0x58 && op <= 0x5f) {
        uint64_t value;
        uint64_t after;
        stop = pop(v, rs, &value, 1, stack_slot(insn), 0, &after);
        if (stop != STOP_NONE) {
            return stop;
        }
        // POP rSP takes the value it popped.
        set_stack_pointer(v, rs, after);
        set(rs, insn, opcode_reg(insn), stack_slot(insn), value);
        return next(rs, insn);
    }
    if ((op >= 0x70 && op < 0x80)
     || (op >= (INSN_OPCODE_0F | 0x80) && op <= (INSN_OPCODE_0F | 0x8f))) {
        return relative(v, rs, insn, emulate_condition(rs->rflags, op & 0xf));
    }
    if (op >= (INSN_OPCODE_0F | 0x90) && op <= (INSN_OPCODE_0F | 0x9f)) {
        stop = rm_operand(v, rs, insn, 1, NEED_WRITE, &o);
        if (stop != STOP_NONE) {
            return stop;
        }
        store(v, rs, insn, &o, emulate_condition(rs->rflags, op & 0xf) ? 1 : 0);
        return next(rs, insn);
    }
    if (op >= 0x91 && op <= 0x97) {
        unsigned int other = opcode_reg(insn);
        uint64_t a = get(rs, insn, 0, operand_size);
        set(rs, insn, 0, operand_size, get(rs, insn, other, operand_size));
        set(rs, insn, other, operand_size, a);
        return next(rs, insn);
    }
    if (op >= 0xb0 && op <= 0xb7) {
        set(rs, insn, opcode_reg(insn), 1, insn->imm);
        return next(rs, insn);
    }
    if (op >= 0xb8 && op <= 0xbf) {
        set(rs, insn, opcode_reg(insn), operand_size, insn->imm);
        return next(rs, insn);
    }
    switch (op) {
        case 0x60: {
            if (insn->long_mode) {
                return STOP_UNSUPPORTED;
            }
            uint64_t values[8];
            for (size_t r = 0; r < 8; r++) {
                values[r] = get(rs, insn, (unsigned int)r, operand_size);
            }
            stop = push(v, rs, values, 8, operand_size);
            return stop != STOP_NONE ? stop : next(rs, insn);
        }
        case 0x61: {
            if (insn->long_mode) {
                return STOP_UNSUPPORTED;
            }
            uint64_t values[8];
            uint64_t after;
            stop = pop(v, rs, values, 8, operand_size, 0, &after);
            if (stop != STOP_NONE) {
                return stop;
            }
            set_stack_pointer(v, rs, after);
            // The lowest slot is rDI's; rSP's is skipped.
            for (size_t r = 0; r < 8; r++) {
                if (r != 4) {
                    set(rs, insn, (unsigned int)r, operand_size, values[7 - r]);
                }
            }
            return next(rs, insn);
        }
        case 0x68:
        case 0x6a: {
            stop = push(v, rs, &imm, 1, stack_slot(insn));
            return stop != STOP_NONE ? stop : next(rs, insn);
        }
        case 0x69:
        case 0x6b:
        case INSN_OPCODE_0F | 0xaf: {
            stop = rm_operand(v, rs, insn, operand_size, NEED_READ, &o);
            if (stop != STOP_NONE) {
                return stop;
            }
            uint64_t value = load(v, rs, insn, &o);
            bool two = op == (INSN_OPCODE_0F | 0xaf);
            uint64_t a = two ? get(rs, insn, reg, operand_size) : value;
            uint64_t b = two ? value : imm;
            set(rs, insn, reg, operand_size, alu(ALU_IMUL, operand_size, a, b, &rs->rflags));
            return next(rs, insn);
        }
        case 0x80:
        case 0x81:
        case 0x82:
        case 0x83: {
            // 82h is undefined in 64-bit mode.
            if (op == 0x82 && insn->long_mode) {
                return STOP_UNSUPPORTED;
            }
            unsigned int need = NEED_READ | (group == ALU_CMP ? 0 : NEED_WRITE);
            stop = rm_operand(v, rs, insn, size, need, &o);
            if (stop != STOP_NONE) {
                return stop;
            }
            return alu_operand(v, rs, insn, group, &o, imm);
        }
        case 0x84:
        case 0x85: {
            stop = rm_operand(v, rs, insn, size, NEED_READ, &o);
            if (stop != STOP_NONE) {
                return stop;
            }
            return alu_operand(v, rs, insn, ALU_TEST, &o, get(rs, insn, reg, size));
        }
        case 0x86:
        case 0x87: {
            stop = rm_operand(v, rs, insn, size, NEED_READ | NEED_WRITE, &o);
            if (stop != STOP_NONE) {
                return stop;
            }
            // XCHG locks its memory operand, which other processors would see
            // unlocked.
            if (o.memory && o.place == MMIO_PLACE_RAM) {
                return STOP_UNSUPPORTED;
            }
            uint64_t a = load(v, rs, insn, &o);
            store(v, rs, insn, &o, get(rs, insn, reg, size));
            set(rs, insn, reg, size, a);
            return next(rs, insn);
        }
        case 0x88:
        case 0x89: {
            stop = rm_operand(v, rs, insn, size, NEED_WRITE, &o);
            if (stop != STOP_NONE) {
                return stop;
            }
            store(v, rs, insn, &o, get(rs, insn, reg, size));
            return next(rs, insn);
        }
        case 0x8a:
        case 0x8b: {
            stop = rm_operand(v, rs, insn, size, NEED_READ, &o);
            if (stop != STOP_NONE) {
                return stop;
            }
            set(rs, insn, reg, size, load(v, rs, insn, &o));
            return next(rs, insn);
        }
        case 0x8d: {
            int seg;
            uint64_t ea;
            if (!effective_address(rs, insn, &seg, &ea)) {
                return STOP_UNSUPPORTED;
            }
            set(rs, insn, reg, operand_size, ea);
            return next(rs, insn);
        }
        case 0x8f: {
            if (group != 0) {
                return STOP_UNSUPPORTED;
            }
            size_t slot = stack_slot(insn);
            uint64_t value;
            uint64_t after;
            stop = pop(v, rs, &value, 1, slot, 0, &after);
            if (stop != STOP_NONE) {
                return stop;
            }
            // A memory operand's address takes rSP as the pop leaves it (SDM
            // 325462-093 Vol. 2B, POP).
            uint64_t before = rs->gpr[4];
            set_stack_pointer(v, rs, after);
            stop = rm_operand(v, rs, insn, slot, NEED_WRITE, &o);
            if (stop != STOP_NONE) {
                rs->gpr[4] = before;
                return stop;
            }
            store(v, rs, insn, &o, value);
            return next(rs, insn);
        }
        case 0x90: {
            // With REX.B, 90h exchanges R8 and rAX.
            if ((insn->rex & INSN_REX_B) != 0) {
                uint64_t a = get(rs, insn, 0, operand_size);
                set(rs, insn, 0, operand_size, get(rs, insn, 8, operand_size));
                set(rs, insn, 8, operand_size, a);
            }
            return next(rs, insn);
        }
        case 0x98: {
            size_t from = operand_size / 2;
            set(rs, insn, 0, operand_size, sign_extend(get(rs, insn, 0, from), from));
            return next(rs, insn);
        }
        case 0x99: {
            uint64_t a = get(rs, insn, 0, operand_size);
            bool negative = ((a >> (operand_size * 8 - 1)) & 1) != 0;
            set(rs, insn, 2, operand_size, negative ? UINT64_MAX : 0);
            return next(rs, insn);
        }
        case 0x9a:
        case 0xca:
        case 0xcb:
        case 0xea: {
            return far(v, rs, insn);
        }
        case 0x9c: {
            if (!flags_own(v, rs)) {
                return STOP_UNSUPPORTED;
            }
            // The image has VM and RF clear.
            uint64_t image = rs->rflags & ~(uint64_t)(RFLAGS_VM | RFLAGS_RF);
            size_t slot = stack_slot(insn);
            stop = push(v, rs, &image, 1, slot);
            return stop != STOP_NONE ? stop : next(rs, insn);
        }
        case 0x9d: {
            // In real mode, in virtual-8086 mode at IOPL 3, which keeps IOPL,
            // and in protected mode at CPL 0, where POPF loads all it can.
            bool v86_own = rs->v86 && ((rs->rflags >> 12) & 3) == 3;
            if (!rs->real && !v86_own && (rs->v86 || cpl(v, rs) != 0)) {
                return STOP_UNSUPPORTED;
            }
            size_t slot = stack_slot(insn);
            if ((rs->real || rs->v86) && slot != 2) {
                return STOP_UNSUPPORTED;
            }
            uint64_t value;
            uint64_t after;
            stop = pop(v, rs, &value, 1, slot, 0, &after);
            if (stop != STOP_NONE) {
                return stop;
            }
            set_stack_pointer(v, rs, after);
            uint64_t loaded = slot == 2 ? FLAGS_LOADED : EFLAGS_LOADED;
            if (rs->v86) {
                loaded &= ~(uint64_t)FLAGS_IOPL;
            }
            stop = load_flags(rs, value, loaded);
            next(rs, insn);
            return stop;
        }
        case 0xa0:
        case 0xa1:
        case 0xa2:
        case 0xa3: {
            stop = memory_operand(v, rs, insn, size, op < 0xa2 ? NEED_READ : NEED_WRITE, &o);
            if (stop != STOP_NONE) {
                return stop;
            }
            if (op < 0xa2) {
                set(rs, insn, 0, size, load(v, rs, insn, &o));
            } else {
                store(v, rs, insn, &o, get(rs, insn, 0, size));
            }
            return next(rs, insn);
        }
        case 0xa8:
        case 0xa9: {
            struct operand dst = { .reg = 0, .size = size };
            return alu_operand(v, rs, insn, ALU_TEST, &dst, imm);
        }
        case 0xc0:
        case 0xc1:
        case 0xd0:
        case 0xd1:
        case 0xd2:
        case 0xd3: {
            stop = rm_operand(v, rs, insn, size, NEED_READ | NEED_WRITE, &o);
            if (stop != STOP_NONE) {
                return stop;
            }
            uint64_t count = op <= 0xc1 ? insn->imm : op <= 0xd1 ? 1 : rs->gpr[1] & 0xff;
            return alu_operand(v, rs, insn, ALU_ROL + group, &o, count);
        }
        case 0xc2:
        case 0xc3: {
            if (insn->long_mode && insn->operand_size == 2) {
                return STOP_UNSUPPORTED;
            }
            uint64_t target;
            uint64_t after;
            size_t extra = op == 0xc2 ? (uint16_t)insn->imm : 0;
            stop = pop(v, rs, &target, 1, stack_slot(insn), extra, &after);
            if (stop != STOP_NONE) {
                return stop;
            }
            stop = near_target(v, rs, insn, &target);
            if (stop != STOP_NONE) {
                return stop;
            }
            set_stack_pointer(v, rs, after);
            rs->rip = target;
            return STOP_NONE;
        }
        case 0xc6:
        case 0xc7: {
            if (group != 0) {
                return STOP_UNSUPPORTED;
            }
            stop = rm_operand(v, rs, insn, size, NEED_WRITE, &o);
            if (stop != STOP_NONE) {
                return stop;
            }
            store(v, rs, insn, &o, imm);
            return next(rs, insn);
        }
        case 0xc9: {
            size_t slot = stack_slot(insn);
            size_t address_size = stack_size(v, rs);
            uint64_t frame = get(rs, insn, 5, address_size);
            uintptr_t linear;
            stop = stack_reach(v, rs, frame, slot, false, &linear);
            if (stop != STOP_NONE) {
                return stop;
            }
            if (slot > size_mask(address_size) - frame) {
                return STOP_UNSUPPORTED;
            }
            uint64_t value = read_linear(v, rs, linear, slot);
            set_stack_pointer(v, rs, frame + slot);
            set(rs, insn, 5, slot, value);
            return next(rs, insn);
        }
        case 0xcc:
        case 0xcd:
        case 0xce: {
            if (op == 0xce && (insn->long_mode || (rs->rflags & RFLAGS_OF) == 0)) {
                return op == 0xce && !insn->long_mode ? next(rs, insn) : STOP_UNSUPPORTED;
            }
            uint8_t vector = op == 0xcc ? 3 : op == 0xce ? 4 : (uint8_t)insn->imm;
            return interrupt(v, rs, insn, vector);
        }
        case 0xcf: {
            return interrupt_return(v, rs, insn);
        }
        case 0xe0:
        case 0xe1:
        case 0xe2:
        case 0xe3: {
            return loop(v, rs, insn);
        }
        case 0xe4:
        case 0xe5:
        case 0xe6:
        case 0xe7:
        case 0xec:
        case 0xed:
        case 0xee:
        case 0xef: {
            return port(v, rs, insn);
        }
        case 0xe8: {
            if (insn->long_mode && insn->operand_size == 2) {
                return STOP_UNSUPPORTED;
            }
            uint64_t back = rs->rip + insn->length;
            uint64_t target = back + imm;
            stop = near_target(v, rs, insn, &target);
            if (stop != STOP_NONE) {
                return stop;
            }
            back = insn->long_mode ? back : back & size_mask(insn->operand_size);
            stop = push(v, rs, &back, 1, stack_slot(insn));
            if (stop != STOP_NONE) {
                return stop;
            }
            rs->rip = target;
            return STOP_NONE;
        }
        case 0xe9:
        case 0xeb: {
            return relative(v, rs, insn, true);
        }
        case 0xf5: {
            rs->rflags ^= RFLAGS_CF;
            return next(rs, insn);
        }
        case 0xf6:
        case 0xf7: {
            return group3(v, rs, insn, size);
        }
        case 0xf8: {
            rs->rflags &= ~(uint64_t)RFLAGS_CF;
            return next(rs, insn);
        }
        case 0xf9: {
            rs->rflags |= RFLAGS_CF;
            return next(rs, insn);
        }
        case 0xfa: {
            if (!flags_own(v, rs)) {
                return STOP_UNSUPPORTED;
            }
            rs->rflags &= ~(uint64_t)RFLAGS_IF;
            return next(rs, insn);
        }
        case 0xfb: {
            if (!flags_own(v, rs)) {
                return STOP_UNSUPPORTED;
            }
            // Interrupts stay off until the next instruction is done, where
            // STI sets IF (SDM 325462-093 Vol. 2B, STI).
            if ((rs->rflags & RFLAGS_IF) == 0) {
                rs->rflags |= RFLAGS_IF;
                next(rs, insn);
                rs->shadow = SHADOW_STI;
                return STOP_NONE;
            }
            return next(rs, insn);
        }
        case 0xfc: {
            rs->rflags &= ~(uint64_t)RFLAGS_DF;
            return next(rs, insn);
        }
        case 0xfd: {
            rs->rflags |= RFLAGS_DF;
            return next(rs, insn);
        }
        case 0xfe:
        case 0xff: {
            if (group < 2) {
                stop = rm_operand(v, rs, insn, size, NEED_READ | NEED_WRITE, &o);
                if (stop != STOP_NONE) {
                    return stop;
                }
                return alu_operand(v, rs, insn, group == 0 ? ALU_INC : ALU_DEC, &o, 0);
            }
            if (op == 0xff && (group == 2 || group == 4)) {
                return indirect(v, rs, insn, group == 2);
            }
            if (op == 0xff && (group == 3 || group == 5)) {
                return far(v, rs, insn);
            }
            if (op == 0xff && group == 6) {
                stop = rm_operand(v, rs, insn, stack_slot(insn), NEED_READ, &o);
                if (stop != STOP_NONE) {
                    return stop;
                }
                if (o.memory && o.place != MMIO_PLACE_RAM) {
                    return STOP_DEVICE;
                }
                uint64_t value = load(v, rs, insn, &o);
                stop = push(v, rs, &value, 1, stack_slot(insn));
                return stop != STOP_NONE ? stop : next(rs, insn);
            }
            return STOP_UNSUPPORTED;
        }
        case INSN_OPCODE_0F | 0x1f: {
            return group == 0 ? next(rs, insn) : STOP_UNSUPPORTED;
        }
        case INSN_OPCODE_0F | 0xa3:
        case INSN_OPCODE_0F | 0xab:
        case INSN_OPCODE_0F | 0xb3:
        case INSN_OPCODE_0F | 0xbb: {
            // With a memory operand, the bit string reaches beyond it.
            if (insn->modrm >> 6 != 3) {
                return STOP_OPERAND;
            }
            struct operand dst = { .reg = rm_field(insn), .size = operand_size };
            uint64_t offset = get(rs, insn, reg, operand_size);
            return alu_operand(v, rs, insn, ALU_BT + ((op >> 3) & 3), &dst, offset);
        }
        case INSN_OPCODE_0F | 0xba: {
            if (group < 4) {
                return STOP_UNSUPPORTED;
            }
            unsigned int need = NEED_READ | (group == 4 ? 0 : NEED_WRITE);
            stop = rm_operand(v, rs, insn, operand_size, need, &o);
            if (stop != STOP_NONE) {
                return stop;
            }
            uint64_t offset = insn->imm & (operand_size * 8 - 1);
            return alu_operand(v, rs, insn, ALU_BT + group - 4, &o, offset);
        }
        case INSN_OPCODE_0F | 0xa4:
        case INSN_OPCODE_0F | 0xa5:
        case INSN_OPCODE_0F | 0xac:
        case INSN_OPCODE_0F | 0xad: {
            stop = rm_operand(v, rs, insn, operand_size, NEED_READ | NEED_WRITE, &o);
            if (stop != STOP_NONE) {
                return stop;
            }
            uint8_t count = (op & 1) == 0 ? (uint8_t)insn->imm : (uint8_t)rs->gpr[1];
            uint64_t result = alu_double(
                (op & 8) != 0 ? ALU_SHRD : ALU_SHLD, operand_size, load(v, rs, insn, &o),
                get(rs, insn, reg, operand_size), count, &rs->rflags
            );
            store(v, rs, insn, &o, result);
            return next(rs, insn);
        }
        case INSN_OPCODE_0F | 0xb6:
        case INSN_OPCODE_0F | 0xb7:
        case INSN_OPCODE_0F | 0xbe:
        case INSN_OPCODE_0F | 0xbf: {
            size_t from = (op & 1) != 0 ? 2 : 1;
            stop = rm_operand(v, rs, insn, from, NEED_READ, &o);
            if (stop != STOP_NONE) {
                return stop;
            }
            uint64_t value = load(v, rs, insn, &o);
            if ((op & 8) != 0) {
                value = sign_extend(value, from);
            }
            set(rs, insn, reg, operand_size, value);
            return next(rs, insn);
        }
        default: {
            return STOP_UNSUPPORTED;
        }
    }
}

// The instruction at CS:RIP, where memory holds it, all of it within CS's
// limit or canonical, and fetching it faults nowhere: decoded again only where
// its bytes changed.
static int fetch(struct vcpu *v, struct run_state *rs, const struct insn **out) {
    uintptr_t rip = rs->rip;
    const struct run_segment *cs = segment(v, rs, VCPU_SEG_CS);
    uintptr_t linear = rs->long_mode ? rip : (cs->base + rip) & 0xffffffff;
    uintptr_t page = linear & ~(uintptr_t)(PAGE_SIZE - 1);
    bool fault = false;
    if (page != rs->code_linear) {
        struct run_page *p = page_get(v, rs, linear, NEED_FETCH, &fault);
        if (p == NULL) {
            return STOP_CODE;
        }
        rs->code_linear = page;
        rs->code_host = p->host;
    }
    size_t offset = linear & (PAGE_SIZE - 1);
    struct run_decoded *d = &rs->decoded[(linear ^ (linear >> 12)) % RUN_DECODED];
    const uint8_t *at = rs->code_host + offset;
    size_t available = PAGE_SIZE - offset;
    bool known = d->linear == linear && d->mode == rs->mode && d->insn.length != 0
              && d->insn.length <= available && memcmp(d->bytes, at, d->insn.length) == 0;
    if (!known) {
        // Zeroes past what can be fetched, as the decoder takes them.
        uint8_t bytes[INSN_MAX_LENGTH + 4] = { 0 };
        if (available >= INSN_MAX_LENGTH) {
            memcpy(bytes, at, INSN_MAX_LENGTH);
        } else {
            memcpy(bytes, at, available);
            uintptr_t following = linear + available;
            if (!rs->long_mode) {
                following &= 0xffffffff;
            }
            struct run_page *q = page_get(v, rs, following, NEED_FETCH, &fault);
            if (q != NULL) {
                memcpy(bytes + available, q->host, INSN_MAX_LENGTH - available);
                available = INSN_MAX_LENGTH;
            }
        }
        d->insn.length = 0;
        if (!insn_decode_bytes(v, bytes, &d->insn) || d->insn.length > available) {
            d->insn.length = 0;
            return STOP_CODE;
        }
        d->linear = linear;
        d->mode = rs->mode;
        memcpy(d->bytes, bytes, d->insn.length);
        atomic_fetch_add_explicit(&decode_count, 1, memory_order_relaxed);
    }
    uint64_t last = rip + d->insn.length - 1;
    bool fits = rs->long_mode ? canonical(v, rip) && canonical(v, last) : last <= cs->limit;
    if (!fits) {
        return STOP_CODE;
    }
    *out = &d->insn;
    return STOP_NONE;
}

// What the guest is to take before its next instruction, and the hypervisor's
// work that cannot wait: the vCPU's RFLAGS hold the run's IF meanwhile.
static bool pending(struct vcpu *v, struct run_state *rs) {
    vcpu_set_rflags(v, rs->rflags);
    return event_halt_ends(v) || vcpu_pause_pending();
}

// Not where the guest steps or breaks on instructions or accesses, which the
// processor then reports itself, nor in the guest's guest, nor where it checks
// alignment.
static bool may_run(struct vcpu *v) {
    uint64_t rflags = vcpu_rflags(v);
    bool alignment = (vcpu_cr0(v) & CR0_AM) != 0 && (rflags & RFLAGS_AC) != 0
                  && vcpu_cpl(v) == 3;
    // TF with DEBUGCTL.BTF steps the branches alone, which runs take too.
    return !nvmx_in_l2(v) && !nsvm_in_l2(v) && !v->stepping && !vcpu_stepped(v)
        && (rflags & RFLAGS_TF) == 0 && !vcpu_breakpoints_armed(v) && v->db_pending == 0
        && !alignment;
}

// What the run starts from: the general registers, RIP, RFLAGS, and the mode,
// with the segments read as instructions use them.
static void load_state(struct vcpu *v, struct run_state *rs) {
    for (size_t i = 0; i < 16; i++) {
        rs->gpr[i] = i == 4 ? vcpu_rsp(v) : ((const uint64_t *)&v->gprs)[i];
    }
    rs->rip = vcpu_rip(v);
    rs->rflags = vcpu_rflags(v);
    rs->segs_loaded = 0;
    rs->segs_changed = 0;
    uint32_t cs = vcpu_seg_attrib(v, VCPU_SEG_CS);
    rs->lma = (vcpu_efer(v) & EFER_LMA) != 0;
    rs->long_mode = rs->lma && (cs & VCPU_SEG_L) != 0;
    rs->real = (vcpu_cr0(v) & CR0_PE) == 0;
    rs->v86 = (rs->rflags & RFLAGS_VM) != 0;
    rs->mode = cs | (rs->v86 ? (uint32_t)1 << 31 : 0) | (rs->long_mode ? (uint32_t)1 << 30 : 0);
    rs->page_count = 0;
    rs->victim = 0;
    rs->code_linear = 1;
    rs->port_count = 0;
}

// The registers back, as the run left them: RF set where the instruction at
// RIP is part done, and clear where the last one is done (SDM 325462-093 Vol.
// 3B, 20.3.1.1); as the run ends, the interrupt shadow of an STI that the next
// instruction is to come under.
static void store_state(struct vcpu *v, struct run_state *rs, bool last) {
    for (size_t i = 0; i < 16; i++) {
        if (i != 4) {
            ((uint64_t *)&v->gprs)[i] = rs->gpr[i];
        }
    }
    vcpu_set_rsp(v, rs->gpr[4]);
    uint64_t rflags = rs->rflags & ~(uint64_t)RFLAGS_RF;
    vcpu_set_rflags(v, rs->partial ? rflags | RFLAGS_RF : rflags);
    vcpu_set_rip(v, rs->rip);
    for (size_t reg = 0; reg <= VCPU_SEG_GS; reg++) {
        struct run_segment *r = &rs->segs[reg];
        bool differs = r->selector != r->loaded_selector || r->base != r->loaded_base;
        if ((rs->segs_changed & ((uint32_t)1 << reg)) != 0 && differs) {
            vcpu_load_real_segment(v, reg, r->selector);
            r->loaded_selector = r->selector;
            r->loaded_base = r->base;
        }
    }
    rs->segs_changed = 0;
    if (last && rs->shadow == SHADOW_STI) {
        vcpu_interrupt_shadow(v);
    } else if (last && rs->shadow == SHADOW_SS) {
        vcpu_ss_shadow(v);
    }
}

static unsigned int opcode_slot(unsigned int op) {
    if (op < 0x100) {
        return op;
    }
    return (op & 0xf00) == INSN_OPCODE_0F ? 0x100 | (op & 0xff) : 0x200;
}

void run_begin(struct vcpu *v) {
    if (!may_run(v)) {
        return;
    }
    if (v->run == NULL) {
        v->run = kmalloc(sizeof(struct run_state));
    }
    struct run_state *rs = v->run;
    uint64_t start = rdtsc();
    load_state(v, rs);
    rs->shadow = SHADOW_NONE;
    rs->recheck = false;
    // The hypervisor's timers are the boot processor's to run.
    uint64_t deadline = v->cpu->bsp ? hv_timer_next() : 0;
    unsigned int idle_limit = cpu_exits_slow() ? RUN_IDLE_SLOW_EXITS : RUN_IDLE;
    unsigned int idle = 0;
    uint64_t count = 0;
    uint64_t accesses = 0;
    unsigned int last_op = 0;
    bool changed = false;
    int stop;
    // What SVM fetched for the exit is the first instruction's.
    v->fetched_count = 0;
    for (;;) {
        if (idle == idle_limit) {
            stop = STOP_IDLE;
            break;
        }
        // Nothing comes between STI and the instruction after it.
        if (rs->shadow == SHADOW_NONE && (count % RUN_CHECK == 0 || rs->recheck)) {
            rs->recheck = false;
            if (cpu_slice_over() || (deadline != 0 && rdtsc() >= deadline)) {
                stop = STOP_TIME;
                break;
            }
            if (pending(v, rs)) {
                stop = STOP_EVENT;
                break;
            }
        }
        const struct insn *insn;
        stop = fetch(v, rs, &insn);
        if (stop != STOP_NONE) {
            break;
        }
        rs->reached = false;
        rs->partial = false;
        int shadowed = rs->shadow;
        stop = carry_out(v, rs, insn);
        if (stop == STOP_PARTIAL || stop == STOP_FLAGS) {
            changed = true;
            last_op = insn->opcode;
            break;
        }
        if (stop != STOP_NONE) {
            last_op = insn->opcode;
            break;
        }
        changed = true;
        count++;
        // The instruction a shadow covered is done: interrupts may come now.
        if (shadowed != SHADOW_NONE && rs->shadow == shadowed && !rs->partial) {
            rs->shadow = SHADOW_NONE;
            rs->recheck = true;
        }
        if (rs->reached) {
            accesses++;
            idle = 0;
        } else {
            idle++;
        }
    }
    if (changed) {
        store_state(v, rs, true);
    }
    if (!profile_on) {
        return;
    }
    atomic_fetch_add_explicit(&run_ticks, rdtsc() - start, memory_order_relaxed);
    atomic_fetch_add_explicit(&run_count, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&insn_count, count, memory_order_relaxed);
    atomic_fetch_add_explicit(&access_count, accesses, memory_order_relaxed);
    atomic_fetch_add_explicit(&stop_counts[stop], 1, memory_order_relaxed);
    if (stop == STOP_UNSUPPORTED || stop == STOP_OPERAND || stop == STOP_DEVICE
     || stop == STOP_FAULT) {
        atomic_fetch_add_explicit(&opcode_counts[opcode_slot(last_op)], 1, memory_order_relaxed);
    }
}

void run_report(uint64_t ms) {
    uint64_t runs = atomic_exchange_explicit(&run_count, 0, memory_order_relaxed);
    uint64_t insns = atomic_exchange_explicit(&insn_count, 0, memory_order_relaxed);
    uint64_t accesses = atomic_exchange_explicit(&access_count, 0, memory_order_relaxed);
    uint64_t decodes = atomic_exchange_explicit(&decode_count, 0, memory_order_relaxed);
    uint64_t ticks = atomic_exchange_explicit(&run_ticks, 0, memory_order_relaxed);
    if (ms == 0) {
        ms = 1;
    }
    if (runs == 0) {
        return;
    }
    uint64_t mhz = time_tsc_hz() / 1000000;
    printf(
        "Profile:   runs %llu/s, %llu instructions/s, %llu accesses/s, %llu decodes/s, "
        "%llu ns each run\n", (unsigned long long)(runs * 1000 / ms),
        (unsigned long long)(insns * 1000 / ms), (unsigned long long)(accesses * 1000 / ms),
        (unsigned long long)(decodes * 1000 / ms),
        (unsigned long long)(ticks * 1000 / (mhz != 0 ? mhz : 1) / runs)
    );
    for (size_t i = 1; i < STOP_COUNT; i++) {
        uint64_t n = atomic_exchange_explicit(&stop_counts[i], 0, memory_order_relaxed);
        if (n != 0) {
            printf(
                "Profile:     run stop %-12s %8llu/s\n", stop_names[i],
                (unsigned long long)(n * 1000 / ms)
            );
        }
    }
    for (size_t n = 0; n < TOP_OPCODES; n++) {
        size_t best = 0;
        uint64_t most = 0;
        for (size_t i = 0; i < OPCODE_SLOTS; i++) {
            uint64_t c = atomic_load_explicit(&opcode_counts[i], memory_order_relaxed);
            if (c > most) {
                most = c;
                best = i;
            }
        }
        if (most == 0) {
            break;
        }
        atomic_store_explicit(&opcode_counts[best], 0, memory_order_relaxed);
        printf(
            "Profile:     run stop opcode %s%02zx %8llu/s\n", best >= 0x100 ? "0f " : "",
            best & 0xff, (unsigned long long)(most * 1000 / ms)
        );
    }
    for (size_t i = 0; i < OPCODE_SLOTS; i++) {
        atomic_store_explicit(&opcode_counts[i], 0, memory_order_relaxed);
    }
}
