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
#include <lib/log.h>
#include <sys/cpu.h>
#include <sys/x86.h>
#include <virt/event.h>
#include <virt/irq.h>
#include <virt/svm.h>
#include <virt/vcpu.h>
#include <virt/vlapic.h>
#include <virt/vmx.h>

// The fields VMX's interruption information and SVM's events share.
#define EVENT_VECTOR_MASK 0xffu
#define EVENT_TYPE_MASK (7u << 8)
#define EVENT_TYPE_EXTERNAL (0u << 8)
#define EVENT_TYPE_NMI (2u << 8)
#define EVENT_TYPE_EXCEPTION (3u << 8)
#define EVENT_TYPE_SOFTWARE (4u << 8)
#define EVENT_ERROR_VALID (1u << 11)
#define EVENT_VALID (1u << 31)

#define VECTOR_NMI 2
#define VECTOR_BP 3
#define VECTOR_OF 4

void event_exit(struct vcpu *v) {
    uint32_t info;
    uint32_t error = 0;
    if (virt_vmx) {
        info = (uint32_t)vmx_read(VMCS_IDT_VECTORING_INFO);
        if (!(info & INTR_INFO_VALID)) {
            return;
        }
        // Software interrupts and exceptions leave RIP at their instruction,
        // which raises them again.
        unsigned type = info & EVENT_TYPE_MASK;
        if (type == INTR_TYPE_SW_INTR || type == INTR_TYPE_PRIV_SW_EXCEPTION
            || type == INTR_TYPE_SW_EXCEPTION) {
            return;
        }
        if (info & INTR_INFO_ERROR_CODE) {
            error = (uint32_t)vmx_read(VMCS_IDT_VECTORING_ERROR);
        }
    } else {
        uint64_t exitintinfo = v->vmcb->control.exitintinfo;
        if (!(exitintinfo & SVM_EVENT_VALID)) {
            return;
        }
        info = (uint32_t)exitintinfo;
        unsigned vector = info & EVENT_VECTOR_MASK;
        unsigned type = info & EVENT_TYPE_MASK;
        if (type == EVENT_TYPE_SOFTWARE
            || (type == EVENT_TYPE_EXCEPTION && (vector == VECTOR_BP || vector == VECTOR_OF))) {
            return;
        }
        error = (uint32_t)(exitintinfo >> 32);
    }
    v->redeliver = info & (EVENT_VALID | EVENT_ERROR_VALID | EVENT_TYPE_MASK | EVENT_VECTOR_MASK);
    v->redeliver_error = error;
}

void event_nmi(struct vcpu *v) {
    v->nmi_pending = true;
}

void event_cancel(struct vcpu *v) {
    uint32_t info = (uint32_t)vmx_read(VMCS_ENTRY_INTR_INFO);
    if (!(info & INTR_INFO_VALID)) {
        return;
    }
    v->redeliver = info;
    if (info & INTR_INFO_ERROR_CODE) {
        v->redeliver_error = (uint32_t)vmx_read(VMCS_ENTRY_EXCEPTION_ERROR);
    }
    vmx_write(VMCS_ENTRY_INTR_INFO, 0);
}

bool event_cut_short(struct vcpu *v) {
    return (v->redeliver & EVENT_VALID) != 0;
}

static void inject(struct vcpu *v, uint32_t info, uint32_t error) {
    if (virt_vmx) {
        if (info & EVENT_ERROR_VALID) {
            vmx_write(VMCS_ENTRY_EXCEPTION_ERROR, error);
        }
        vmx_write(VMCS_ENTRY_INTR_INFO, info);
        return;
    }
    v->vmcb->control.eventinj = info | (uint64_t)error << 32;
}

static void set_proc_control(uint32_t control, bool on) {
    uint64_t controls = vmx_read(VMCS_PROC_CONTROLS);
    vmx_write(VMCS_PROC_CONTROLS, on ? controls | control : controls & ~(uint64_t)control);
}

// SVM shows nothing of the guest's NMI blocking without its NMI
// virtualisation, which not every processor has; an NMI goes in when the
// next entry injects nothing else.
static bool nmi_blocked(void) {
    if (!virt_vmx) {
        return false;
    }
    uint64_t intr = vmx_read(VMCS_GUEST_INTERRUPTIBILITY);
    return (intr & (INTERRUPTIBILITY_STI | INTERRUPTIBILITY_MOV_SS | INTERRUPTIBILITY_NMI)) != 0;
}

// Returns whether it injected one.
static bool deliver_nmi(struct vcpu *v) {
    if (__atomic_exchange_n(&v->cpu->nmi_pending, false, __ATOMIC_ACQUIRE)) {
        v->nmi_pending = true;
    }
    if (!v->nmi_pending) {
        return false;
    }
    if (nmi_blocked()) {
        set_proc_control(PROC_NMI_WINDOW_EXITING, true);
        return false;
    }
    if (virt_vmx) {
        set_proc_control(PROC_NMI_WINDOW_EXITING, false);
    }
    inject(v, EVENT_VALID | EVENT_TYPE_NMI | VECTOR_NMI, 0);
    v->nmi_pending = false;
    return true;
}

