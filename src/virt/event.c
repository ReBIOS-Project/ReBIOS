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
#include <virt/event.h>
#include <hw/lapic.h>
#include <lib/log.h>
#include <sys/cpu.h>
#include <sys/x86.h>
#include <virt/irq.h>
#include <virt/mmio.h>
#include <virt/nsvm.h>
#include <virt/nvmx.h>
#include <virt/pit.h>
#include <virt/svm.h>
#include <virt/vcpu.h>
#include <virt/virt.h>
#include <virt/vlapic.h>
#include <virt/vmx.h>

// The fields VMX's interruption information and SVM's events share.
#define EVENT_VECTOR_MASK ((uint32_t)0xff)
#define EVENT_TYPE_MASK ((uint32_t)7 << 8)
#define EVENT_TYPE_EXTERNAL ((uint32_t)0 << 8)
#define EVENT_TYPE_NMI ((uint32_t)2 << 8)
#define EVENT_TYPE_EXCEPTION ((uint32_t)3 << 8)
#define EVENT_TYPE_SOFTWARE ((uint32_t)4 << 8)
// FRED's INT1 and SYSCALL in SVM's events (AMD 69191 rev. 1.10).
#define EVENT_TYPE_INT1 ((uint32_t)5 << 8)
#define EVENT_TYPE_SYSCALL ((uint32_t)7 << 8)
#define EVENT_ERROR_VALID ((uint32_t)1 << 11)
// An exception FRED event delivery met (SDM 325462-093 Vol. 3C, 30.2.4; AMD
// 69191 rev. 1.10).
#define EVENT_NESTED ((uint32_t)1 << 13)
#define EVENT_VALID ((uint32_t)1 << 31)

// UINV, the vector of user-interrupt notifications (SDM 325462-093 Vol. 3A,
// 9.4).
#define MSR_IA32_UINTR_MISC 0x988
#define UINTR_MISC_UINV_SHIFT 32

void event_exit(struct vcpu *v) {
    uint32_t info;
    uint32_t error = 0;
    uint64_t data = 0;
    if (virt_vmx) {
        info = (uint32_t)vmx_read(VMCS_IDT_VECTORING_INFO);
        if ((info & INTR_INFO_VALID) == 0) {
            return;
        }
        // Software interrupts and exceptions, and FRED's SYSCALL and SYSENTER,
        // leave RIP at their instruction, which raises them again; those the
        // guest injects into its guest have none, so the guest's guest's go in
        // again with their length, as Linux 7.3's KVM has it.
        unsigned int type = info & EVENT_TYPE_MASK;
        if (type == INTR_TYPE_SW_INTR || type == INTR_TYPE_PRIV_SW_EXCEPTION
         || type == INTR_TYPE_SW_EXCEPTION || type == INTR_TYPE_OTHER_EVENT) {
            if (!nvmx_in_l2(v)) {
                return;
            }
            v->redeliver_length = (uint32_t)vmx_read(VMCS_EXIT_INSTR_LEN);
        }
        if ((info & INTR_INFO_ERROR_CODE) != 0) {
            error = (uint32_t)vmx_read(VMCS_IDT_VECTORING_ERROR);
        }
        if (vcpu_fred()) {
            data = vmx_read(VMCS_ORIGINAL_EVENT_DATA);
        }
    } else {
        uint64_t exitintinfo = v->vmcb->control.exitintinfo;
        if ((exitintinfo & SVM_EVENT_VALID) == 0) {
            return;
        }
        info = (uint32_t)exitintinfo;
        unsigned int vector = info & EVENT_VECTOR_MASK;
        unsigned int type = info & EVENT_TYPE_MASK;
        if (type == EVENT_TYPE_SOFTWARE || type == EVENT_TYPE_INT1 || type == EVENT_TYPE_SYSCALL
         || (type == EVENT_TYPE_EXCEPTION && (vector == VECTOR_BP || vector == VECTOR_OF))) {
            if (!nsvm_inject_again(v, exitintinfo)) {
                return;
            }
        }
        error = (uint32_t)(exitintinfo >> 32);
        if (vcpu_fred()) {
            data = v->vmcb->control.exitintdata;
        }
    }
    v->redeliver = info & (EVENT_VALID | EVENT_ERROR_VALID | EVENT_NESTED | EVENT_TYPE_MASK
                         | EVENT_VECTOR_MASK);
    v->redeliver_error = error;
    v->redeliver_data = data;
}

