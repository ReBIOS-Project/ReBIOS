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
#include <stdnoreturn.h>
#include <virt/mmio.h>
#include <lib/heap.h>
#include <lib/log.h>
#include <lib/spinlock.h>
#include <lib/util.h>
#include <memory.h>
#include <sys/mm.h>
#include <sys/x86.h>
#include <virt/emulate.h>
#include <virt/gpt.h>
#include <virt/guest_mem.h>
#include <virt/insn.h>
#include <virt/io.h>
#include <virt/nsvm.h>
#include <virt/nvmx.h>
#include <virt/profile.h>
#include <virt/vcpu.h>
#include <virt/virt.h>

#define MAX_REGIONS 256

// The accesses to where nothing decodes that are logged.
#define ABSENT_REPORTS 16

// The most bytes of a repeated string instruction's operands mmio_string()
// takes at a time, which a buffer on the stack holds.
#define STRING_RUN_BYTES 1024

struct region {
    _Atomic(uintptr_t) base;
    _Atomic(size_t) size;
    const struct mmio_ops *ops;
    void *ctx;
};

static struct region regions[MAX_REGIONS];
static _Atomic(size_t) region_count;
static struct spinlock region_lock;

int mmio_register(uintptr_t base, size_t size, const struct mmio_ops *ops, void *ctx) {
    spinlock_acquire(&region_lock);
    int region = -1;
    size_t count = atomic_load_explicit(&region_count, memory_order_relaxed);
    if (count == MAX_REGIONS) {
        printf("Too many emulated memory regions for %#llx\n", (unsigned long long)base);
    } else {
        region = (int)count;
        regions[count] = (struct region){ base, size, ops, ctx };
        atomic_store_explicit(&region_count, count + 1, memory_order_release);
    }
    spinlock_release(&region_lock);
    return region;
}

// A lookup meanwhile sees the region at one place or the other, or nowhere.
void mmio_move(int region, uintptr_t base, size_t size) {
    struct region *r = &regions[region];
    atomic_store_explicit(&r->size, 0, memory_order_release);
    atomic_store_explicit(&r->base, base, memory_order_release);
    atomic_store_explicit(&r->size, size, memory_order_release);
}

uint64_t mmio_read_host(uintptr_t hpa, size_t size) {
    switch (size) {
        case 1: {
            return *(volatile uint8_t *)(uintptr_t)hpa;
        }
        case 2: {
            return *(volatile uint16_t *)(uintptr_t)hpa;
        }
        case 4: {
            return *(volatile uint32_t *)(uintptr_t)hpa;
        }
        default: {
            return *(volatile uint64_t *)(uintptr_t)hpa;
        }
    }
}

void mmio_write_host(uintptr_t hpa, size_t size, uint64_t value) {
    switch (size) {
        case 1: {
            *(volatile uint8_t *)(uintptr_t)hpa = (uint8_t)value;
            break;
        }
        case 2: {
            *(volatile uint16_t *)(uintptr_t)hpa = (uint16_t)value;
            break;
        }
        case 4: {
            *(volatile uint32_t *)(uintptr_t)hpa = (uint32_t)value;
            break;
        }
        default: {
            *(volatile uint64_t *)(uintptr_t)hpa = value;
            break;
        }
    }
}

static uint64_t absent_read(struct vcpu *v, void *ctx, uint64_t offset, size_t size) {
    (void)v;
    (void)ctx;
    (void)offset;
    return size == 8 ? UINT64_MAX : ((uint64_t)1 << (size * 8)) - 1;
}

void mmio_absent_write(struct vcpu *v, void *ctx, uint64_t offset, size_t size, uint64_t value) {
    (void)v;
    (void)ctx;
    (void)offset;
    (void)size;
    (void)value;
}

const struct mmio_ops mmio_absent_ops = {
    .read = absent_read,
    .write = mmio_absent_write,
    .reads_virtual = true,
    .writes_virtual = true,
    .writes_ignored = true
};

// What the guest reaches where it has neither memory nor a device, as a PC
// does where nothing decodes: such as the addresses an OS moves a BAR to.
static const struct region absent = { 0, UINT64_MAX, &mmio_absent_ops, NULL };

