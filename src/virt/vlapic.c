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
#include <hw/lapic.h>
#include <hw/power.h>
#include <lib/heap.h>
#include <lib/log.h>
#include <lib/spinlock.h>
#include <sys/cpu.h>
#include <sys/fixup.h>
#include <sys/mm.h>
#include <sys/smp.h>
#include <sys/x86.h>
#include <virt/gpt.h>
#include <virt/irq.h>
#include <virt/mmio.h>
#include <virt/svm.h>
#include <virt/vcpu.h>
#include <virt/virt.h>
#include <virt/vlapic.h>
#include <virt/vmx.h>

#define MSR_IA32_ARCH_CAPABILITIES 0x10a
#define MSR_IA32_XAPIC_DISABLE_STATUS 0xbd
#define MSR_X2APIC_FIRST 0x800
#define MSR_X2APIC_LAST 0x8ff
#define MSR_X2APIC_ICR 0x830
#define MSR_X2APIC_SELF_IPI 0x83f

#define ARCH_CAPABILITIES_XAPIC_DISABLE_STATUS (1ull << 21)

#define APIC_BASE_BSP (1ull << 8)
#define APIC_BASE_EXTD (1ull << 10)
#define APIC_BASE_EN (1ull << 11)
#define APIC_BASE_ADDRESS 0x000ffffffffff000ull
#define APIC_DEFAULT_BASE 0xfee00000ull

#define APIC_APR 0x90
#define APIC_PPR 0xa0
#define APIC_RRD 0xc0
#define APIC_SELF_IPI 0x3f0

#define VERSION_EOI_SUPPRESSION (1u << 24)
#define SVR_WRITABLE 0x1ff
#define SVR_SUPPRESS_EOI_BROADCAST (1u << 12)
#define LVT_DELIVERY_MASK (7u << 8)
#define LVT_DELIVERY_EXTINT (7u << 8)
#define DFR_FLAT 0xf0000000u
#define ESR_RECEIVE_ILLEGAL_VECTOR (1u << 6)
#define XAPIC_BROADCAST 0xffu
#define X2APIC_BROADCAST 0xffffffffu
#define TIMER_DIVIDE_WRITABLE 0xbu

enum {
    LVT_CMCI,
    LVT_TIMER,
    LVT_THERMAL,
    LVT_PERF,
    LVT_LINT0,
    LVT_LINT1,
    LVT_ERROR,
    LVT_COUNT,
};

static const uint32_t lvt_regs[LVT_COUNT] = {
    APIC_LVT_CMCI, APIC_LVT_TIMER, APIC_LVT_THERMAL, APIC_LVT_PERF,
    APIC_LVT_LINT0, APIC_LVT_LINT1, APIC_LVT_ERROR,
};

// The bits of each entry that software writes: the vector and mask, the
// delivery mode where there is one, the timer mode, and LINT's polarity and
// trigger mode.
static const uint32_t lvt_writable[LVT_COUNT] = {
    0x107ff, 0x700ff, 0x107ff, 0x107ff, 0x1a7ff, 0x1a7ff, 0x100ff,
};

struct vlapic {
    uint64_t base;
    uint32_t id;
    // SVM's; VMX's is in the virtual-APIC page.
    uint32_t tpr;
    uint8_t *vapic_page;
    uint32_t ldr;
    uint32_t dfr;
    uint32_t svr;
    uint32_t esr;
    uint32_t esr_pending;
    uint32_t icr_low;
    uint32_t icr_high;
    uint32_t lvt[LVT_COUNT];
    uint32_t timer_divide;
    // Set from any processor.
    uint32_t irr[8];
    uint32_t tmr[8];
    uint32_t isr[8];
};

static uint32_t host_version;
// What a legacy BIOS hands the boot processor's local APIC over with.
static uint32_t legacy_svr;
static uint32_t legacy_lint0;
static uint32_t legacy_lint1;

// Pages the guest has had its local APICs at, which all exit.
#define MAX_PAGES 4
static uint64_t pages[MAX_PAGES];
static unsigned page_count;
static struct spinlock page_lock;

static const struct mmio_ops mmio_ops;