void event_nmi(struct vcpu *v) {
    v->nmi_pending = true;
}

void event_cancel(struct vcpu *v) {
    uint32_t info = (uint32_t)vmx_read(VMCS_ENTRY_INTR_INFO);
    if ((info & INTR_INFO_VALID) == 0) {
        return;
    }
    v->redeliver = info;
    if ((info & INTR_INFO_ERROR_CODE) != 0) {
        v->redeliver_error = (uint32_t)vmx_read(VMCS_ENTRY_EXCEPTION_ERROR);
    }
    v->redeliver_data = vcpu_fred() ? vmx_read(VMCS_INJECTED_EVENT_DATA) : 0;
    vmx_write(VMCS_ENTRY_INTR_INFO, 0);
}

bool event_cut_short(struct vcpu *v) {
    return (v->redeliver & EVENT_VALID) != 0;
}

// SDM 325462-093 Vol. 3B, 23.1.4: FLAGS, CS, and ip pushed, the handler from
// the interrupt table IDTR points to, and no error code.
static bool deliver_real_mode(struct vcpu *v, unsigned int vector, uintptr_t ip) {
    struct vcpu_segment idtr, ss, cs;
    vcpu_get_seg(v, VCPU_SEG_IDTR, &idtr);
    vcpu_get_seg(v, VCPU_SEG_SS, &ss);
    vcpu_get_seg(v, VCPU_SEG_CS, &cs);
    uint32_t handler;
    if (vector * 4 + 3 > idtr.limit || !mmio_read_linear(v, idtr.base + vector * 4, &handler, 4)) {
        return false;
    }
    uint64_t rflags = vcpu_rflags(v);
    uint16_t frame[3] = { (uint16_t)ip, cs.selector, (uint16_t)rflags };
    uint64_t sp_mask = (ss.attrib & VCPU_SEG_DB) != 0 ? (uint64_t)0xffffffff : (uint64_t)0xffff;
    uintptr_t rsp = vcpu_rsp(v);
    uint64_t sp = (rsp - sizeof(frame)) & sp_mask;
    for (size_t i = 0; i < 3; i++) {
        bool written = mmio_write_linear(v, ss.base + ((sp + 2 * i) & sp_mask), &frame[i], 2);
        if (!written) {
            return false;
        }
    }
    vcpu_set_rsp(v, (rsp & ~sp_mask) | sp);
    cs.selector = (uint16_t)(handler >> 16);
    cs.base = (uint64_t)cs.selector << 4;
    vcpu_set_seg(v, VCPU_SEG_CS, &cs);
    vcpu_set_rip(v, handler & 0xffff);
    vcpu_set_rflags(v, rflags & ~(uint64_t)(RFLAGS_IF | RFLAGS_TF | RFLAGS_RF | RFLAGS_AC));
    return true;
}

bool event_deliver_real_mode(struct vcpu *v) {
    unsigned int type = v->redeliver & EVENT_TYPE_MASK;
    if ((v->redeliver & EVENT_VALID) == 0 || (vcpu_cr0(v) & CR0_PE) != 0
     || (type != EVENT_TYPE_EXTERNAL && type != EVENT_TYPE_EXCEPTION)
     || !deliver_real_mode(v, v->redeliver & EVENT_VECTOR_MASK, vcpu_rip(v))) {
        return false;
    }
    v->redeliver = 0;
    return true;
}