static const struct region *find(uintptr_t gpa) {
    size_t count = atomic_load_explicit(&region_count, memory_order_acquire);
    for (size_t i = 0; i < count; i++) {
        size_t size = atomic_load_explicit(&regions[i].size, memory_order_acquire);
        if (gpa - atomic_load_explicit(&regions[i].base, memory_order_acquire) < size) {
            return &regions[i];
        }
    }
    uintptr_t hpa;
    uint32_t prot;
    if (!gpt_translate(page_down(gpa), &hpa, &prot, NULL)) {
        return &absent;
    }
    return NULL;
}

// In naturally aligned pieces of up to 8 bytes.
static void region_access(
    struct vcpu *v,
    const struct region *r,
    uintptr_t gpa,
    uint8_t *buf,
    size_t len,
    bool write
) {
    while (len != 0) {
        uint64_t offset = gpa - atomic_load_explicit(&r->base, memory_order_relaxed);
        size_t size = 8;
        while (size > len || (offset & (size - 1)) != 0) {
            size >>= 1;
        }
        uint64_t value = 0;
        if (write) {
            memcpy(&value, buf, size);
            r->ops->write(v, r->ctx, offset, size, value);
        } else {
            value = r->ops->read(v, r->ctx, offset, size);
            memcpy(buf, &value, size);
        }
        gpa += size;
        buf += size;
        len -= size;
    }
}

// Translates every page an access touches before any part happens, so that a
// fault leaves none of it done.
static bool translate(
    struct vcpu *v,
    uintptr_t linear,
    size_t size,
    bool write,
    uint64_t *gpas,
    unsigned int *lens,
    unsigned int *pieces
) {
    *pieces = 0;
    while (size != 0) {
        uintptr_t gpa;
        uint32_t pf_error;
        if (!guest_mem_linear_to_phys(v, linear, write, false, &gpa, &pf_error)) {
            if ((pf_error & GUEST_MEM_PF_ERROR_EXIT) == 0) {
                vcpu_inject_page_fault(v, linear, pf_error);
            }
            return false;
        }
        unsigned int chunk = PAGE_SIZE - (linear & (PAGE_SIZE - 1));
        if (chunk > size) {
            chunk = size;
        }
        gpas[*pieces] = gpa;
        lens[*pieces] = chunk;
        (*pieces)++;
        linear += chunk;
        size -= chunk;
    }
    return true;
}

void mmio_phys_access(struct vcpu *v, uintptr_t gpa, void *buf, size_t len, bool write) {
    const struct region *r = find(gpa);
    if (r != NULL) {
        region_access(v, r, gpa, buf, len, write);
    } else if (write) {
        // What is neither RAM nor emulated decodes nothing.
        guest_mem_phys_write(gpa, buf, len);
    } else if (!guest_mem_phys_read(gpa, buf, len)) {
        memset(buf, 0xff, len);
    }
}

static bool access(struct vcpu *v, uintptr_t linear, void *buf, size_t size, bool write) {
    uint64_t gpas[2];
    unsigned int lens[2];
    unsigned int pieces;
    if (!translate(v, linear, size, write, gpas, lens, &pieces)) {
        return false;
    }
    v->mmio_retry = false;
    uint8_t *p = buf;
    for (size_t i = 0; i < pieces; i++) {
        mmio_phys_access(v, gpas[i], p, lens[i], write);
        p += lens[i];
    }
    if (v->mmio_retry) {
        v->mmio_retry = false;
        return false;
    }
    vcpu_data_access(v, linear, size, write);
    return true;
}

bool mmio_read_linear(struct vcpu *v, uintptr_t linear, void *buf, size_t size) {
    return access(v, linear, buf, size, false);
}

bool mmio_write_linear(struct vcpu *v, uintptr_t linear, const void *buf, size_t size) {
    return access(v, linear, (void *)buf, size, true);
}

static bool mem_ins(
    struct vcpu *v,
    uintptr_t linear,
    uint16_t port,
    size_t size,
    uint32_t *value
) {
    uint64_t gpas[2];
    unsigned int lens[2];
    unsigned int pieces;
    if (!translate(v, linear, size, true, gpas, lens, &pieces)) {
        return false;
    }
    const struct region *r = find(gpas[0]);
    if (r == NULL || !r->ops->writes_ignored) {
        return false;
    }
    *value = 0xffffffff;
    vcpu_io_access(v, port, size);
    return true;
}

