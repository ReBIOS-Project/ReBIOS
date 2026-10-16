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
#include <memory.h>
#include <lib/log.h>
#include <lib/spinlock.h>
#include <sys/mm.h>
#include <sys/x86.h>
#include <virt/emulate.h>
#include <virt/guest_mem.h>
#include <virt/insn.h>
#include <virt/io.h>
#include <virt/mmio.h>
#include <virt/vcpu.h>

#define MAX_REGIONS 32

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

static const struct region *find(uint64_t gpa) {
    unsigned count = __atomic_load_n(&region_count, __ATOMIC_ACQUIRE);
    for (unsigned i = 0; i < count; i++) {
        if (gpa - regions[i].base < regions[i].size) {
            return &regions[i];
        }
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

bool mmio_fault(struct vcpu *v, uint64_t gpa) {
    if (find(gpa) == NULL) {
        return false;
    }
    struct insn insn;
    if (!insn_decode(v, &insn)) {
        panic("Cannot decode the instruction at %#llx that reached emulated memory at %#llx\n",
              (unsigned long long)vcpu_rip(v), (unsigned long long)gpa);
    }
    if (emulate(v, &insn, &guest_mem) == EMULATE_UNSUPPORTED) {
        printf("The instruction at %#llx that reached emulated memory at %#llx is not emulated, "
               "skipped\n", (unsigned long long)vcpu_rip(v), (unsigned long long)gpa);
        vcpu_advance(v, insn.length);
    }
    return true;
}