static void trap_page(uint64_t page) {
    spinlock_acquire(&page_lock);
    for (unsigned i = 0; i < page_count; i++) {
        if (pages[i] == page) {
            spinlock_release(&page_lock);
            return;
        }
    }
    if (page_count == MAX_PAGES) {
        printf("Local APIC page %#llx left mapped: too many\n", (unsigned long long)page);
        spinlock_release(&page_lock);
        return;
    }
    pages[page_count++] = page;
    spinlock_release(&page_lock);
    mmio_register(page, PAGE_SIZE, &mmio_ops, (void *)(uintptr_t)page);
    gpt_map(page, 0, PAGE_SIZE, 0, 0);
    gpt_flush();
}

void vlapic_init(void) {
    host_version = lapic_read(APIC_VERSION);
    legacy_svr = lapic_read(APIC_SVR);
    legacy_lint0 = lapic_read(APIC_LVT_LINT0);
    legacy_lint1 = lapic_read(APIC_LVT_LINT1);
    trap_page(APIC_DEFAULT_BASE);
    virt_intercept_msr(MSR_IA32_APIC_BASE, true, true);
    virt_intercept_msr(MSR_IA32_ARCH_CAPABILITIES, true, false);
    virt_intercept_msr(MSR_IA32_XAPIC_DISABLE_STATUS, true, true);
    for (uint32_t msr = MSR_X2APIC_FIRST; msr <= MSR_X2APIC_LAST; msr++) {
        virt_intercept_msr(msr, true, true);
    }
}

static bool enabled(const struct vlapic *vl) {
    return (vl->base & APIC_BASE_EN) != 0;
}

static bool x2apic(const struct vlapic *vl) {
    return (vl->base & (APIC_BASE_EN | APIC_BASE_EXTD)) == (APIC_BASE_EN | APIC_BASE_EXTD);
}

static bool sw_enabled(const struct vlapic *vl) {
    return (vl->svr & APIC_SVR_ENABLED) != 0;
}

static uint32_t x2apic_ldr(uint32_t id) {
    return (id >> 4) << 16 | 1u << (id & 0xf);
}

static uint32_t tpr_get(const struct vlapic *vl) {
    if (virt_vmx) {
        return *(volatile uint32_t *)(vl->vapic_page + APIC_TPR) & 0xff;
    }
    return vl->tpr;
}

static void tpr_set(struct vlapic *vl, uint32_t tpr) {
    if (virt_vmx) {
        *(volatile uint32_t *)(vl->vapic_page + APIC_TPR) = tpr & 0xff;
    } else {
        vl->tpr = tpr & 0xff;
    }
}

static int highest(const uint32_t *bits) {
    for (int i = 7; i >= 0; i--) {
        uint32_t word = __atomic_load_n(&bits[i], __ATOMIC_ACQUIRE);
        if (word != 0) {
            return i * 32 + 31 - __builtin_clz(word);
        }
    }
    return -1;
}

static uint32_t ppr(const struct vlapic *vl) {
    uint32_t tpr = tpr_get(vl);
    int isrv = highest(vl->isr);
    uint32_t isr_class = isrv < 0 ? 0 : (uint32_t)isrv & 0xf0;
    return (tpr & 0xf0) >= isr_class ? tpr : isr_class;
}

// Mirrors the guest's logical destination where the processor's local APIC
// is in the same mode, for the interrupts the guest addresses that way.
static void mirror_logical(const struct vlapic *vl) {
    if (!x2apic(vl) && !lapic_x2apic()) {
        lapic_write(APIC_LDR, vl->ldr);
        lapic_write(APIC_DFR, vl->dfr);
    }
}

// The timer the processor runs for the guest: the guest's registers, but for
// the vector, which is the hypervisor's.
static void timer_lvt(const struct vlapic *vl) {
    lapic_write(APIC_LVT_TIMER, (vl->lvt[LVT_TIMER] & ~0xffu) | VLAPIC_TIMER_VECTOR);
}

static void reset_registers(struct vcpu *v) {
    struct vlapic *vl = v->vlapic;
    tpr_set(vl, 0);
    vl->ldr = x2apic(vl) ? x2apic_ldr(vl->id) : 0;
    vl->dfr = 0xffffffff;
    vl->svr = 0xff;
    vl->esr = 0;
    vl->esr_pending = 0;
    vl->icr_low = 0;
    vl->icr_high = 0;
    for (unsigned i = 0; i < LVT_COUNT; i++) {
        vl->lvt[i] = APIC_LVT_MASKED;
    }
    vl->timer_divide = 0;
    for (unsigned i = 0; i < 8; i++) {
        __atomic_store_n(&vl->irr[i], 0, __ATOMIC_RELAXED);
        __atomic_store_n(&vl->tmr[i], 0, __ATOMIC_RELAXED);
        vl->isr[i] = 0;
    }
    lapic_write(APIC_TIMER_INITIAL, 0);
    timer_lvt(vl);
    lapic_write(APIC_TIMER_DIVIDE, 0);
    mirror_logical(vl);
}