static void mem_outs(struct vcpu *v, uint16_t port, size_t size, uint32_t value) {
    vcpu_io_access(v, port, size);
    io_dispatch(v, port, size, false, &value);
}

// A run of a repeated string instruction's operands at rDI, or rSI, within a
// page and within STRING_RUN_BYTES: the guest-physical address of the first,
// *count cut to the run. False where the first crosses a page, or where any
// would fault, which the instruction then meets an iteration at a time.
static bool string_run(
    struct vcpu *v,
    const struct insn *insn,
    bool destination,
    size_t size,
    int64_t step,
    bool write,
    size_t *count,
    uintptr_t *gpa
) {
    uint64_t linear;
    if (!insn_string_run(v, insn, destination, size, step, 1, write, &linear)) {
        return false;
    }
    size_t in_page = linear & (PAGE_SIZE - 1);
    if (in_page + size > PAGE_SIZE) {
        return false;
    }
    size_t room = step > 0 ? (PAGE_SIZE - in_page) / size : in_page / size + 1;
    if (room > STRING_RUN_BYTES / size) {
        room = STRING_RUN_BYTES / size;
    }
    if (*count > room) {
        *count = room;
    }
    uint32_t pf_error;
    return insn_string_run(v, insn, destination, size, step, *count, write, &linear)
        && guest_mem_linear_to_phys(v, linear, write, false, gpa, &pf_error);
}

// Where such a run of count operands is: RAM, *r NULL, or a region, *offset
// the first's offset into it. False where it is neither, or more than one.
static bool string_place(
    uintptr_t gpa,
    size_t size,
    int64_t step,
    size_t count,
    const struct region **r,
    uint64_t *offset
) {
    *r = find(gpa);
    if (*r == NULL) {
        return true;
    }
    uint64_t base = atomic_load_explicit(&(*r)->base, memory_order_relaxed);
    uint64_t length = atomic_load_explicit(&(*r)->size, memory_order_relaxed);
    *offset = gpa - base;
    uint64_t low = step > 0 ? *offset : *offset - (uint64_t)(count - 1) * size;
    return low < length && length - low >= (uint64_t)count * size;
}

// Puts a run's elements, which lie in memory from its lowest address on, in
// the order the instruction reaches them, or back.
static void string_order(uint8_t *buf, size_t size, int64_t step, size_t count) {
    if (step > 0) {
        return;
    }
    for (size_t i = 0, j = count - 1; i < j; i++, j--) {
        uint8_t t[8];
        memcpy(t, buf + i * size, size);
        memcpy(buf + i * size, buf + j * size, size);
        memcpy(buf + j * size, t, size);
    }
}

size_t mmio_string(
    struct vcpu *v,
    const struct insn *insn,
    size_t size,
    int64_t step,
    size_t count
) {
    // Breakpoints are checked an access at a time.
    if (vcpu_breakpoints_armed(v)) {
        return 0;
    }
    unsigned int op = insn->opcode;
    bool moves = op == 0xa4 || op == 0xa5;
    bool stores = op == 0xaa || op == 0xab;
    bool outputs = op == 0x6e || op == 0x6f;
    // The source first, as the processor reaches it first.
    uintptr_t src = 0;
    uintptr_t dst = 0;
    if ((moves || outputs) && !string_run(v, insn, false, size, step, false, &count, &src)) {
        return 0;
    }
    if ((moves || stores) && !string_run(v, insn, true, size, step, true, &count, &dst)) {
        return 0;
    }
    const struct region *from = NULL;
    const struct region *to = NULL;
    uint64_t source = 0;
    uint64_t destination = 0;
    if ((moves || outputs) && !string_place(src, size, step, count, &from, &source)) {
        return 0;
    }
    if ((moves || stores) && !string_place(dst, size, step, count, &to, &destination)) {
        return 0;
    }
    uintptr_t src_low = step > 0 ? src : src - (count - 1) * size;
    uintptr_t dst_low = step > 0 ? dst : dst - (count - 1) * size;
    uint8_t buf[STRING_RUN_BYTES];
    if (stores && to != NULL && to->ops->write_string != NULL) {
        uint8_t value[8];
        write_le64(value, v->gprs.rax);
        to->ops->write_string(v, to->ctx, destination, size, step, value, true, count);
    } else if (outputs && from == NULL && guest_mem_phys_read(src_low, buf, count * size)) {
        string_order(buf, size, step, count);
        uint16_t port = (uint16_t)v->gprs.rdx;
        for (size_t n = 0; n < count; n++) {
            uint32_t value = 0;
            memcpy(&value, buf + n * size, size);
            io_dispatch(v, port, size, false, &value);
        }
    } else if (moves && from != NULL && from == to && from->ops->move_string != NULL) {
        from->ops->move_string(v, from->ctx, source, destination, size, step, count);
    } else if (moves && from == NULL && to != NULL && to->ops->write_string != NULL
            && guest_mem_phys_read(src_low, buf, count * size)) {
        string_order(buf, size, step, count);
        to->ops->write_string(v, to->ctx, destination, size, step, buf, false, count);
    } else if (moves && from != NULL && to == NULL && from->ops->read_string != NULL) {
        from->ops->read_string(v, from->ctx, source, size, step, buf, count);
        string_order(buf, size, step, count);
        // What is neither RAM nor emulated decodes nothing.
        guest_mem_phys_write(dst_low, buf, count * size);
    } else {
        return 0;
    }
    return count;
}

