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
#include <virt/svm.h>
#include <virt/vcpu.h>
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
    if (!__atomic_exchange_n(&v->cpu->kick, false, __ATOMIC_ACQ_REL)) {
        v->nmi_pending = true;
    }
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
    return (intr & (INTERRUPTIBILITY_STI | INTERRUPTIBILITY_MOV_SS | INTERRUPTIBILITY_NMI))
           || vmx_read(VMCS_GUEST_ACTIVITY_STATE) == ACTIVITY_WAIT_SIPI;
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

void event_intr_exiting(struct vcpu *v, bool on) {
    if (virt_vmx) {
        uint64_t pin = vmx_read(VMCS_PIN_CONTROLS);
        vmx_write(VMCS_PIN_CONTROLS,
                  on ? pin | PIN_EXT_INTR_EXITING : pin & ~(uint64_t)PIN_EXT_INTR_EXITING);
        return;
    }
    // Physical interrupts exit regardless of the guest's IF only with
    // V_INTR_MASKING, which also keeps CR8 writes from the local APIC.
    struct vmcb_control *c = &v->vmcb->control;
    if (on) {
        c->intercept_misc1 |= SVM_INTERCEPT_INTR;
        c->vintr |= SVM_VINTR_MASKING;
    } else {
        c->intercept_misc1 &= ~SVM_INTERCEPT_INTR;
        c->vintr &= ~SVM_VINTR_MASKING;
    }
}

void event_interrupt(uint8_t vector) {
    struct vcpu *v = this_cpu()->vcpu;
    v->taken[vector / 64] |= 1ull << (vector % 64);
}

static int highest_taken(struct vcpu *v) {
    for (int i = 3; i >= 0; i--) {
        if (v->taken[i] != 0) {
            return i * 64 + 63 - __builtin_clzll(v->taken[i]);
        }
    }
    return -1;
}

static bool interruptible(struct vcpu *v) {
    if (virt_vmx) {
        return (vmx_read(VMCS_GUEST_RFLAGS) & RFLAGS_IF)
               && !(vmx_read(VMCS_GUEST_INTERRUPTIBILITY)
                    & (INTERRUPTIBILITY_STI | INTERRUPTIBILITY_MOV_SS))
               && vmx_read(VMCS_GUEST_ACTIVITY_STATE) != ACTIVITY_WAIT_SIPI;
    }
    return (v->vmcb->save.rflags & RFLAGS_IF) && !(v->vmcb->control.interrupt_shadow & 1);
}

// An exit once the guest can take an interrupt.
static void interrupt_window(struct vcpu *v, bool on) {
    if (virt_vmx) {
        set_proc_control(PROC_INTR_WINDOW_EXITING, on);
        return;
    }
    struct vmcb_control *c = &v->vmcb->control;
    if (on) {
        c->vintr |= SVM_VINTR_IRQ | SVM_VINTR_IGN_TPR;
        c->intercept_misc1 |= SVM_INTERCEPT_VINTR;
    } else {
        c->vintr &= ~(SVM_VINTR_IRQ | SVM_VINTR_IGN_TPR);
        c->intercept_misc1 &= ~SVM_INTERCEPT_VINTR;
    }
}

static bool interrupt_pending(struct vcpu *v) {
    return highest_taken(v) >= 0;
}

static void deliver_interrupt(struct vcpu *v) {
    int vector = highest_taken(v);
    if (vector < 0) {
        interrupt_window(v, false);
        return;
    }
    // One the guest took before, and has yet to EOI, outranks it, unless the
    // local APIC did not take it, as with an 8259's.
    if (!interruptible(v)
        || (lapic_in_service((uint8_t)vector) && lapic_highest_isr() > vector)) {
        interrupt_window(v, true);
        return;
    }
    v->taken[vector / 64] &= ~(1ull << (vector % 64));
    inject(v, EVENT_VALID | EVENT_TYPE_EXTERNAL | (uint32_t)vector, 0);
    interrupt_window(v, interrupt_pending(v));
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
            interrupt_window(v, true);
        }
        return;
    }
    if (deliver_nmi(v)) {
        if (interrupt_pending(v)) {
            interrupt_window(v, true);
        }
        return;
    }
    deliver_interrupt(v);
}
