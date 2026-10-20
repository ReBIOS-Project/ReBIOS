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
#include <stddef.h>
#include <stdbool.h>
#include <memory.h>
#include <lib/log.h>
#include <sys/x86.h>
#include <virt/guest_mem.h>
#include <virt/task.h>
#include <virt/vcpu.h>
#include <virt/vmx.h>

// The 32-bit and 16-bit TSS, SDM 325462-093 Vol. 3A, Figures 10-2 and 10-10.
#define TSS32_CR3 0x1c
#define TSS32_EIP 0x20
#define TSS32_EFLAGS 0x24
#define TSS32_GPRS 0x28
#define TSS32_SEGS 0x48
#define TSS32_LDT 0x60
#define TSS32_TRAP 0x64
#define TSS32_SIZE 0x68
#define TSS16_IP 0x0e
#define TSS16_FLAGS 0x10
#define TSS16_GPRS 0x12
#define TSS16_SEGS 0x22
#define TSS16_LDT 0x2a
#define TSS16_SIZE 0x2c

#define DESC_TYPE_MASK 0xfu
#define DESC_TYPE_BUSY 0x2u
#define DESC_TYPE_32BIT 0x8u
#define DESC_TYPE_LDT 0x2u
#define DESC_S (1u << 4)
#define DESC_P (1u << 7)
#define DESC_CODE (1u << 3)
// Readable for code, writable for data.
#define DESC_RW (1u << 1)
#define DESC_CONFORMING (1u << 2)
#define DESC_ACCESSED (1u << 0)
#define DESC_G (1u << 11)

#define VECTOR_TS 10
#define VECTOR_NP 11
#define VECTOR_SS 12

#define SELECTOR_TI (1u << 2)
#define SELECTOR_RPL 3u

// The EFLAGS bits that are not reserved (SDM 325462-093 Vol. 1, Figure 3-8).
#define EFLAGS_DEFINED 0x3f7fd5u

// The segment registers in the order both TSS formats keep them.
static const enum seg_reg tss_segs[] = {SEG_ES, SEG_CS, SEG_SS, SEG_DS, SEG_FS, SEG_GS};

struct desc {
    uint32_t lo;
    uint32_t hi;
};

static uint32_t desc_attrib(struct desc d) {
    return ((d.hi >> 8) & 0xff) | ((d.hi >> 12) & 0xf00);
}

static uint64_t desc_base(struct desc d) {
    return (d.lo >> 16) | (d.hi & 0xff) << 16 | (d.hi & 0xff000000);
}

static uint32_t desc_limit(struct desc d) {
    uint32_t limit = (d.lo & 0xffff) | (d.hi & 0xf0000);
    if (desc_attrib(d) & DESC_G) {
        limit = limit << 12 | 0xfff;
    }
    return limit;
}

struct switcher {
    struct vcpu *v;
    // Whether an exception is already on its way to the guest.
    bool faulted;
    struct segment ldtr;
};

static void fault(struct switcher *s, uint8_t vector, uint32_t error) {
    if (!s->faulted) {
        vcpu_inject_exception(s->v, vector, true, error);
        s->faulted = true;
    }
}

static bool system_read(struct switcher *s, uint64_t linear, void *buf, size_t len) {
    uint32_t pf_error;
    uint64_t pf_linear;
    if (guest_system_read(s->v, linear, buf, len, &pf_error, &pf_linear)) {
        return true;
    }
    if (!s->faulted) {
        vcpu_inject_page_fault(s->v, pf_linear, pf_error);
        s->faulted = true;
    }
    return false;
}

static bool system_write(struct switcher *s, uint64_t linear, const void *buf, size_t len) {
    uint32_t pf_error;
    uint64_t pf_linear;
    if (guest_system_write(s->v, linear, buf, len, &pf_error, &pf_linear)) {
        return true;
    }
    if (!s->faulted) {
        vcpu_inject_page_fault(s->v, pf_linear, pf_error);
        s->faulted = true;
    }
    return false;
}

// Where a selector's descriptor is, in the GDT or the LDT the switch loaded.
static bool desc_linear(struct switcher *s, uint16_t selector, uint64_t *linear) {
    struct segment table;
    if (selector & SELECTOR_TI) {
        table = s->ldtr;
        if (table.attrib & SEG_UNUSABLE) {
            return false;
        }
    } else {
        vcpu_get_seg(s->v, SEG_GDTR, &table);
    }
    uint32_t index = selector & ~7u;
    if ((uint64_t)index + 7 > table.limit) {
        return false;
    }
    *linear = table.base + index;
    return true;
}