static const struct emulate_mem guest_mem = {
    mmio_read_linear, mmio_write_linear, mem_ins, mem_outs, mmio_string
};

// The pages a single-stepped instruction runs against: those its operands
// reach in emulated memory, its ModRM operand's and its stack's, where the
// bytes that SDM 325462-093, Intel APX 355828-009, and APM 40332 rev. 4.11 say
// it reads are what their regions read as, and those they say it writes come
// back as writes.
#define MAX_STEP_OPERANDS 2
#define MAX_STEP_PAGES (MAX_STEP_OPERANDS * 2)
// Masked stores are vector stores.
#define MAX_MASKED 64

struct step_page {
    uintptr_t gpa;
    const struct region *r;
    uint8_t *scratch;
    uint8_t *before;
    // The operand's bytes in the page, and what the instruction does with
    // them, INSN_MEM_*.
    size_t start;
    size_t end;
    unsigned int access;
    uint8_t first[MAX_MASKED];
    bool mapped;
    uintptr_t hpa;
    uint32_t prot;
    uint8_t memtype;
};

struct mmio_step {
    size_t count;
    struct step_page pages[MAX_STEP_PAGES];
    const char *mode;
    struct vcpu_segment cs;
    uintptr_t rip;
    // Where the instruction leaves the guest once done.
    uint16_t done_cs;
    uintptr_t done_rip;
    bool branch;
    bool loads_flags;
    // The byte of a pushed RFLAGS image that holds TF, which SVM's stepping
    // sets.
    bool stores_flags;
    uintptr_t flags_gpa;
    // A masked store runs twice.
    bool masked;
    bool second;
};

static const char *mode_name(struct vcpu *v, const struct vcpu_segment *cs) {
    if ((vcpu_cr0(v) & CR0_PE) == 0) {
        return "real-mode";
    }
    if ((vcpu_rflags(v) & RFLAGS_VM) != 0) {
        return "virtual-8086";
    }
    if ((vcpu_efer(v) & EFER_LMA) != 0 && (cs->attrib & VCPU_SEG_L) != 0) {
        return "64-bit";
    }
    return (cs->attrib & VCPU_SEG_DB) != 0 ? "32-bit" : "16-bit";
}

// What the guest runs that the hypervisor neither carries out nor can step
// against no more than its operands, within the hypervisor: the machine stops,
// showing the instruction, for it to be carried out.
static noreturn void stop(
    struct vcpu *v,
    const char *mode,
    const struct vcpu_segment *cs,
    uintptr_t rip,
    uintptr_t gpa,
    const char *why
) {
    static const char digits[] = "0123456789abcdef";
    bool long_mode = (vcpu_efer(v) & EFER_LMA) != 0 && (cs->attrib & VCPU_SEG_L) != 0;
    char bytes[INSN_MAX_LENGTH * 3 + 1] = "unreadable";
    size_t count = 0;
    for (; count < INSN_MAX_LENGTH; count++) {
        uintptr_t linear = cs->base + rip + count;
        uint8_t byte;
        if (!guest_mem_linear_read(v, long_mode ? linear : linear & 0xffffffff, &byte, 1, true)) {
            break;
        }
        bytes[count * 3] = digits[byte >> 4];
        bytes[count * 3 + 1] = digits[byte & 0xf];
        bytes[count * 3 + 2] = ' ';
    }
    if (count != 0) {
        bytes[count * 3 - 1] = '\0';
    }
    panic(
        "The guest's %s instruction at %04x:%llx, bytes %s, reached emulated memory at %#llx, "
        "but %s\n", mode, cs->selector, (unsigned long long)rip, bytes, (unsigned long long)gpa,
        why
    );
}