noreturn void event_undeliverable(struct vcpu *v, uintptr_t gpa) {
    panic(
        "The guest's event %#x reached emulated memory at %#llx in its delivery, which the "
        "hypervisor carries out in real mode only, at %#llx\n", v->redeliver,
        (unsigned long long)gpa, (unsigned long long)vcpu_rip(v)
    );
}

bool event_raise_real_mode(struct vcpu *v, uint8_t vector, size_t length) {
    return (vcpu_cr0(v) & CR0_PE) == 0 && deliver_real_mode(v, vector, vcpu_rip(v) + length);
}

static void inject(struct vcpu *v, uint32_t info, uint32_t error, uint64_t data) {
    vcpu_set_event_data(v, data);
    if (virt_vmx) {
        if ((info & EVENT_ERROR_VALID) != 0) {
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
// virtualisation, which not every processor has; an NMI goes in when the next
// entry injects nothing else.
static bool nmi_blocked(void) {
    if (!virt_vmx) {
        return false;
    }
    uint64_t intr = vmx_read(VMCS_GUEST_INTERRUPTIBILITY);
    return (intr & (INTERRUPTIBILITY_STI | INTERRUPTIBILITY_MOV_SS | INTERRUPTIBILITY_NMI)) != 0;
}

// Returns whether it injected one.
static bool deliver_nmi(struct vcpu *v) {
    if (atomic_exchange_explicit(&v->cpu->nmi_pending, false, memory_order_acquire)) {
        v->nmi_pending = true;
    }
    if (!v->nmi_pending) {
        return false;
    }
    // The processor takes it once the guest can; one pending there already
    // stands for this one too, as a processor holds one NMI back at most.
    if (!virt_vmx && svm_vnmi) {
        v->vmcb->control.vintr |= SVM_VINTR_NMI;
        v->nmi_pending = false;
        return false;
    }
    // A clear global interrupt flag holds it until STGI, which exits.
    if (!nsvm_gif(v)) {
        return false;
    }
    if (nmi_blocked()) {
        set_proc_control(PROC_NMI_WINDOW_EXITING, true);
        return false;
    }
    if (virt_vmx) {
        set_proc_control(PROC_NMI_WINDOW_EXITING, false);
    }
    inject(v, EVENT_VALID | EVENT_TYPE_NMI | VECTOR_NMI, 0, 0);
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
        case VLAPIC_THERMAL_VECTOR: {
            lapic_eoi();
            vlapic_thermal_fired(v);
            return;
        }
        case VLAPIC_PERF_VECTOR: {
            lapic_eoi();
            vlapic_perf_fired(v);
            return;
        }
        case VLAPIC_KICK_VECTOR: {
            atomic_store_explicit(&cpu->kick, false, memory_order_seq_cst);
            lapic_eoi();
            if (cpu->bsp) {
                vlapic_posted_log();
            }
            return;
        }
        // A notification outside the guest: what it posted is merged before
        // the next entry, the guest's guest's too.
        case VLAPIC_POSTED_VECTOR:
        case VLAPIC_NESTED_POSTED_VECTOR: {
            lapic_eoi();
            return;
        }
        case VLAPIC_DEVICE_VECTOR: {
            irq_device_interrupt();
            return;
        }
        default: {
            break;
        }
    }
    if (vector >= VLAPIC_EILVT_VECTOR && vector < VLAPIC_EILVT_VECTOR + APIC_EILVT_MAX) {
        lapic_eoi();
        vlapic_eilvt_fired(v, vector - VLAPIC_EILVT_VECTOR);
        return;
    }
    if (irq_host_vector(vector)) {
        irq_host_interrupt(vector);
        return;
    }
    if (irq_slot_interrupt(vector)) {
        return;
    }
    // An interrupt the guest programmed into something the hypervisor does not
    // keep its own messages of, addressed to this vCPU.
    lapic_eoi();
    vlapic_accept(v, vector, false);
}

bool event_interruptible(struct vcpu *v) {
    if (virt_vmx) {
        if ((vmx_read(VMCS_GUEST_RFLAGS) & RFLAGS_IF) == 0) {
            return false;
        }
        uint64_t blocking = vmx_read(VMCS_GUEST_INTERRUPTIBILITY);
        return (blocking & (INTERRUPTIBILITY_STI | INTERRUPTIBILITY_MOV_SS)) == 0;
    }
    return nsvm_interruptible(v);
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
    // The guest's TPR, which holds back a priority class, changes in its
    // guest's exits alone.
    if (nsvm_in_l2(v)) {
        nsvm_interrupt_window(v, class == WINDOW_ANY);
        return;
    }
    // While the global interrupt flag holds interrupts back, the STGI that
    // sets it, intercepted, is the window, but where the processor holds the
    // window's virtual interrupt back under its copy of the flag itself (APM
    // 24593 rev. 3.46, 15.33.2).
    if (!nsvm_gif(v) && !svm_vgif) {
        class = WINDOW_OFF;
    }
    struct vmcb_control *c = &v->vmcb->control;
    c->vintr &= ~(SVM_VINTR_IRQ | SVM_VINTR_IGN_TPR | SVM_VINTR_PRIO_MASK);
    c->intercept_misc1 &= ~SVM_INTERCEPT_VINTR;
    if (class != WINDOW_OFF) {
        c->vintr |= SVM_VINTR_IRQ;
        c->vintr |= class == WINDOW_ANY
                  ? SVM_VINTR_IGN_TPR
                  : (uint64_t)class << SVM_VINTR_PRIO_SHIFT;
        c->intercept_misc1 |= SVM_INTERCEPT_VINTR;
    }
}

static bool interrupt_pending(struct vcpu *v) {
    return irq_extint_pending(v) || vlapic_pending(v) >= 0;
}

// What only an exit once the guest takes interrupts can deliver: the processor
// delivers the local APIC's itself where it virtualises them.
static bool window_needed(struct vcpu *v) {
    return irq_extint_pending(v) || (!vlapic_virtual_delivery(v) && vlapic_pending(v) >= 0)
        || pit_tick_returning(v->cpu);
}

bool event_halt_ends(struct vcpu *v) {
    struct cpu *cpu = v->cpu;
    if (v->injecting) {
        return true;
    }
    // A clear global interrupt flag holds back everything that would.
    if (!nsvm_gif(v)) {
        return false;
    }
    if (nsvm_in_l2(v)) {
        return nsvm_halt_ends(v);
    }
    if (v->nmi_pending || atomic_load_explicit(&cpu->nmi_pending, memory_order_acquire)
     || atomic_load_explicit(&cpu->init_pending, memory_order_acquire)) {
        return true;
    }
    if (!virt_vmx && svm_vnmi
     && (v->vmcb->control.vintr & (SVM_VINTR_NMI | SVM_VINTR_NMI_MASK)) == SVM_VINTR_NMI) {
        return true;
    }
    return (vcpu_rflags(v) & RFLAGS_IF) != 0 && interrupt_pending(v);
}

// An injected interrupt of the vector that UINV has is no interrupt to a guest
// in 64-bit mode with user interrupts on: the processor processes the
// notification instead, with no EOI (SDM 325462-093 Vol. 3C, 29.6), so it ends
// as soon as it is taken. UINV is the guest's, as VM entry loads none.
static bool user_notification(struct vcpu *v, uint8_t vector) {
    return virt_vmx && (vcpu_cr4(v) & CR4_UINTR) != 0 && (vcpu_efer(v) & EFER_LMA) != 0
        && (uint8_t)(rdmsr(MSR_IA32_UINTR_MISC) >> UINTR_MISC_UINV_SHIFT) == vector;
}

static void deliver_interrupt(struct vcpu *v) {
    bool extint = irq_extint_pending(v);
    bool virtual_delivery = vlapic_virtual_delivery(v);
    int vector = extint || virtual_delivery ? -1 : vlapic_pending(v);
    if (!extint && vector < 0) {
        int class = virtual_delivery ? WINDOW_OFF : vlapic_tpr_held(v);
        interrupt_window(v, pit_tick_returning(v->cpu) ? WINDOW_ANY : class);
        return;
    }
    if (!event_interruptible(v)) {
        interrupt_window(v, WINDOW_ANY);
        return;
    }
    if (extint) {
        // The 8259 answers the acknowledge only now, when the guest takes it.
        vector = irq_extint_acknowledge();
    } else if (user_notification(v, (uint8_t)vector)) {
        vlapic_dismiss(v, (uint8_t)vector);
    } else {
        vlapic_take(v, (uint8_t)vector);
    }
    inject(v, EVENT_VALID | EVENT_TYPE_EXTERNAL | (uint32_t)vector, 0, 0);
    interrupt_window(v, window_needed(v) ? WINDOW_ANY : WINDOW_OFF);
}

void event_hold(struct vcpu *v) {
    if (virt_vmx) {
        set_proc_control(PROC_INTR_WINDOW_EXITING, false);
        set_proc_control(PROC_NMI_WINDOW_EXITING, false);
        return;
    }
    interrupt_window(v, WINDOW_OFF);
}

// The windows for what is pending, once the guest has taken what goes first.
static void windows_after(struct vcpu *v, bool nmis, bool interrupts) {
    if (virt_vmx && nmis
     && (v->nmi_pending || atomic_load_explicit(&v->cpu->nmi_pending, memory_order_acquire))) {
        set_proc_control(PROC_NMI_WINDOW_EXITING, true);
    }
    if (interrupts && window_needed(v)) {
        interrupt_window(v, WINDOW_ANY);
    }
}

static void entry(struct vcpu *v, bool nmis, bool interrupts) {
    if (v->injecting) {
        // What the exit's handler injected comes first: only exits that
        // instructions cause inject, and those never cut a delivery short.
        if ((v->redeliver & EVENT_VALID) != 0) {
            printf("Dropping event %#x for one the exit raised\n", v->redeliver);
        }
        v->redeliver = 0;
        v->injecting = false;
        windows_after(v, nmis, interrupts);
        return;
    }
    if ((v->redeliver & EVENT_VALID) != 0) {
        inject(v, v->redeliver, v->redeliver_error, v->redeliver_data);
        unsigned int type = v->redeliver & EVENT_TYPE_MASK;
        if (virt_vmx
         && (type == INTR_TYPE_SW_INTR || type == INTR_TYPE_PRIV_SW_EXCEPTION
          || type == INTR_TYPE_SW_EXCEPTION || type == INTR_TYPE_OTHER_EVENT)) {
            vmx_write(VMCS_ENTRY_INSTR_LEN, v->redeliver_length);
        }
        v->redeliver = 0;
        windows_after(v, nmis, interrupts);
        return;
    }
    // A debug trap the processor left pending, as a monitor trap flag exit
    // does, would be lost to an injected event (SDM 325462-093 Vol. 3C,
    // 29.7.3), and goes before interrupts and their windows.
    if (virt_vmx
     && (vmx_read(VMCS_GUEST_PENDING_DBG) & (PENDING_DBG_ENABLED | PENDING_DBG_BS)) != 0) {
        windows_after(v, nmis, interrupts);
        return;
    }
    if (nmis && deliver_nmi(v)) {
        if (interrupts && window_needed(v)) {
            interrupt_window(v, WINDOW_ANY);
        }
        return;
    }
    if (interrupts) {
        deliver_interrupt(v);
    }
}

void event_entry(struct vcpu *v) {
    entry(v, true, true);
}

void event_entry_for(struct vcpu *v, bool nmis, bool interrupts) {
    entry(v, nmis, interrupts);
}
