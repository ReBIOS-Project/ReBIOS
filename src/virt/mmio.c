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
#include <memory.h>
#include <lib/heap.h>
#include <lib/log.h>
#include <lib/spinlock.h>
#include <sys/mm.h>
#include <sys/x86.h>
#include <virt/emulate.h>
#include <virt/gpt.h>
#include <virt/guest_mem.h>
#include <virt/insn.h>
#include <virt/io.h>
#include <virt/mmio.h>
#include <virt/profile.h>
#include <virt/vcpu.h>

#define MAX_REGIONS 32

// The accesses to where nothing decodes that are logged.
#define ABSENT_REPORTS 16

struct region {
    uint64_t base;
    uint64_t size;
    const struct mmio_ops *ops;
    void *ctx;
};

static struct region regions[MAX_REGIONS];
static unsigned region_count;
static struct spinlock region_lock;

void mmio_register(uint64_t base, uint64_t size, const struct mmio_ops *ops, void *ctx) {
    spinlock_acquire(&region_lock);
    if (region_count == MAX_REGIONS) {
        panic("Too many emulated memory regions\n");
    }
    regions[region_count] = (struct region){base, size, ops, ctx};
    __atomic_store_n(&region_count, region_count + 1, __ATOMIC_RELEASE);
    spinlock_release(&region_lock);
}

static uint64_t absent_read(struct vcpu *v, void *ctx, uint64_t offset, unsigned size) {
    (void)v;
    (void)ctx;
    (void)offset;
    return size == 8 ? UINT64_MAX : (1ull << (size * 8)) - 1;
}

static void absent_write(struct vcpu *v, void *ctx, uint64_t offset, unsigned size,
                         uint64_t value) {
    (void)v;
    (void)ctx;
    (void)offset;
    (void)size;
    (void)value;
}

static const struct mmio_ops absent_ops = {absent_read, absent_write, true};

// What the guest reaches where it has neither memory nor a device, as a PC
// does where nothing decodes: such as the addresses an OS moves a BAR to.
static const struct region absent = {0, UINT64_MAX, &absent_ops, NULL};

static const struct region *find(uint64_t gpa) {
    unsigned count = __atomic_load_n(&region_count, __ATOMIC_ACQUIRE);
    for (unsigned i = 0; i < count; i++) {
        if (gpa - regions[i].base < regions[i].size) {
            return &regions[i];
        }
    }
    uint64_t hpa;
    uint32_t prot;
    if (!gpt_translate(gpa & ~(uint64_t)(PAGE_SIZE - 1), &hpa, &prot, NULL)) {
        return &absent;
    }
    return NULL;
}

