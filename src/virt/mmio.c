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
#include <virt/mmio.h>
#include <lib/heap.h>
#include <lib/log.h>
#include <lib/spinlock.h>
#include <memory.h>
#include <sys/mm.h>
#include <sys/x86.h>
#include <virt/emulate.h>
#include <virt/gpt.h>
#include <virt/guest_mem.h>
#include <virt/insn.h>
#include <virt/io.h>
#include <virt/nvmx.h>
#include <virt/profile.h>
#include <virt/vcpu.h>

#define MAX_REGIONS 256

// The accesses to where nothing decodes that are logged.
#define ABSENT_REPORTS 16

struct region {
    uintptr_t base;
    size_t size;
    const struct mmio_ops *ops;
    void *ctx;
};

static struct region regions[MAX_REGIONS];
static size_t region_count;
static struct spinlock region_lock;

int mmio_register(
    uintptr_t base, size_t size, const struct mmio_ops *ops, void *ctx
) {
    spinlock_acquire(&region_lock);
    int region = -1;
    if (region_count == MAX_REGIONS) {
        printf(
            "Too many emulated memory regions for %#llx\n",
            (unsigned long long)base
        );
    } else {
        region = (int)region_count;
        regions[region_count] = (struct region){ base, size, ops, ctx };
        __atomic_store_n(&region_count, region_count + 1, __ATOMIC_RELEASE);
    }
    spinlock_release(&region_lock);
    return region;
}