void event_interrupt(uint8_t vector) {
    struct cpu *cpu = this_cpu();
    struct vcpu *v = cpu->vcpu;
    switch (vector) {
        case VLAPIC_SPURIOUS_VECTOR: {
            return;
        }
        case VLAPIC_TIMER_VECTOR: {
            lapic_eoi();
            vlapic_timer_fired(v);
            return;
        }
        case VLAPIC_KICK_VECTOR: {
            __atomic_store_n(&cpu->kick, false, __ATOMIC_SEQ_CST);
            lapic_eoi();
            return;
        }
        default: {
            break;
        }
    }
    if (irq_host_vector(vector)) {
        irq_host_interrupt(vector);
        return;
    }
    // One of the guest's MSIs or I/O APIC entries, addressed to this vCPU:
    // level-triggered entries stay masked at the host until the guest's EOI.
    bool level = irq_mirror_taken(vector);
    lapic_eoi();
    vlapic_accept(v, vector, level);
}

static bool interruptible(struct vcpu *v) {
    if (virt_vmx) {
        return (vmx_read(VMCS_GUEST_RFLAGS) & RFLAGS_IF)
               && !(vmx_read(VMCS_GUEST_INTERRUPTIBILITY)
                    & (INTERRUPTIBILITY_STI | INTERRUPTIBILITY_MOV_SS));
    }
    return (v->vmcb->save.rflags & RFLAGS_IF) && !(v->vmcb->control.interrupt_shadow & 1);
}

#define WINDOW_OFF (-1)
#define WINDOW_ANY 0

// An exit once the guest can take an interrupt: any, or, for SVM, one of a
// priority class only the TPR holds back. VMX's TPR threshold does that.
static void interrupt_window(struct vcpu *v, int class) {
    if (virt_vmx) {
        set_proc_control(PROC_INTR_WINDOW_EXITING, class == WINDOW_ANY);
        return;
    }
    struct vmcb_control *c = &v->vmcb->control;
    c->vintr &= ~(SVM_VINTR_IRQ | SVM_VINTR_IGN_TPR | SVM_VINTR_PRIO_MASK);
    c->intercept_misc1 &= ~SVM_INTERCEPT_VINTR;
    if (class != WINDOW_OFF) {
        c->vintr |= SVM_VINTR_IRQ;
        c->vintr |= class == WINDOW_ANY ? SVM_VINTR_IGN_TPR : (uint64_t)class << SVM_VINTR_PRIO_SHIFT;
        c->intercept_misc1 |= SVM_INTERCEPT_VINTR;
    }
}

static bool interrupt_pending(struct vcpu *v) {
    return irq_extint_pending(v) || vlapic_pending(v) >= 0;
}

bool event_halt_ends(struct vcpu *v) {
    struct cpu *cpu = v->cpu;
    if (v->nmi_pending || cpu->nmi_pending || cpu->init_pending) {
        return true;
    }
    return (vcpu_rflags(v) & RFLAGS_IF) && interrupt_pending(v);
}

static void deliver_interrupt(struct vcpu *v) {
    bool extint = irq_extint_pending(v);
    int vector = extint ? -1 : vlapic_pending(v);
    if (!extint && vector < 0) {
        interrupt_window(v, vlapic_tpr_held(v));
        return;
    }
    if (!interruptible(v)) {
        interrupt_window(v, WINDOW_ANY);
        return;
    }
    if (extint) {
        // The 8259 answers the acknowledge only now, when the guest takes it.
        vector = irq_extint_acknowledge();
    } else {
        vlapic_take(v, (uint8_t)vector);
    }
    inject(v, EVENT_VALID | EVENT_TYPE_EXTERNAL | (uint32_t)vector, 0);
    interrupt_window(v, interrupt_pending(v) ? WINDOW_ANY : WINDOW_OFF);
}

void event_hold(struct vcpu *v) {
    if (virt_vmx) {
        set_proc_control(PROC_INTR_WINDOW_EXITING, false);
        set_proc_control(PROC_NMI_WINDOW_EXITING, false);
        return;
    }
    interrupt_window(v, WINDOW_OFF);
}

void event_entry(struct vcpu *v) {
    if (v->injecting) {
        // What the exit's handler injected comes first: only exits that
        // instructions cause inject, and those never cut a delivery short.
        if (v->redeliver & EVENT_VALID) {
            printf("Dropping event %#x for one the exit raised\n", v->redeliver);
        }
        v->redeliver = 0;
        v->injecting = false;
        return;
    }
    if (v->redeliver & EVENT_VALID) {
        inject(v, v->redeliver, v->redeliver_error);
        v->redeliver = 0;
        // What else is pending follows once the guest takes this.
        if (virt_vmx && (v->nmi_pending || v->cpu->nmi_pending)) {
            set_proc_control(PROC_NMI_WINDOW_EXITING, true);
        }
        if (interrupt_pending(v)) {
            interrupt_window(v, WINDOW_ANY);
        }
        return;
    }
    if (deliver_nmi(v)) {
        if (interrupt_pending(v)) {
            interrupt_window(v, WINDOW_ANY);
        }
        return;
    }
    deliver_interrupt(v);
}