static bool desc_read(struct switcher *s, uint16_t selector, struct desc *d, uint64_t *linear) {
    return desc_linear(s, selector, linear) && system_read(s, *linear, d, sizeof(*d));
}

static bool null_selector(uint16_t selector) {
    return (selector & ~SELECTOR_RPL) == 0 && !(selector & SELECTOR_TI);
}

// The type in a TSS descriptor's second dword, busy or available.
static void tss_mark(struct switcher *s, uint16_t selector, bool busy) {
    struct desc d;
    uint64_t linear;
    if (!desc_read(s, selector, &d, &linear)) {
        return;
    }
    if (busy) {
        d.hi |= DESC_TYPE_BUSY << 8;
    } else {
        d.hi &= ~(DESC_TYPE_BUSY << 8);
    }
    system_write(s, linear + 4, &d.hi, sizeof(d.hi));
}

// The LDT a TSS's LDT selector names, from the GDT, or false where it does not
// name a present one.
static bool ldt_lookup(struct switcher *s, uint16_t selector, struct segment *ldt) {
    struct segment gdtr;
    vcpu_get_seg(s->v, SEG_GDTR, &gdtr);
    uint32_t index = selector & ~7u;
    if ((selector & SELECTOR_TI) || (uint64_t)index + 7 > gdtr.limit) {
        return false;
    }
    struct desc d;
    if (!system_read(s, gdtr.base + index, &d, sizeof(d))) {
        return false;
    }
    uint32_t attrib = desc_attrib(d);
    if ((attrib & DESC_S) || (attrib & DESC_TYPE_MASK) != DESC_TYPE_LDT || !(attrib & DESC_P)) {
        return false;
    }
    *ldt = (struct segment){selector, attrib, desc_limit(d), desc_base(d)};
    return true;
}

static struct segment v86_segment(uint16_t selector) {
    return (struct segment){selector, 0xf3, 0xffff, (uint64_t)selector << 4};
}

// Step 9 loads every selector, step 14 the descriptors (SDM 325462-093 Vol. 3A,
// 10.3), so the registers the switch does not get to keep the new selectors
// with their old descriptors, as Linux 7.3's KVM leaves them.
static void set_selector(struct vcpu *v, enum seg_reg reg, uint16_t selector) {
    struct segment seg;
    vcpu_get_seg(v, reg, &seg);
    seg.selector = selector;
    vcpu_set_seg(v, reg, &seg);
}

// Loads the descriptor of a segment register with the checks of SDM 325462-093
// Vol. 3A, Tables 7-6 and 10-1; data and non-conforming code against CPL and
// RPL both, as Linux 7.3's KVM and other segment loads check them. They come
// after the commit point, so the first failure raises its exception in the new
// task, and the registers after it are left.
static bool load_seg(struct switcher *s, enum seg_reg reg, uint16_t selector, unsigned cpl) {
    if (s->faulted) {
        return false;
    }
    if (null_selector(selector)) {
        if (reg == SEG_CS || reg == SEG_SS) {
            fault(s, VECTOR_TS, 0);
            return false;
        }
        struct segment seg = {selector, SEG_UNUSABLE, 0, 0};
        vcpu_set_seg(s->v, reg, &seg);
        return true;
    }
    struct desc d;
    uint64_t linear;
    if (!desc_linear(s, selector, &linear)) {
        fault(s, VECTOR_TS, selector & ~SELECTOR_RPL);
        return false;
    }
    if (!system_read(s, linear, &d, sizeof(d))) {
        return false;
    }
    uint32_t attrib = desc_attrib(d);
    unsigned type = attrib & DESC_TYPE_MASK;
    unsigned dpl = (attrib >> 5) & 3;
    unsigned rpl = selector & SELECTOR_RPL;
    bool ok = (attrib & DESC_S) != 0;
    if (reg == SEG_CS) {
        ok = ok && (type & DESC_CODE)
             && ((type & DESC_CONFORMING) ? dpl <= rpl : dpl == rpl);
    } else if (reg == SEG_SS) {
        ok = ok && !(type & DESC_CODE) && (type & DESC_RW) && dpl == cpl && rpl == cpl;
    } else {
        bool readable = !(type & DESC_CODE) || (type & DESC_RW);
        bool conforming = (type & DESC_CODE) && (type & DESC_CONFORMING);
        ok = ok && readable && (conforming || (dpl >= cpl && dpl >= rpl));
    }
    if (!ok) {
        fault(s, VECTOR_TS, selector & ~SELECTOR_RPL);
        return false;
    }
    if (!(attrib & DESC_P)) {
        fault(s, reg == SEG_SS ? VECTOR_SS : VECTOR_NP, selector & ~SELECTOR_RPL);
        return false;
    }
    if (!(type & DESC_ACCESSED)) {
        d.hi |= DESC_ACCESSED << 8;
        attrib |= DESC_ACCESSED;
        system_write(s, linear + 4, &d.hi, sizeof(d.hi));
    }
    struct segment seg = {selector, attrib, desc_limit(d), desc_base(d)};
    vcpu_set_seg(s->v, reg, &seg);
    return true;
}

