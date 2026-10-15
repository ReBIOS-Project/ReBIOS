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
#include <lib/log.h>
#include <sys/cpu.h>
#include <virt/event.h>
#include <virt/svm.h>
#include <virt/vcpu.h>
#include <virt/vmx.h>

// The fields VMX's interruption information and SVM's events share.
#define EVENT_VECTOR_MASK 0xffu
#define EVENT_TYPE_MASK (7u << 8)
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

static void deliver_nmi(struct vcpu *v) {
    if (__atomic_exchange_n(&v->cpu->nmi_pending, false, __ATOMIC_ACQUIRE)) {
        v->nmi_pending = true;
    }
    if (!v->nmi_pending) {
        return;
    }
    if (nmi_blocked()) {
        set_proc_control(PROC_NMI_WINDOW_EXITING, true);
        return;
    }
    if (virt_vmx) {
        set_proc_control(PROC_NMI_WINDOW_EXITING, false);
    }
    inject(v, EVENT_VALID | EVENT_TYPE_NMI | VECTOR_NMI, 0);
    v->nmi_pending = false;
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
        // The NMI follows once the guest takes this.
        if (virt_vmx && (v->nmi_pending || v->cpu->nmi_pending)) {
            set_proc_control(PROC_NMI_WINDOW_EXITING, true);
        }
        return;
    }
    deliver_nmi(v);
}