void vlapic_reset(struct vcpu *v) {
    reset_registers(v);
}

void vlapic_create(struct vcpu *v) {
    struct vlapic *vl = kmalloc(sizeof(*vl));
    *vl = (struct vlapic){
        .base = APIC_DEFAULT_BASE | APIC_BASE_EN | (v->cpu->bsp ? APIC_BASE_BSP : 0),
        .id = v->cpu->apic_id,
    };
    v->vlapic = vl;
    if (virt_vmx) {
        vl->vapic_page = page_alloc(1);
        vmx_write(VMCS_VIRTUAL_APIC_ADDR, (uint64_t)(uintptr_t)vl->vapic_page);
        vmx_write(VMCS_TPR_THRESHOLD, 0);
    }
    reset_registers(v);
    if (v->cpu->bsp) {
        // Its LINT entries as lapic_legacy_init() set the processor's.
        vl->svr = legacy_svr & SVR_WRITABLE;
        vl->lvt[LVT_LINT0] = legacy_lint0 & lvt_writable[LVT_LINT0];
        vl->lvt[LVT_LINT1] = legacy_lint1 & lvt_writable[LVT_LINT1];
    }
}

bool vlapic_enabled(struct vcpu *v) {
    return enabled(v->vlapic);
}

void vlapic_accept(struct vcpu *v, uint8_t vector, bool level) {
    struct vlapic *vl = v->vlapic;
    if (!enabled(vl) || !sw_enabled(vl)) {
        return;
    }
    if (vector < 16) {
        __atomic_or_fetch(&vl->esr_pending, ESR_RECEIVE_ILLEGAL_VECTOR, __ATOMIC_RELAXED);
        return;
    }
    uint32_t bit = 1u << (vector % 32);
    if (level) {
        __atomic_or_fetch(&vl->tmr[vector / 32], bit, __ATOMIC_RELAXED);
    } else {
        __atomic_and_fetch(&vl->tmr[vector / 32], ~bit, __ATOMIC_RELAXED);
    }
    __atomic_or_fetch(&vl->irr[vector / 32], bit, __ATOMIC_RELEASE);
    if (v->cpu != this_cpu()) {
        vcpu_kick(v->cpu);
    }
}

void vlapic_timer_fired(struct vcpu *v) {
    struct vlapic *vl = v->vlapic;
    if (!(vl->lvt[LVT_TIMER] & APIC_LVT_MASKED)) {
        vlapic_accept(v, (uint8_t)vl->lvt[LVT_TIMER], false);
    }
}

int vlapic_pending(struct vcpu *v) {
    struct vlapic *vl = v->vlapic;
    if (!enabled(vl) || !sw_enabled(vl)) {
        return -1;
    }
    int irr = highest(vl->irr);
    if (irr < 0 || ((uint32_t)irr & 0xf0) <= (ppr(vl) & 0xf0)) {
        return -1;
    }
    return irr;
}

int vlapic_tpr_held(struct vcpu *v) {
    struct vlapic *vl = v->vlapic;
    if (!enabled(vl) || !sw_enabled(vl)) {
        return -1;
    }
    int irr = highest(vl->irr);
    int isrv = highest(vl->isr);
    if (irr < 0 || (isrv >= 0 && (irr & 0xf0) <= (isrv & 0xf0))) {
        return -1;
    }
    if (((uint32_t)irr & 0xf0) > (tpr_get(vl) & 0xf0)) {
        return -1;
    }
    return irr >> 4;
}

void vlapic_take(struct vcpu *v, uint8_t vector) {
    struct vlapic *vl = v->vlapic;
    uint32_t bit = 1u << (vector % 32);
    __atomic_and_fetch(&vl->irr[vector / 32], ~bit, __ATOMIC_ACQ_REL);
    vl->isr[vector / 32] |= bit;
}

bool vlapic_extint_open(struct vcpu *v) {
    struct vlapic *vl = v->vlapic;
    // A globally disabled local APIC leaves LINT0 the processor's INTR pin.
    if (!enabled(vl)) {
        return true;
    }
    uint32_t lvt = vl->lvt[LVT_LINT0];
    return !(lvt & APIC_LVT_MASKED) && (lvt & LVT_DELIVERY_MASK) == LVT_DELIVERY_EXTINT;
}