static noreturn void unsupported(struct vcpu *v, uintptr_t gpa, const char *why) {
    struct vcpu_segment cs;
    vcpu_get_seg(v, VCPU_SEG_CS, &cs);
    stop(v, mode_name(v, &cs), &cs, vcpu_rip(v), gpa, why);
}

// The same for the instruction a step ran, which the guest has left.
static noreturn void step_unsupported(struct vcpu *v, uintptr_t gpa, const char *why) {
    struct mmio_step *s = v->step;
    stop(v, s->mode, &s->cs, s->rip, gpa, why);
}

// Maps a scratch page in place of the region's page at gpa, the operand's len
// bytes there holding what the region reads as where the instruction reads
// them, others nothing in particular.
static void step_map(struct vcpu *v, uintptr_t gpa, size_t len, unsigned int access) {
    struct mmio_step *s = v->step;
    uintptr_t page = page_down(gpa);
    const struct region *r = find(page);
    if (r == NULL) {
        return;
    }
    struct step_page *p = &s->pages[s->count++];
    if (p->scratch == NULL) {
        p->scratch = page_alloc(1);
        p->before = page_alloc(1);
    }
    p->gpa = page;
    p->r = r;
    p->start = gpa - page;
    p->end = p->start + len;
    p->access = access;
    memset(p->scratch, 0xff, PAGE_SIZE);
    if ((access & INSN_MEM_READ) != 0) {
        region_access(v, r, gpa, p->scratch + p->start, len, false);
    } else {
        memset(p->scratch + p->start, 0, len);
    }
    memcpy(p->before, p->scratch, PAGE_SIZE);
    p->mapped = gpt_translate(page, &p->hpa, &p->prot, &p->memtype);
    gpt_map(page, (uintptr_t)p->scratch, PAGE_SIZE, GPT_RWX, MT_WB);
    gpt_flush();
}

// FXRSTOR and the x87 environment and state forms, whose areas hold fields the
// manuals leave unused, which the processor may or may not reach.
static bool state_area(const struct insn *insn) {
    unsigned int reg = (insn->modrm >> 3) & 7;
    if (insn->space == (INSN_SPACE_LEGACY | 1)) {
        return insn->opcode_byte == 0xae && reg == 1;
    }
    bool x87 = insn->opcode_byte == 0xd9 || insn->opcode_byte == 0xdd;
    return insn->space == INSN_SPACE_LEGACY && x87 && (reg == 4 || reg == 6);
}

// The instruction's ModRM operand, where the step can take it.
static bool step_modrm_operand(
    struct vcpu *v,
    uintptr_t gpa,
    const struct insn *insn,
    struct insn_mem *mem
) {
    if (!insn_mem_operand(v, insn, mem) || mem->size == 0) {
        return false;
    }
    if (state_area(insn)) {
        unsupported(v, gpa, "the bytes its state area takes are not known");
    }
    bool masked = (mem->access & INSN_MEM_MASKED) != 0;
    if (mem->size > PAGE_SIZE || (masked && mem->size > MAX_MASKED)) {
        unsupported(v, gpa, "its operand is larger than a step takes");
    }
    if (masked && (mem->access & INSN_MEM_READ) != 0) {
        unsupported(v, gpa, "a masked load would read the elements its mask leaves out");
    }
    return true;
}