// In naturally aligned pieces of up to 8 bytes.
static void region_access(struct vcpu *v, const struct region *r, uint64_t gpa, uint8_t *buf,
                          unsigned len, bool write) {
    while (len != 0) {
        uint64_t offset = gpa - r->base;
        unsigned size = 8;
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
static bool translate(struct vcpu *v, uint64_t linear, unsigned size, bool write, uint64_t *gpas,
                      unsigned *lens, unsigned *pieces) {
    *pieces = 0;
    while (size != 0) {
        uint64_t gpa;
        uint32_t pf_error;
        if (!guest_linear_to_phys(v, linear, write, false, &gpa, &pf_error)) {
            vcpu_inject_page_fault(v, linear, pf_error);
            return false;
        }
        unsigned chunk = PAGE_SIZE - (linear & (PAGE_SIZE - 1));
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

static bool access(struct vcpu *v, uint64_t linear, void *buf, unsigned size, bool write) {
    uint64_t gpas[2];
    unsigned lens[2];
    unsigned pieces;
    if (!translate(v, linear, size, write, gpas, lens, &pieces)) {
        return false;
    }
    uint8_t *p = buf;
    for (unsigned i = 0; i < pieces; i++) {
        const struct region *r = find(gpas[i]);
        if (r != NULL) {
            region_access(v, r, gpas[i], p, lens[i], write);
        } else if (write) {
            // What is neither RAM nor emulated decodes nothing.
            guest_phys_write(gpas[i], p, lens[i]);
        } else if (!guest_phys_read(gpas[i], p, lens[i])) {
            memset(p, 0xff, lens[i]);
        }
        p += lens[i];
    }
    return true;
}

static bool mem_read(struct vcpu *v, uint64_t linear, void *buf, unsigned size) {
    return access(v, linear, buf, size, false);
}

static bool mem_write(struct vcpu *v, uint64_t linear, const void *buf, unsigned size) {
    return access(v, linear, (void *)buf, size, true);
}

static bool mem_ins(struct vcpu *v, uint64_t linear, uint16_t port, unsigned size,
                    uint32_t *value) {
    (void)port;
    uint64_t gpas[2];
    unsigned lens[2];
    unsigned pieces;
    if (!translate(v, linear, size, true, gpas, lens, &pieces)) {
        return false;
    }
    const struct region *r = find(gpas[0]);
    if (r == NULL || !r->ops->writes_ignored) {
        return false;
    }
    *value = 0xffffffff;
    return true;
}

static void mem_outs(struct vcpu *v, uint16_t port, unsigned size, uint32_t value) {
    if (io_dispatch(v, port, size, false, &value)) {
        return;
    }
    switch (size) {
        case 1: {
            outb(port, (uint8_t)value);
            break;
        }
        case 2: {
            outw(port, (uint16_t)value);
            break;
        }
        default: {
            outl(port, value);
            break;
        }
    }
}

static const struct emulate_mem guest_mem = {mem_read, mem_write, mem_ins, mem_outs};

// The pages a single-stepped instruction runs against, and of them the bytes
// the SDM and APM say it reads or writes: those are what their regions read
// as, and come back as writes; other bytes it changes come back too.
#define MAX_STEP_PAGES 4
// Masked stores are vector stores.
#define MAX_MASKED 64

struct step_page {
    uint64_t gpa;
    const struct region *r;
    uint8_t *scratch;
    uint8_t *before;
    // The operand's bytes in the page, if any.
    unsigned start;
    unsigned end;
    uint8_t first[MAX_MASKED];
    bool mapped;
    uint64_t hpa;
    uint32_t prot;
    uint8_t memtype;
};

struct mmio_step {
    unsigned count;
    struct step_page pages[MAX_STEP_PAGES];
    // Whether the operand is known, and what it is.
    bool known;
    struct insn_mem mem;
    uint64_t rip;
    uint64_t next_rip;
    // A masked store runs twice.
    bool second;
};

// Maps a scratch page in place of the region's page at gpa. The operand's
// bytes in it hold what the region reads as, where the instruction reads
// them; others nothing in particular.
static struct step_page *step_map(struct vcpu *v, uint64_t gpa, unsigned len) {
    struct mmio_step *s = v->step;
    uint64_t page = gpa & ~(uint64_t)(PAGE_SIZE - 1);
    const struct region *r = find(page);
    if (r == NULL || s->count == MAX_STEP_PAGES) {
        return NULL;
    }
    for (unsigned i = 0; i < s->count; i++) {
        if (s->pages[i].gpa == page) {
            return NULL;
        }
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
    if (len == 0) {
        // Not the operand: all of what the region reads as.
        for (uint64_t offset = 0; offset < PAGE_SIZE; offset += 8) {
            uint64_t value = r->ops->read(v, r->ctx, page + offset - r->base, 8);
            memcpy(p->scratch + offset, &value, 8);
        }
    } else {
        memset(p->scratch, 0xff, PAGE_SIZE);
        if (s->mem.access & INSN_MEM_READ) {
            region_access(v, r, gpa, p->scratch + p->start, len, false);
        } else {
            memset(p->scratch + p->start, 0, len);
        }
    }
    memcpy(p->before, p->scratch, PAGE_SIZE);
    p->mapped = gpt_translate(page, &p->hpa, &p->prot, &p->memtype);
    gpt_map(page, (uint64_t)(uintptr_t)p->scratch, PAGE_SIZE, GPT_R | GPT_W, MT_WB);
    gpt_flush();
    return p;
}

// The guest runs the instruction itself, with every other vCPU out of the
// guest, as the pages are theirs too.
static void step_begin(struct vcpu *v, uint64_t gpa, const struct insn *insn) {
    if (v->step == NULL) {
        v->step = kmalloc(sizeof(*v->step));
        memset(v->step, 0, sizeof(*v->step));
    }
    struct mmio_step *s = v->step;
    s->count = 0;
    s->second = false;
    s->known = insn_mem_operand(v, insn, &s->mem) && s->mem.size <= PAGE_SIZE
               && (!(s->mem.access & INSN_MEM_MASKED) || s->mem.size <= MAX_MASKED);
    if (s->known && s->mem.size == 0) {
        // It only takes the operand's address, as CLFLUSH: nothing for the
        // device.
        vcpu_advance(v, insn->length);
        return;
    }
    if (!vcpu_can_single_step()) {
        printf("The instruction at %#llx that reached emulated memory at %#llx cannot be "
               "emulated, skipped\n", (unsigned long long)vcpu_rip(v), (unsigned long long)gpa);
        vcpu_advance(v, insn->length);
        return;
    }
    uint64_t gpas[2];
    unsigned lens[2];
    unsigned pieces = 0;
    if (s->known) {
        if (!translate(v, s->mem.linear, s->mem.size, (s->mem.access & INSN_MEM_WRITE) != 0, gpas, lens,
                       &pieces)) {
            return;
        }
        // The fault may be another access of the instruction's, as
        // MOVDIR64B's to its destination.
        bool operand = false;
        for (unsigned i = 0; i < pieces; i++) {
            operand |= (gpas[i] ^ gpa) < PAGE_SIZE;
        }
        if (!operand) {
            s->known = false;
            pieces = 0;
        }
    }
    vcpu_pause_others();
    for (unsigned i = 0; i < pieces; i++) {
        step_map(v, gpas[i], lens[i]);
    }
    if (!s->known) {
        step_map(v, gpa, 0);
    }
    s->rip = vcpu_rip(v);
    s->next_rip = s->rip + insn->length;
    v->stepping = true;
    vcpu_single_step(v, true);
}

// Writes back the runs of bytes in [start, end) that changed, or that a
// masked store's two runs wrote alike.
static void write_runs(struct vcpu *v, struct step_page *p, unsigned start, unsigned end, bool masked) {
    for (unsigned at = start; at < end;) {
        bool written = masked ? p->first[at - p->start] == p->scratch[at] : p->scratch[at] != p->before[at];
        if (!written) {
            at++;
            continue;
        }
        unsigned stop = at + 1;
        while (stop < end && (masked ? p->first[stop - p->start] == p->scratch[stop]
                                     : p->scratch[stop] != p->before[stop])) {
            stop++;
        }
        region_access(v, p->r, p->gpa + at, p->scratch + at, stop - at, true);
        at = stop;
    }
}

void mmio_step_done(struct vcpu *v) {
    struct mmio_step *s = v->step;
    // A step can end in a fault or an event instead, with nothing written.
    bool completed = vcpu_rip(v) == s->next_rip;
    bool writes = s->known && (s->mem.access & INSN_MEM_WRITE);
    bool masked = writes && (s->mem.access & INSN_MEM_MASKED);
    if (masked && completed && !s->second) {
        // A masked store writes only the elements its mask selects, which
        // may hold what they did: it runs again over other values, and
        // writes its elements alike.
        for (unsigned i = 0; i < s->count; i++) {
            struct step_page *p = &s->pages[i];
            memcpy(p->first, p->scratch + p->start, p->end - p->start);
            memset(p->scratch + p->start, 0xff, p->end - p->start);
        }
        vcpu_set_rip(v, s->rip);
        s->second = true;
        return;
    }
    for (unsigned i = 0; i < s->count; i++) {
        struct step_page *p = &s->pages[i];
        if (completed && writes && p->end != p->start) {
            if (masked) {
                write_runs(v, p, p->start, p->end, true);
            } else {
                // All of them, alike or not.
                region_access(v, p->r, p->gpa + p->start, p->scratch + p->start, p->end - p->start, true);
            }
            write_runs(v, p, 0, p->start, false);
            write_runs(v, p, p->end, PAGE_SIZE, false);
        } else if (p->end == p->start || !masked) {
            write_runs(v, p, 0, PAGE_SIZE, false);
        }
        if (p->mapped) {
            gpt_map(p->gpa, p->hpa, PAGE_SIZE, p->prot, p->memtype);
        } else {
            gpt_map(p->gpa, 0, PAGE_SIZE, 0, 0);
        }
    }
    gpt_flush();
    s->count = 0;
    v->stepping = false;
    vcpu_single_step(v, false);
    vcpu_resume_others();
}

bool mmio_fault(struct vcpu *v, uint64_t gpa) {
    const struct region *r = find(gpa);
    if (r == NULL) {
        return false;
    }
    profile_mmio(gpa);
    if (r == &absent) {
        static unsigned reported;
        if (__atomic_fetch_add(&reported, 1, __ATOMIC_RELAXED) < ABSENT_REPORTS) {
            printf("The guest reached %#llx, where nothing decodes, from %#llx\n",
                   (unsigned long long)gpa, (unsigned long long)vcpu_rip(v));
        }
    }
    if (v->stepping) {
        // The stepped instruction reaches another page, and runs again.
        if (step_map(v, gpa, 0) == NULL) {
            panic("The instruction at %#llx reached emulated memory at %#llx while stepped\n",
                  (unsigned long long)vcpu_rip(v), (unsigned long long)gpa);
        }
        return true;
    }
    struct insn insn;
    if (!insn_decode(v, &insn)) {
        panic("Cannot decode the instruction at %#llx that reached emulated memory at %#llx\n",
              (unsigned long long)vcpu_rip(v), (unsigned long long)gpa);
    }
    if (emulate(v, &insn, &guest_mem) == EMULATE_UNSUPPORTED) {
        step_begin(v, gpa, &insn);
    }
    return true;
}