// A lookup meanwhile sees the region at one place or the other, or nowhere.
void mmio_move(int region, uintptr_t base, size_t size) {
    struct region *r = &regions[region];
    __atomic_store_n(&r->size, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&r->base, base, __ATOMIC_RELEASE);
    __atomic_store_n(&r->size, size, __ATOMIC_RELEASE);
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

static uint64_t absent_read(
    struct vcpu *v, void *ctx, uint64_t offset, size_t size
) {
    (void)v;
    (void)ctx;
    (void)offset;
    return size == 8 ? UINT64_MAX : ((uint64_t)1 << (size * 8)) - 1;
}

static void absent_write(
    struct vcpu *v, void *ctx, uint64_t offset, size_t size, uint64_t value
) {
    (void)v;
    (void)ctx;
    (void)offset;
    (void)size;
    (void)value;
}

static const struct mmio_ops absent_ops = { absent_read, absent_write, true };

// What the guest reaches where it has neither memory nor a device, as a PC
// does where nothing decodes: such as the addresses an OS moves a BAR to.
static const struct region absent = { 0, UINT64_MAX, &absent_ops, NULL };

static const struct region *find(uintptr_t gpa) {
    size_t count = __atomic_load_n(&region_count, __ATOMIC_ACQUIRE);
    for (size_t i = 0; i < count; i++) {
        size_t size = __atomic_load_n(&regions[i].size, __ATOMIC_ACQUIRE);
        if (gpa - __atomic_load_n(&regions[i].base, __ATOMIC_ACQUIRE) < size) {
            return &regions[i];
        }
    }
    uintptr_t hpa;
    uint32_t prot;
    if (!gpt_translate(gpa & ~(uint64_t)(PAGE_SIZE - 1), &hpa, &prot, NULL)) {
        return &absent;
    }
    return NULL;
}

// In naturally aligned pieces of up to 8 bytes.
static void region_access(
    struct vcpu *v, const struct region *r, uintptr_t gpa, uint8_t *buf,
    size_t len, bool write
) {
    while (len != 0) {
        uint64_t offset = gpa - r->base;
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
    struct vcpu *v, uintptr_t linear, size_t size, bool write, uint64_t *gpas,
    unsigned int *lens, unsigned int *pieces
) {
    *pieces = 0;
    while (size != 0) {
        uintptr_t gpa;
        uint32_t pf_error;
        if (!guest_linear_to_phys(v, linear, write, false, &gpa, &pf_error)) {
            if ((pf_error & PF_ERROR_EXIT) == 0) {
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

void mmio_phys_access(
    struct vcpu *v, uintptr_t gpa, void *buf, size_t len, bool write
) {
    const struct region *r = find(gpa);
    if (r != NULL) {
        region_access(v, r, gpa, buf, len, write);
    } else if (write) {
        // What is neither RAM nor emulated decodes nothing.
        guest_phys_write(gpa, buf, len);
    } else if (!guest_phys_read(gpa, buf, len)) {
        memset(buf, 0xff, len);
    }
}

static bool access(
    struct vcpu *v, uintptr_t linear, void *buf, size_t size, bool write
) {
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

bool mmio_read_linear(
    struct vcpu *v, uintptr_t linear, void *buf, size_t size
) {
    return access(v, linear, buf, size, false);
}

bool mmio_write_linear(
    struct vcpu *v, uintptr_t linear, const void *buf, size_t size
) {
    return access(v, linear, (void *)buf, size, true);
}

static bool mem_ins(
    struct vcpu *v, uintptr_t linear, uint16_t port, size_t size,
    uint32_t *value
) {
    (void)port;
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

static void mem_outs(
    struct vcpu *v, uint16_t port, size_t size, uint32_t value
) {
    vcpu_io_access(v, port, size);
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

static const struct emulate_mem guest_mem = {
    mmio_read_linear, mmio_write_linear, mem_ins, mem_outs
};

// The pages a single-stepped instruction runs against, and of them the bytes
// SDM 325462-093 and APM 40332 rev. 4.10 say it reads or writes: those are
// what their regions read as, and come back as writes; other bytes it changes
// come back too.
#define MAX_STEP_PAGES 4
// Masked stores are vector stores.
#define MAX_MASKED 64

struct step_page {
    uintptr_t gpa;
    const struct region *r;
    uint8_t *scratch;
    uint8_t *before;
    // The operand's bytes in the page, if any.
    size_t start;
    size_t end;
    uint8_t first[MAX_MASKED];
    bool mapped;
    uintptr_t hpa;
    uint32_t prot;
    uint8_t memtype;
};

struct mmio_step {
    size_t count;
    struct step_page pages[MAX_STEP_PAGES];
    // Whether the operand is known, and what it is.
    bool known;
    struct insn_mem mem;
    uintptr_t rip;
    uintptr_t next_rip;
    // A masked store runs twice.
    bool second;
};

// Maps a scratch page in place of the region's page at gpa. The operand's
// bytes in it hold what the region reads as, where the instruction reads them;
// others nothing in particular.
static struct step_page *step_map(struct vcpu *v, uintptr_t gpa, size_t len) {
    struct mmio_step *s = v->step;
    uintptr_t page = gpa & ~(uint64_t)(PAGE_SIZE - 1);
    const struct region *r = find(page);
    if (r == NULL || s->count == MAX_STEP_PAGES) {
        return NULL;
    }
    for (size_t i = 0; i < s->count; i++) {
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
        for (size_t offset = 0; offset < PAGE_SIZE; offset += 8) {
            uint64_t value = r->ops->read(
                v, r->ctx, page + offset - r->base, 8
            );
            memcpy(p->scratch + offset, &value, 8);
        }
    } else {
        memset(p->scratch, 0xff, PAGE_SIZE);
        if ((s->mem.access & INSN_MEM_READ) != 0) {
            region_access(v, r, gpa, p->scratch + p->start, len, false);
        } else {
            memset(p->scratch + p->start, 0, len);
        }
    }
    memcpy(p->before, p->scratch, PAGE_SIZE);
    p->mapped = gpt_translate(page, &p->hpa, &p->prot, &p->memtype);
    gpt_map(page, (uintptr_t)p->scratch, PAGE_SIZE, GPT_RWX, MT_WB);
    gpt_flush();
    return p;
}

// The guest runs the instruction itself, with every other vCPU out of the
// guest, as the pages are theirs too; one the decoder does not know, insn
// NULL, against the whole page.
static void step_begin(
    struct vcpu *v, uintptr_t gpa, const struct insn *insn
) {
    if (v->step == NULL) {
        v->step = kmalloc(sizeof(struct mmio_step));
        memset(v->step, 0, sizeof(struct mmio_step));
    }
    struct mmio_step *s = v->step;
    s->count = 0;
    s->second = false;
    s->known = insn != NULL && insn_mem_operand(v, insn, &s->mem)
        && s->mem.size <= PAGE_SIZE
        && ((s->mem.access & INSN_MEM_MASKED) == 0
            || s->mem.size <= MAX_MASKED);
    if (s->known && s->mem.size == 0) {
        // It only takes the operand's address, as CLFLUSH: nothing for the
        // device.
        vcpu_advance(v, insn->length);
        return;
    }
    if (!vcpu_can_single_step()) {
        printf(
            "The instruction at %#llx that reached emulated memory at %#llx "
            "cannot be emulated, skipped\n",
            (unsigned long long)vcpu_rip(v), (unsigned long long)gpa
        );
        if (insn != NULL) {
            vcpu_advance(v, insn->length);
        } else {
            vcpu_inject_exception(v, 6, false, 0);
        }
        return;
    }
    uint64_t gpas[2];
    unsigned int lens[2];
    unsigned int pieces = 0;
    if (s->known) {
        bool translated = translate(
            v, s->mem.linear, s->mem.size,
            (s->mem.access & INSN_MEM_WRITE) != 0, gpas, lens, &pieces
        );
        if (!translated) {
            return;
        }
        // The fault may be another access of the instruction's, as MOVDIR64B's
        // to its destination.
        bool operand = false;
        for (size_t i = 0; i < pieces; i++) {
            operand |= (gpas[i] ^ gpa) < PAGE_SIZE;
        }
        if (!operand) {
            s->known = false;
            pieces = 0;
        }
    }
    vcpu_pause_others();
    for (size_t i = 0; i < pieces; i++) {
        step_map(v, gpas[i], lens[i]);
    }
    if (!s->known) {
        step_map(v, gpa, 0);
    }
    s->rip = vcpu_rip(v);
    // Where its length is not known, whether it completed is not either.
    s->next_rip = insn != NULL ? s->rip + insn->length : UINT64_MAX;
    v->stepping = true;
    vcpu_single_step(v, true);
}

// Writes back the runs of bytes in [start, end) that changed, or that a masked
// store's two runs wrote alike.
static void write_runs(
    struct vcpu *v, struct step_page *p, size_t start, size_t end, bool masked
) {
    for (size_t at = start; at < end; ) {
        bool written = masked ? p->first[at - p->start] == p->scratch[at]
            : p->scratch[at] != p->before[at];
        if (!written) {
            at++;
            continue;
        }
        unsigned int stop = at + 1;
        while (stop < end
            && (masked ? p->first[stop - p->start] == p->scratch[stop]
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
    bool writes = s->known && (s->mem.access & INSN_MEM_WRITE) != 0;
    bool masked = writes && (s->mem.access & INSN_MEM_MASKED) != 0;
    if (masked && completed && !s->second) {
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
    for (size_t i = 0; i < s->count; i++) {
        struct step_page *p = &s->pages[i];
        if (completed && writes && p->end != p->start) {
            if (masked) {
                write_runs(v, p, p->start, p->end, true);
            } else {
                // All of them, alike or not.
                region_access(
                    v, p->r, p->gpa + p->start, p->scratch + p->start,
                    p->end - p->start, true
                );
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
    // The hypervisor's own step took the guest's MTF exit for its guest.
    if (completed) {
        nvmx_monitor_trap(v, 0);
    }
    // SVM's stepping took the trap the guest's own TF asked for, and the
    // breakpoints the instruction hit.
    if (completed && !virt_vmx) {
        vcpu_debug_traps(v, vcpu_guest_tf(v));
    }
}

// Ends a step that cannot go on, its writes dropped: past its instruction
// where that is known, else with #UD.
static void step_abort(struct vcpu *v) {
    struct mmio_step *s = v->step;
    for (size_t i = 0; i < s->count; i++) {
        struct step_page *p = &s->pages[i];
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
    if (s->next_rip != UINT64_MAX) {
        vcpu_set_rip(v, s->next_rip);
    } else {
        vcpu_inject_exception(v, 6, false, 0);
    }
}

// A data access, or a fetch, to emulated memory.
static bool fault(struct vcpu *v, uintptr_t gpa, bool fetch) {
    gpa = gpt_cpu_gpa(gpa);
    const struct region *r = find(gpa);
    if (r == NULL) {
        return false;
    }
    profile_mmio(gpa);
    if (r == &absent) {
        static unsigned int reported;
        if (__atomic_fetch_add(&reported, 1, __ATOMIC_RELAXED)
          < ABSENT_REPORTS) {
            printf(
                "The guest reached %#llx, where nothing decodes, from %#llx\n",
                (unsigned long long)gpa, (unsigned long long)vcpu_rip(v)
            );
        }
    }
    if (v->stepping) {
        // The stepped instruction reaches another page, and runs again; one
        // that reaches more than a step can map is left out.
        if (step_map(v, gpa, 0) == NULL && find(gpa) != NULL) {
            printf(
                "The instruction at %#llx reaches more emulated memory than "
                "can be stepped, skipped\n",
                (unsigned long long)vcpu_rip(v)
            );
            step_abort(v);
        }
        return true;
    }
    // Code there runs against the page, a step at a time.
    if (fetch) {
        step_begin(v, gpa, NULL);
        return true;
    }
    struct insn insn;
    if (!insn_decode(v, &insn)) {
        // The guest's EPT may not let it fetch the instruction.
        if (!nvmx_exit_pending(v)) {
            step_begin(v, gpa, NULL);
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