// Where a stack instruction leaves the guest once done, from the slots it pops
// where it pops its destination, which the step's pages now hold.
static void step_end(struct vcpu *v, uintptr_t gpa, const struct insn_stack *stack) {
    struct mmio_step *s = v->step;
    uint64_t offset = 0;
    uint16_t selector = 0;
    switch (stack->end) {
        case INSN_END_TARGET: {
            s->done_rip = stack->target;
            break;
        }
        case INSN_END_FAR: {
            s->done_rip = stack->target;
            s->done_cs = stack->selector;
            break;
        }
        case INSN_END_POP_NEAR:
        case INSN_END_POP_FAR: {
            bool read = guest_mem_linear_read(v, stack->mem.linear, &offset, stack->slot, false);
            if (read && stack->end == INSN_END_POP_FAR) {
                read = guest_mem_linear_read(
                    v, stack->mem.linear + stack->slot, &selector, sizeof(uint16_t), false
                );
                s->done_cs = selector;
            }
            if (!read) {
                unsupported(v, gpa, "where it returns to cannot be read");
            }
            s->done_rip = offset;
            break;
        }
        default: {
            break;
        }
    }
}

// The guest runs the instruction itself, with every other vCPU out of the
// guest, as the pages are theirs too, and no interrupt before it: only where
// the decoder knows the bytes of its operands, which the step's accesses are
// then exactly, and where each region they reach takes those accesses within
// the hypervisor. Anything else stops the machine.
static void step_begin(struct vcpu *v, uintptr_t gpa, const struct insn *insn) {
    struct insn_mem ops[MAX_STEP_OPERANDS];
    size_t count = 0;
    struct insn_mem mem;
    bool modrm = step_modrm_operand(v, gpa, insn, &mem);
    struct insn_stack stack;
    bool stacked = insn_stack_operand(v, insn, &stack);
    if (!modrm && !stacked) {
        // INS faults on its write to emulated memory only from a port that the
        // guest reaches itself, which the processor has read already.
        if (insn->opcode == 0x6c || insn->opcode == 0x6d) {
            unsupported(v, gpa, "its port was read already, and a step would read it again");
        }
        if (insn_mem_operand(v, insn, &mem) && mem.size == 0) {
            // It only takes the operand's address, as CLFLUSH: nothing for the
            // device.
            vcpu_advance(v, insn->length);
            return;
        }
        unsupported(v, gpa, "its operands are not known");
    }
    // A pop reaches the stack before the operand it writes, a push after the
    // one it reads, so that the first to fault is the processor's.
    if (stacked && (stack.mem.access & INSN_MEM_READ) != 0) {
        ops[count++] = stack.mem;
    }
    if (modrm) {
        ops[count++] = mem;
    }
    if (stacked && (stack.mem.access & INSN_MEM_READ) == 0) {
        ops[count++] = stack.mem;
    }
    if (!vcpu_can_single_step()) {
        unsupported(v, gpa, "the processor cannot single-step it");
    }
    uint64_t gpas[MAX_STEP_OPERANDS][2];
    unsigned int lens[MAX_STEP_OPERANDS][2];
    unsigned int pieces[MAX_STEP_OPERANDS];
    // The fault may be another access of the instruction's, as MOVDIR64B's to
    // its destination, or the processor's own, as to a page table.
    bool operand = false;
    for (size_t i = 0; i < count; i++) {
        bool reads = (ops[i].access & INSN_MEM_READ) != 0;
        bool writes = (ops[i].access & INSN_MEM_WRITE) != 0;
        if (!translate(v, ops[i].linear, ops[i].size, writes, gpas[i], lens[i], &pieces[i])) {
            return;
        }
        for (size_t j = 0; j < pieces[i]; j++) {
            operand |= (gpas[i][j] ^ gpa) < PAGE_SIZE;
            const struct region *r = find(gpas[i][j]);
            if (r == NULL) {
                continue;
            }
            if ((reads && !r->ops->reads_virtual) || (writes && !r->ops->writes_virtual)) {
                unsupported(v, gpa, "its operand reaches a device of the host's");
            }
            for (size_t k = 0; k < i; k++) {
                for (size_t l = 0; l < pieces[k]; l++) {
                    if ((gpas[k][l] ^ gpas[i][j]) < PAGE_SIZE) {
                        unsupported(v, gpa, "two of its operands share a page of emulated memory");
                    }
                }
            }
        }
    }
    if (!operand) {
        unsupported(v, gpa, "the access that faulted is none of its operands'");
    }
    if (v->step == NULL) {
        v->step = kmalloc(sizeof(struct mmio_step));
        memset(v->step, 0, sizeof(struct mmio_step));
    }
    struct mmio_step *s = v->step;
    struct vcpu_segment cs;
    vcpu_get_seg(v, VCPU_SEG_CS, &cs);
    s->count = 0;
    s->second = false;
    s->masked = modrm && (mem.access & INSN_MEM_MASKED) != 0;
    s->branch = stacked && stack.end != INSN_END_NEXT;
    s->loads_flags = stacked && stack.loads_flags;
    s->stores_flags = !virt_vmx && stacked && stack.stores_flags;
    if (s->stores_flags) {
        // TF is bit 8, in the second byte of the image, which is the push's
        // operand, the last.
        size_t i = count - 1;
        s->flags_gpa = lens[i][0] > 1 ? gpas[i][0] + 1 : gpas[i][1];
    }
    s->mode = mode_name(v, &cs);
    s->cs = cs;
    s->rip = vcpu_rip(v);
    s->done_cs = cs.selector;
    s->done_rip = s->rip + insn->length;
    vcpu_pause_others();
    for (size_t i = 0; i < count; i++) {
        for (size_t j = 0; j < pieces[i]; j++) {
            step_map(v, gpas[i][j], lens[i][j], ops[i].access);
        }
    }
    if (stacked) {
        step_end(v, gpa, &stack);
    }
    v->stepping = true;
    vcpu_interrupt_shadow(v);
    vcpu_single_step(v, true);
}