// LDTR first, which Table 10-1 checks before the other registers. A bad LDT
// leaves LDTR unusable, as VMX takes no LDTR with the TI flag set.
static void load_ldt(struct switcher *s, uint16_t selector) {
    s->ldtr = (struct segment){selector, SEG_UNUSABLE, 0, 0};
    if (!null_selector(selector) && !ldt_lookup(s, selector, &s->ldtr)) {
        s->ldtr = (struct segment){selector, SEG_UNUSABLE, 0, 0};
        fault(s, VECTOR_TS, selector & ~SELECTOR_RPL);
    }
    vcpu_set_seg(s->v, SEG_LDTR, &s->ldtr);
}

void task_switch(struct vcpu *v, uint16_t selector, enum task_source source,
                 const struct task_event *event, unsigned length) {
    struct switcher s = {.v = v};
    struct segment tr;
    vcpu_get_seg(v, SEG_TR, &tr);

    // What VMX has checked: that the descriptor is in the GDT, a TSS of the
    // right busy state, present, and large enough.
    struct desc nd;
    uint64_t nd_linear;
    if (!desc_read(&s, selector, &nd, &nd_linear)) {
        return;
    }
    uint32_t new_attrib = desc_attrib(nd);
    bool new32 = (new_attrib & DESC_TYPE_32BIT) != 0;
    uint64_t new_base = desc_base(nd);
    uint32_t new_limit = desc_limit(nd);
    uint8_t image[TSS32_SIZE];
    if (!system_read(&s, new_base, image, new32 ? TSS32_SIZE : TSS16_SIZE)) {
        return;
    }

    uint32_t new_regs[8];
    uint32_t new_eip;
    uint32_t new_eflags;
    uint32_t cr3 = 0;
    uint16_t sels[6] = {0};
    uint16_t ldt;
    if (new32) {
        memcpy(&cr3, image + TSS32_CR3, 4);
        memcpy(&new_eip, image + TSS32_EIP, 4);
        memcpy(&new_eflags, image + TSS32_EFLAGS, 4);
        memcpy(new_regs, image + TSS32_GPRS, sizeof(new_regs));
        for (unsigned i = 0; i < 6; i++) {
            memcpy(&sels[i], image + TSS32_SEGS + i * 4, 2);
        }
        memcpy(&ldt, image + TSS32_LDT, 2);
    } else {
        uint16_t words[2];
        memcpy(words, image + TSS16_IP, sizeof(words));
        new_eip = words[0];
        new_eflags = words[1];
        for (unsigned i = 0; i < 8; i++) {
            uint16_t value;
            memcpy(&value, image + TSS16_GPRS + i * 2, 2);
            new_regs[i] = value;
        }
        for (unsigned i = 0; i < 4; i++) {
            memcpy(&sels[i], image + TSS16_SEGS + i * 2, 2);
        }
        memcpy(&ldt, image + TSS16_LDT, 2);
    }
    new_eflags = (new_eflags & EFLAGS_DEFINED) | RFLAGS_FIXED;
    if (source == TASK_CALL || source == TASK_GATE) {
        new_eflags |= RFLAGS_NT;
    }
    bool v86 = (new_eflags & RFLAGS_VM) != 0;

    // The old task's state, as it resumes.
    uint64_t rip = vcpu_rip(v);
    if (source != TASK_GATE || event->software) {
        rip += length;
    }
    uint32_t eflags = (uint32_t)vcpu_rflags(v);
    if (source == TASK_IRET) {
        eflags &= ~RFLAGS_NT;
    }
    uint64_t *gpr = &v->gprs.rax;
    uint32_t regs[8];
    for (unsigned i = 0; i < 8; i++) {
        regs[i] = (uint32_t)(i == 4 ? vcpu_rsp(v) : gpr[i]);
    }
    bool old32 = (tr.attrib & DESC_TYPE_32BIT) != 0;
    if (old32) {
        uint8_t state[TSS32_LDT - TSS32_EIP];
        uint32_t eip = (uint32_t)rip;
        memcpy(state, &eip, 4);
        memcpy(state + 4, &eflags, 4);
        memcpy(state + TSS32_GPRS - TSS32_EIP, regs, sizeof(regs));
        for (unsigned i = 0; i < 6; i++) {
            struct segment seg;
            vcpu_get_seg(v, tss_segs[i], &seg);
            uint32_t value = seg.selector;
            memcpy(state + TSS32_SEGS - TSS32_EIP + i * 4, &value, 4);
        }
        if (!system_write(&s, tr.base + TSS32_EIP, state, sizeof(state))) {
            return;
        }
    } else {
        uint8_t state[TSS16_LDT - TSS16_IP];
        uint16_t words[2] = {(uint16_t)rip, (uint16_t)eflags};
        memcpy(state, words, sizeof(words));
        for (unsigned i = 0; i < 8; i++) {
            uint16_t value = (uint16_t)regs[i];
            memcpy(state + TSS16_GPRS - TSS16_IP + i * 2, &value, 2);
        }
        for (unsigned i = 0; i < 4; i++) {
            struct segment seg;
            vcpu_get_seg(v, tss_segs[i], &seg);
            memcpy(state + TSS16_SEGS - TSS16_IP + i * 2, &seg.selector, 2);
        }
        if (!system_write(&s, tr.base + TSS16_IP, state, sizeof(state))) {
            return;
        }
    }
    if (source == TASK_CALL || source == TASK_GATE) {
        if (!system_write(&s, new_base, &tr.selector, sizeof(tr.selector))) {
            return;
        }
    }

    // The commit point: what goes wrong from here on raises its exception in
    // the new task.
    struct segment new_tr = {selector, new_attrib | DESC_TYPE_BUSY, new_limit, new_base};
    vcpu_set_seg(v, SEG_TR, &new_tr);
    if (new32 && (vcpu_cr0(v) & CR0_PG)) {
        vmx_load_guest_cr3(cr3);
    }
    vcpu_set_rflags(v, new_eflags);
    vcpu_set_rip(v, new_eip);
    for (unsigned i = 0; i < 8; i++) {
        if (i == 4) {
            vcpu_set_rsp(v, new_regs[i]);
        } else {
            gpr[i] = new_regs[i];
        }
    }
    for (unsigned i = 0; i < 6; i++) {
        set_selector(v, tss_segs[i], sels[i]);
    }
    load_ldt(&s, ldt);

    // The busy flags, SDM 325462-093 Vol. 3A, Table 10-2.
    if (source == TASK_JMP || source == TASK_IRET) {
        tss_mark(&s, tr.selector, false);
    }
    if (source != TASK_IRET) {
        tss_mark(&s, selector, true);
    }

    if (v86) {
        for (unsigned i = 0; i < 6; i++) {
            struct segment seg = v86_segment(sels[i]);
            vcpu_set_seg(v, tss_segs[i], &seg);
        }
    } else {
        // CS's RPL is the new CPL, which SS's checks need first. Where CS
        // loads but SS does not, SS gets the new CPL, which VMX takes from it.
        unsigned cpl = sels[1] & SELECTOR_RPL;
        if (load_seg(&s, SEG_CS, sels[1], cpl) && !load_seg(&s, SEG_SS, sels[2], cpl)) {
            struct segment ss = {sels[2], SEG_UNUSABLE | cpl << 5, 0, 0};
            vcpu_set_seg(v, SEG_SS, &ss);
        }
        load_seg(&s, SEG_DS, sels[3], cpl);
        load_seg(&s, SEG_ES, sels[0], cpl);
        load_seg(&s, SEG_FS, sels[4], cpl);
        load_seg(&s, SEG_GS, sels[5], cpl);
    }

    vmx_task_switched(v, event != NULL && event->nmi);

    if (event != NULL && event->error_valid && !s.faulted) {
        struct segment ss;
        vcpu_get_seg(v, SEG_SS, &ss);
        unsigned size = new32 ? 4 : 2;
        uint64_t rsp = vcpu_rsp(v) - size;
        if (!(ss.attrib & SEG_DB)) {
            rsp = (vcpu_rsp(v) & ~0xffffull) | ((vcpu_rsp(v) - size) & 0xffff);
        }
        uint64_t sp = ss.attrib & SEG_DB ? rsp & 0xffffffff : rsp & 0xffff;
        if (system_write(&s, ss.base + sp, &event->error, size)) {
            vcpu_set_rsp(v, rsp);
        }
    }

    if (new32 && (image[TSS32_TRAP] & 1)) {
        printf("A task switch to TSS %#x asks for a debug trap, which is not raised\n", selector);
    }
}