void vlapic_before_entry(struct vcpu *v) {
    struct vlapic *vl = v->vlapic;
    if (virt_vmx) {
        // An exit when the guest lowers its TPR below what holds back.
        int held = vlapic_tpr_held(v);
        vmx_write(VMCS_TPR_THRESHOLD, held < 0 ? 0 : (uint64_t)held);
        return;
    }
    struct vmcb_control *c = &v->vmcb->control;
    c->vintr = (c->vintr & ~0xfull) | (vl->tpr >> 4);
}

void vlapic_after_exit(struct vcpu *v) {
    if (virt_vmx) {
        return;
    }
    struct vlapic *vl = v->vlapic;
    uint32_t vtpr = (uint32_t)(v->vmcb->control.vintr & 0xf);
    if (vtpr != vl->tpr >> 4) {
        vl->tpr = vtpr << 4;
    }
}

static void eoi(struct vlapic *vl) {
    int vector = highest(vl->isr);
    if (vector < 0) {
        return;
    }
    vl->isr[vector / 32] &= ~(1u << (vector % 32));
    // Level-triggered ones end at the I/O APICs too, but where the guest
    // ends them there itself.
    if ((__atomic_load_n(&vl->tmr[vector / 32], __ATOMIC_ACQUIRE) >> (vector % 32)) & 1
        && !(vl->svr & SVR_SUPPRESS_EOI_BROADCAST)) {
        irq_mirror_eoi((uint8_t)vector);
    }
}

static bool matches(const struct vlapic *target, uint32_t dest, bool logical, bool x2apic_dest) {
    if (!logical) {
        if (x2apic_dest) {
            return dest == X2APIC_BROADCAST || dest == target->id;
        }
        return dest == XAPIC_BROADCAST || dest == (target->id & 0xff);
    }
    if (x2apic(target)) {
        if (dest == X2APIC_BROADCAST) {
            return true;
        }
        return dest >> 16 == target->ldr >> 16 && (dest & target->ldr & 0xffff) != 0;
    }
    uint32_t mda = dest & 0xff;
    uint32_t logical_id = target->ldr >> 24;
    if (mda == XAPIC_BROADCAST) {
        return true;
    }
    if ((target->dfr & DFR_FLAT) == DFR_FLAT) {
        return (mda & logical_id) != 0;
    }
    return mda >> 4 == logical_id >> 4 && (mda & logical_id & 0xf) != 0;
}