// Writes back the runs of bytes of an operand that a masked store's two runs
// wrote alike.
static void write_runs(struct vcpu *v, struct step_page *p) {
    for (size_t at = p->start; at < p->end; ) {
        if (p->first[at - p->start] != p->scratch[at]) {
            at++;
            continue;
        }
        size_t stop = at + 1;
        while (stop < p->end && p->first[stop - p->start] == p->scratch[stop]) {
            stop++;
        }
        region_access(v, p->r, p->gpa + at, p->scratch + at, stop - at, true);
        at = stop;
    }
}

// SVM steps with TF set, which a pushed RFLAGS image holds in place of the
// guest's own.
static void store_guest_tf(struct vcpu *v, const struct mmio_step *s) {
    uint8_t tf = v->step_tf ? (uint8_t)(RFLAGS_TF >> 8) : 0;
    for (size_t i = 0; i < s->count; i++) {
        const struct step_page *p = &s->pages[i];
        if (p->gpa == page_down(s->flags_gpa)) {
            size_t at = s->flags_gpa - p->gpa;
            p->scratch[at] = (uint8_t)((p->scratch[at] & ~(RFLAGS_TF >> 8)) | tf);
            return;
        }
    }
    // Where the image crosses into RAM, the processor wrote that byte there.
    uint8_t byte;
    if (guest_mem_phys_read(s->flags_gpa, &byte, 1)) {
        byte = (uint8_t)((byte & ~(RFLAGS_TF >> 8)) | tf);
        guest_mem_phys_write(s->flags_gpa, &byte, 1);
    }
}