static void deliver(struct vcpu *from, struct vcpu *to, uint32_t command) {
    uint8_t vector = command & ICR_VECTOR_MASK;
    switch (command & ICR_DELIVERY_MASK) {
        case ICR_DELIVERY_FIXED:
        case ICR_DELIVERY_LOWEST: {
            vlapic_accept(to, vector, false);
            break;
        }
        case ICR_DELIVERY_NMI: {
            __atomic_store_n(&to->cpu->nmi_pending, true, __ATOMIC_RELEASE);
            vcpu_kick(to->cpu);
            break;
        }
        case ICR_DELIVERY_INIT: {
            // The sender takes its own once done with the write that sent it.
            if (__atomic_load_n(&to->sipi, __ATOMIC_ACQUIRE) == VCPU_SIPI_RUNNING) {
                __atomic_store_n(&to->cpu->init_pending, true, __ATOMIC_RELEASE);
                vcpu_kick(to->cpu);
            }
            break;
        }
        case ICR_DELIVERY_STARTUP: {
            // As on a processor, only a vCPU waiting for one takes it.
            int32_t waiting = VCPU_SIPI_WAITING;
            if (__atomic_compare_exchange_n(&to->sipi, &waiting, (int32_t)vector, false,
                                            __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
                vcpu_wake(to->cpu);
            }
            break;
        }
        default: {
            printf("IPI %#x from processor %#x not delivered\n", command, from->cpu->apic_id);
            break;
        }
    }
}

static void send_ipi(struct vcpu *from, uint32_t dest, uint32_t command) {
    uint32_t mode = command & ICR_DELIVERY_MASK;
    // The INIT level de-assert of old processors does nothing.
    if (mode == ICR_DELIVERY_INIT && !(command & ICR_ASSERT)) {
        return;
    }
    bool x2apic_dest = x2apic(from->vlapic);
    bool logical = (command & ICR_LOGICAL) != 0;
    uint32_t shorthand = command & ICR_SHORTHAND_MASK;
    for (uint32_t i = 0; i < smp_cpu_count(); i++) {
        struct vcpu *to = smp_cpu(i)->vcpu;
        if (to == NULL) {
            continue;
        }
        bool hit;
        switch (shorthand) {
            case ICR_SHORTHAND_SELF: {
                hit = to == from;
                break;
            }
            case ICR_SHORTHAND_ALL: {
                hit = true;
                break;
            }
            case ICR_SHORTHAND_OTHERS: {
                hit = to != from;
                break;
            }
            default: {
                hit = matches(to->vlapic, dest, logical, x2apic_dest);
                break;
            }
        }
        if (!hit) {
            continue;
        }
        deliver(from, to, command);
        // A lowest priority interrupt goes to one processor.
        if (mode == ICR_DELIVERY_LOWEST) {
            break;
        }
    }
}

// Returns false for registers that are not there.
static bool reg_read(struct vcpu *v, uint32_t reg, uint32_t *value) {
    struct vlapic *vl = v->vlapic;
    bool x2 = x2apic(vl);
    for (unsigned i = 0; i < LVT_COUNT; i++) {
        if (reg == lvt_regs[i]) {
            *value = vl->lvt[i];
            return true;
        }
    }
    if (reg >= APIC_ISR && reg < APIC_ISR + 0x80) {
        *value = vl->isr[(reg - APIC_ISR) / 0x10];
        return true;
    }
    if (reg >= APIC_TMR && reg < APIC_TMR + 0x80) {
        *value = __atomic_load_n(&vl->tmr[(reg - APIC_TMR) / 0x10], __ATOMIC_ACQUIRE);
        return true;
    }
    if (reg >= APIC_IRR && reg < APIC_IRR + 0x80) {
        *value = __atomic_load_n(&vl->irr[(reg - APIC_IRR) / 0x10], __ATOMIC_ACQUIRE);
        return true;
    }
    switch (reg) {
        case APIC_ID: {
            *value = x2 ? vl->id : vl->id << 24;
            return true;
        }
        case APIC_VERSION: {
            *value = host_version;
            return true;
        }
        case APIC_TPR: {
            *value = tpr_get(vl);
            return true;
        }
        case APIC_PPR: {
            *value = ppr(vl);
            return true;
        }
        case APIC_LDR: {
            *value = vl->ldr;
            return true;
        }
        case APIC_SVR: {
            *value = vl->svr;
            return true;
        }
        case APIC_ESR: {
            *value = vl->esr;
            return true;
        }
        case APIC_ICR_LOW: {
            *value = vl->icr_low & ~ICR_PENDING;
            return true;
        }
        case APIC_TIMER_INITIAL:
        case APIC_TIMER_CURRENT: {
            *value = lapic_read(reg);
            return true;
        }
        case APIC_TIMER_DIVIDE: {
            *value = vl->timer_divide;
            return true;
        }
        default: {
            break;
        }
    }
    if (x2) {
        return false;
    }
    switch (reg) {
        case APIC_DFR: {
            *value = vl->dfr;
            return true;
        }
        case APIC_ICR_HIGH: {
            *value = vl->icr_high;
            return true;
        }
        case APIC_APR:
        case APIC_RRD:
        case APIC_EOI: {
            *value = 0;
            return true;
        }
        default: {
            return false;
        }
    }
}

// Returns false where the write raises #GP in x2APIC mode.
static bool reg_write(struct vcpu *v, uint32_t reg, uint32_t value) {
    struct vlapic *vl = v->vlapic;
    bool x2 = x2apic(vl);
    for (unsigned i = 0; i < LVT_COUNT; i++) {
        if (reg == lvt_regs[i]) {
            value &= lvt_writable[i];
            // A software disabled local APIC keeps them masked.
            if (!sw_enabled(vl)) {
                value |= APIC_LVT_MASKED;
            }
            vl->lvt[i] = value;
            if (i == LVT_TIMER) {
                timer_lvt(vl);
            }
            return true;
        }
    }
    switch (reg) {
        case APIC_TPR: {
            tpr_set(vl, value);
            return true;
        }
        case APIC_EOI: {
            eoi(vl);
            return !x2 || value == 0;
        }
        case APIC_SVR: {
            uint32_t writable = SVR_WRITABLE;
            if (host_version & VERSION_EOI_SUPPRESSION) {
                writable |= SVR_SUPPRESS_EOI_BROADCAST;
            }
            vl->svr = value & writable;
            if (!sw_enabled(vl)) {
                for (unsigned i = 0; i < LVT_COUNT; i++) {
                    vl->lvt[i] |= APIC_LVT_MASKED;
                }
                timer_lvt(vl);
            }
            return true;
        }
        case APIC_ESR: {
            vl->esr = __atomic_exchange_n(&vl->esr_pending, 0, __ATOMIC_RELAXED);
            return !x2 || value == 0;
        }
        case APIC_TIMER_INITIAL: {
            lapic_write(APIC_TIMER_INITIAL, value);
            return true;
        }
        case APIC_TIMER_DIVIDE: {
            vl->timer_divide = value & TIMER_DIVIDE_WRITABLE;
            lapic_write(APIC_TIMER_DIVIDE, vl->timer_divide);
            return true;
        }
        default: {
            break;
        }
    }
    if (x2) {
        return false;
    }
    switch (reg) {
        case APIC_LDR: {
            vl->ldr = value & 0xff000000;
            mirror_logical(vl);
            break;
        }
        case APIC_DFR: {
            vl->dfr = value | 0x0fffffff;
            mirror_logical(vl);
            break;
        }
        case APIC_ICR_HIGH: {
            vl->icr_high = value & 0xff000000;
            break;
        }
        case APIC_ICR_LOW: {
            vl->icr_low = value;
            send_ipi(v, vl->icr_high >> 24, value);
            break;
        }
        default: {
            break;
        }
    }
    return true;
}

// Where the page is not the vCPU's xAPIC, it is not there.
static bool xapic_at(const struct vlapic *vl, uint64_t page) {
    return enabled(vl) && !x2apic(vl) && page == (vl->base & APIC_BASE_ADDRESS);
}

// Registers are 32 bits wide, in the first 32 bits of each 16-byte slot.
static uint32_t dword_read(struct vcpu *v, uint64_t page, uint64_t offset) {
    if (!xapic_at(v->vlapic, page)) {
        return 0xffffffff;
    }
    uint32_t value = 0;
    if ((offset & 0xc) != 0 || !reg_read(v, (uint32_t)offset, &value)) {
        return 0;
    }
    return value;
}

static void dword_write(struct vcpu *v, uint64_t page, uint64_t offset, uint32_t value) {
    if (xapic_at(v->vlapic, page) && (offset & 0xc) == 0) {
        reg_write(v, (uint32_t)offset, value);
    }
}

static uint64_t mmio_read(struct vcpu *v, void *ctx, uint64_t offset, unsigned size) {
    uint64_t page = (uint64_t)(uintptr_t)ctx;
    if (size == 8) {
        return dword_read(v, page, offset) | (uint64_t)dword_read(v, page, offset + 4) << 32;
    }
    uint64_t mask = (1ull << (size * 8)) - 1;
    return (dword_read(v, page, offset & ~3ull) >> ((offset & 3) * 8)) & mask;
}

static void mmio_write(struct vcpu *v, void *ctx, uint64_t offset, unsigned size, uint64_t value) {
    uint64_t page = (uint64_t)(uintptr_t)ctx;
    if (size == 8) {
        dword_write(v, page, offset, (uint32_t)value);
        dword_write(v, page, offset + 4, (uint32_t)(value >> 32));
        return;
    }
    // Narrower writes change part of a register.
    unsigned shift = (offset & 3) * 8;
    uint32_t mask = (uint32_t)(size == 4 ? 0xffffffffu : (1u << (size * 8)) - 1) << shift;
    uint32_t old = size == 4 ? 0 : dword_read(v, page, offset & ~3ull);
    dword_write(v, page, offset & ~3ull, (old & ~mask) | (((uint32_t)value << shift) & mask));
}

static const struct mmio_ops mmio_ops = {mmio_read, mmio_write, false};

static bool apic_base_write(struct vcpu *v, uint64_t value) {
    struct vlapic *vl = v->vlapic;
    uint64_t reserved = 0xff | (1ull << 9) | ~((1ull << cpu_physical_address_bits()) - 1);
    if (!lapic_x2apic_supported()) {
        reserved |= APIC_BASE_EXTD;
    }
    if (value & reserved) {
        return false;
    }
    bool en = value & APIC_BASE_EN;
    bool extd = value & APIC_BASE_EXTD;
    bool was_en = enabled(vl);
    bool was_x2 = x2apic(vl);
    // EXTD without EN, x2APIC mode straight from disabled, and xAPIC mode
    // straight from x2APIC mode are not allowed.
    if ((extd && !en) || (extd && !was_en) || (was_x2 && en && !extd)) {
        return false;
    }

    vl->base = (value & ~APIC_BASE_BSP) | (vl->base & APIC_BASE_BSP);
    if (!en) {
        if (was_en) {
            reset_registers(v);
        }
        return true;
    }
    if (extd != was_x2 || !was_en) {
        lapic_host_mode(extd);
        if (!was_en) {
            reset_registers(v);
        }
        if (extd) {
            // x2APIC mode derives the LDR, and keeps no ICR high half.
            vl->ldr = x2apic_ldr(vl->id);
            vl->icr_high = 0;
        }
        timer_lvt(vl);
        lapic_write(APIC_TIMER_DIVIDE, vl->timer_divide);
        mirror_logical(vl);
    }
    if (!extd) {
        trap_page(vl->base & APIC_BASE_ADDRESS);
    }
    return true;
}

bool vlapic_rdmsr(struct vcpu *v, uint32_t msr, uint64_t *value, bool *fault) {
    struct vlapic *vl = v->vlapic;
    *fault = false;
    switch (msr) {
        case MSR_IA32_APIC_BASE: {
            *value = vl->base;
            return true;
        }
        case MSR_IA32_ARCH_CAPABILITIES: {
            // The guest's xAPIC mode is never off for good.
            if (!rdmsr_safe(msr, value)) {
                *fault = true;
            }
            *value &= ~ARCH_CAPABILITIES_XAPIC_DISABLE_STATUS;
            return true;
        }
        case MSR_IA32_XAPIC_DISABLE_STATUS: {
            *value = 0;
            *fault = !lapic_xapic_locked();
            return true;
        }
        default: {
            break;
        }
    }
    if (msr < MSR_X2APIC_FIRST || msr > MSR_X2APIC_LAST) {
        return false;
    }
    uint32_t reg = (msr - MSR_X2APIC_FIRST) << 4;
    uint32_t low;
    if (!x2apic(vl) || reg == APIC_EOI || msr == MSR_X2APIC_SELF_IPI || !reg_read(v, reg, &low)) {
        *fault = true;
        return true;
    }
    *value = low;
    if (msr == MSR_X2APIC_ICR) {
        *value |= (uint64_t)vl->icr_high << 32;
    }
    return true;
}

bool vlapic_wrmsr(struct vcpu *v, uint32_t msr, uint64_t value, bool *fault) {
    struct vlapic *vl = v->vlapic;
    *fault = false;
    switch (msr) {
        case MSR_IA32_APIC_BASE: {
            *fault = !apic_base_write(v, value);
            return true;
        }
        case MSR_IA32_ARCH_CAPABILITIES: {
            *fault = true;
            return true;
        }
        case MSR_IA32_XAPIC_DISABLE_STATUS: {
            // Its lock is the host's to keep.
            *fault = !lapic_xapic_locked();
            return true;
        }
        default: {
            break;
        }
    }
    if (msr < MSR_X2APIC_FIRST || msr > MSR_X2APIC_LAST) {
        return false;
    }
    if (!x2apic(vl)) {
        *fault = true;
        return true;
    }
    if (msr == MSR_X2APIC_ICR) {
        vl->icr_low = (uint32_t)value;
        vl->icr_high = (uint32_t)(value >> 32);
        send_ipi(v, vl->icr_high, vl->icr_low);
    } else if (msr == MSR_X2APIC_SELF_IPI) {
        vlapic_accept(v, (uint8_t)value, false);
    } else {
        uint32_t reg = (msr - MSR_X2APIC_FIRST) << 4;
        uint32_t ignored;
        bool readable = reg == APIC_EOI || reg_read(v, reg, &ignored);
        if (!readable || (value >> 32) != 0 || reg == APIC_ID || reg == APIC_VERSION
            || reg == APIC_PPR || reg == APIC_LDR || reg == APIC_TIMER_CURRENT
            || (reg >= APIC_ISR && reg < APIC_IRR + 0x80)
            || !reg_write(v, reg, (uint32_t)value)) {
            *fault = true;
        }
    }
    return true;
}