void mmio_step_done(struct vcpu *v) {
    struct mmio_step *s = v->step;
    // A step can end in a fault or an event instead, with nothing written.
    struct vcpu_segment cs;
    vcpu_get_seg(v, VCPU_SEG_CS, &cs);
    bool completed = cs.selector == s->done_cs && vcpu_rip(v) == s->done_rip;
    if (s->masked && completed && !s->second) {
        // A masked store writes only the elements its mask selects, which may
        // hold what they did: it runs again over other values, and writes its
        // elements alike.
        for (size_t i = 0; i < s->count; i++) {
            struct step_page *p = &s->pages[i];
            memcpy(p->first, p->scratch + p->start, p->end - p->start);
            memset(p->scratch + p->start, 0xff, p->end - p->start);
        }
        vcpu_set_rip(v, s->rip);
        s->second = true;
        return;
    }
    if (completed && s->stores_flags) {
        store_guest_tf(v, s);
    }
    for (size_t i = 0; i < s->count; i++) {
        struct step_page *p = &s->pages[i];
        // What it changed beside its operands would come back as writes the
        // manuals do not give it.
        if (completed
         && (memcmp(p->scratch, p->before, p->start) != 0
          || memcmp(p->scratch + p->end, p->before + p->end, PAGE_SIZE - p->end) != 0)) {
            step_unsupported(v, p->gpa, "it changed bytes beside its operands");
        }
        if (completed && (p->access & INSN_MEM_WRITE) != 0) {
            if (s->masked) {
                write_runs(v, p);
            } else {
                region_access(
                    v, p->r, p->gpa + p->start, p->scratch + p->start, p->end - p->start, true
                );
            }
        }
        if (p->mapped) {
            gpt_map(p->gpa, p->hpa, PAGE_SIZE, p->prot, p->memtype);
        } else {
            gpt_map(p->gpa, 0, PAGE_SIZE, 0, 0);
        }
    }
    gpt_flush();
    s->count = 0;
    // The trap after the instruction is the one TF asked for as it began, none
    // where POPF or IRET sets it (SDM 325462-093 Vol. 3B, 20.3.1.4; APM 24593
    // rev. 3.46, 13.1.4).
    bool tf = vcpu_step_tf(v, s->branch);
    v->stepping = false;
    // POPF and IRET leave the trap flag the guest's.
    v->step_flags_loaded = completed && s->loads_flags;
    vcpu_single_step(v, false);
    v->step_flags_loaded = false;
    vcpu_resume_others();
    // The hypervisor's own step took the guest's MTF exit for its guest.
    if (completed) {
        nvmx_monitor_trap(v, 0);
    }
    // SVM's stepping took the trap the guest's own TF asked for, and the
    // breakpoints the instruction hit.
    if (completed && !virt_vmx) {
        vcpu_debug_traps(v, tf);
    }
}

// Code where nothing decodes is all ones to a PC, which begins with FF FF, an
// undefined opcode. The hypervisor runs no code from a device.
static void fetch_fault(struct vcpu *v, uintptr_t gpa) {
    struct vcpu_segment cs;
    vcpu_get_seg(v, VCPU_SEG_CS, &cs);
    bool long_mode = (vcpu_efer(v) & EFER_LMA) != 0 && (cs.attrib & VCPU_SEG_L) != 0;
    bool absent_code = true;
    for (size_t i = 0; i < 2; i++) {
        uintptr_t linear = cs.base + vcpu_rip(v) + i;
        if (!long_mode) {
            linear &= 0xffffffff;
        }
        uint64_t at;
        uint32_t pf_error;
        if (!guest_mem_linear_to_phys(v, linear, false, true, &at, &pf_error)) {
            absent_code = false;
            break;
        }
        absent_code &= find(gpt_cpu_gpa(at)) == &absent;
    }
    if (!absent_code) {
        unsupported(v, gpa, "it is code in emulated memory");
    }
    vcpu_inject_exception(v, 6, false, 0);
}

// A data access, or a fetch, to emulated memory.
static bool fault(struct vcpu *v, uintptr_t gpa, bool fetch) {
    if (gpt_a20_fill(gpa)) {
        return true;
    }
    gpa = gpt_cpu_gpa(gpa);
    const struct region *r = find(gpa);
    if (r == NULL) {
        return false;
    }
    profile_mmio(gpa);
    if (r == &absent) {
        static _Atomic(unsigned int) reported;
        if (atomic_fetch_add_explicit(&reported, 1, memory_order_relaxed) < ABSENT_REPORTS) {
            printf(
                "The guest reached %#llx, where nothing decodes, from %#llx\n",
                (unsigned long long)gpa, (unsigned long long)vcpu_rip(v)
            );
        }
    }
    if (v->stepping) {
        unsupported(v, gpa, "it reaches emulated memory beside its operands");
    }
    if (fetch) {
        fetch_fault(v, gpa);
        return true;
    }
    struct insn insn;
    if (!insn_decode(v, &insn)) {
        // The guest's EPT or nested page tables may keep it from being read,
        // where the guest's hypervisor takes the exit.
        if (!nvmx_exit_pending(v) && !nsvm_exit_pending(v)) {
            unsupported(v, gpa, "it cannot be read and decoded");
        }
        return true;
    }
    if (emulate(v, &insn, &guest_mem) == EMULATE_UNSUPPORTED) {
        step_begin(v, gpa, &insn);
    }
    return true;
}

bool mmio_fault(struct vcpu *v, uintptr_t gpa) {
    return fault(v, gpa, false);
}

bool mmio_fetch(struct vcpu *v, uintptr_t gpa) {
    return fault(v, gpa, true);
}
