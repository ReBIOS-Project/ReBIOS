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
#include <hw/power.h>
#include <lib/log.h>
#include <sys/cpu.h>
#include <sys/fixup.h>
#include <sys/mm.h>
#include <sys/x86.h>
#include <virt/event.h>
#include <virt/exit.h>
#include <virt/gpt.h>
#include <virt/mmio.h>
#include <virt/profile.h>
#include <virt/svm.h>
#include <virt/vcpu.h>
#include <virt/vioapic.h>
#include <virt/virt.h>
#include <virt/vlapic.h>

#define SVM_FEATURE_NP (1u << 0)
#define SVM_FEATURE_NRIPS (1u << 3)

bool svm_nrips;

void svm_enter(uint64_t vmcb_pa, struct gprs *gprs);

bool svm_supported(void) {
    if (cpuid(0x80000000, 0).eax < 0x8000000a) {
        return false;
    }
    if (!(cpuid(0x80000001, 0).ecx & (1u << 2))) {
        return false;
    }
    if (rdmsr(MSR_VM_CR) & VM_CR_SVMDIS) {
        printf("SVM is disabled by the firmware\n");
        return false;
    }

    uint32_t features = cpuid(0x8000000a, 0).edx;
    if (!(features & SVM_FEATURE_NP)) {
        printf("SVM lacks nested paging\n");
        return false;
    }
    svm_nrips = (features & SVM_FEATURE_NRIPS) != 0;
    return true;
}

bool svm_huge_pages(void) {
    return (cpuid(0x80000001, 0).edx & (1u << 26)) != 0;
}

void svm_cpu_init(void) {
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_SVME);
    wrmsr(MSR_VM_HSAVE_PA, (uint64_t)(uintptr_t)page_alloc(1));
    // INITs become #SX, which the guest's intercept or the host's handler
    // takes, rather than resetting the processor under the hypervisor.
    if (!wrmsr_safe(MSR_VM_CR, rdmsr(MSR_VM_CR) | VM_CR_R_INIT)) {
        printf("Cannot redirect INIT on this processor\n");
    }
}

void svm_vcpu_init(struct vcpu *v) {
    v->vmcb = page_alloc(1);
    v->vmcb_pa = (uint64_t)(uintptr_t)v->vmcb;

    struct vmcb_control *c = &v->vmcb->control;
    c->intercept_misc1 = SVM_INTERCEPT_INTR | SVM_INTERCEPT_NMI | SVM_INTERCEPT_CPUID
                         | SVM_INTERCEPT_HLT | SVM_INTERCEPT_INVD | SVM_INTERCEPT_IOIO
                         | SVM_INTERCEPT_MSR | SVM_INTERCEPT_SHUTDOWN;
    // Physical interrupts exit whatever the guest's IF, and CR8 is the virtual
    // local APIC's TPR.
    c->vintr = SVM_VINTR_MASKING;
    c->intercept_misc2 = SVM_INTERCEPT_VMRUN | SVM_INTERCEPT_VMMCALL | SVM_INTERCEPT_VMLOAD
                         | SVM_INTERCEPT_VMSAVE | SVM_INTERCEPT_STGI | SVM_INTERCEPT_CLGI
                         | SVM_INTERCEPT_SKINIT;
    // The legacy BIOS's VMCALL hypercalls raise #UD here, and INITs #SX.
    c->intercept_exceptions = 1u << 6 | 1u << 30;
    c->iopm_base_pa = virt_io_bitmap();
    c->msrpm_base_pa = virt_msr_bitmap();
    c->guest_asid = 1;
    // Flush everything on the first VMRUN, then nothing.
    c->tlb_control = 1;
    c->np_enable = 1;
    c->n_cr3 = gpt_root();

    struct vmcb_save *s = &v->vmcb->save;
    // The PAT's power-on value.
    s->g_pat = 0x0007040600070406ull;
    s->dr7 = 0x400;
    s->dr6 = 0xffff0ff0;
    // VMRUN requires SVME in the guest's EFER, which the guest is never shown.
    s->efer = EFER_SVME;
}

static void svm_dump_guest(struct vcpu *v) {
    struct vmcb_save *s = &v->vmcb->save;
    printf("Guest RIP %#llx RSP %#llx RFLAGS %#llx CPL %u\n",
           (unsigned long long)s->rip, (unsigned long long)s->rsp,
           (unsigned long long)s->rflags, s->cpl);
    printf("Guest CR0 %#llx CR3 %#llx CR4 %#llx EFER %#llx\n",
           (unsigned long long)s->cr0, (unsigned long long)s->cr3,
           (unsigned long long)s->cr4, (unsigned long long)s->efer);
    printf("Guest CS sel %#x base %#llx limit %#x attrib %#x\n", s->cs.selector,
           (unsigned long long)s->cs.base, s->cs.limit, s->cs.attrib);
}

static void svm_handle_io(struct vcpu *v) {
    uint64_t info = v->vmcb->control.exitinfo1;
    bool in = info & 1;
    bool string = (info >> 2) & 1;
    unsigned size = (info >> 4) & 7;
    uint16_t port = info >> 16;

    // SZ8, SZ16 and SZ32 are one-hot.
    size = size == 4 ? 4 : size == 2 ? 2 : 1;
    if (string) {
        exit_string_io(v, port);
        return;
    }

    uint32_t value = (uint32_t)v->gprs.rax;
    exit_io(v, port, size, in, &value);
    if (in) {
        if (size == 4) {
            v->gprs.rax = value;
        } else {
            uint64_t mask = (1ull << (size * 8)) - 1;
            v->gprs.rax = (v->gprs.rax & ~mask) | (value & mask);
        }
    }
    // Reported whether or not next RIP saving is supported.
    v->vmcb->save.rip = v->vmcb->control.exitinfo2;
    v->vmcb->control.interrupt_shadow = 0;
}

// The single-step trap ends a step, and so does a fault, which goes on to the
// guest.
static void svm_step_exception(struct vcpu *v, unsigned vector) {
    struct vmcb_control *c = &v->vmcb->control;
    mmio_step_done(v);
    if (vector == 1) {
        return;
    }
    bool has_error = vector == 8 || (vector >= 10 && vector <= 14) || vector == 17 || vector == 21;
    if (vector == 14) {
        vcpu_inject_page_fault(v, c->exitinfo2, (uint32_t)c->exitinfo1);
    } else {
        vcpu_inject_exception(v, (uint8_t)vector, has_error, (uint32_t)c->exitinfo1);
    }
}

static void svm_handle_exit(struct vcpu *v) {
    struct vmcb_control *c = &v->vmcb->control;
    profile_exit(v, (uint32_t)c->exitcode);
    if (v->stepping && c->exitcode >= SVM_EXIT_EXCEPTION_BASE
        && c->exitcode < SVM_EXIT_EXCEPTION_BASE + 32) {
        svm_step_exception(v, (unsigned)(c->exitcode - SVM_EXIT_EXCEPTION_BASE));
        return;
    }

    switch (c->exitcode) {
        case SVM_EXIT_INTR: {
            // Held while the host runs with GIF clear; the host's handlers
            // take them.
            v->cpu->taking_interrupts = true;
            asm volatile ("stgi\n\tsti\n\tnop\n\tcli\n\tclgi" ::: "memory");
            v->cpu->taking_interrupts = false;
            break;
        }
        case SVM_EXIT_VINTR: {
            break;
        }
        case SVM_EXIT_NMI: {
            // Held while the host runs with GIF clear; the host's handler
            // takes it.
            asm volatile ("stgi\n\tnop\n\tclgi" ::: "memory");
            break;
        }
        case SVM_EXIT_CPUID: {
            exit_cpuid(v);
            break;
        }
        case SVM_EXIT_HLT: {
            vcpu_halt(v);
            break;
        }
        case SVM_EXIT_EXCEPTION_BASE + 6: {
            exit_invalid_opcode(v);
            break;
        }
        case SVM_EXIT_EXCEPTION_BASE + 30: {
            vcpu_init_signal(v);
            break;
        }
        case SVM_EXIT_VMMCALL: {
            exit_hypercall(v);
            break;
        }
        case SVM_EXIT_IOIO: {
            svm_handle_io(v);
            break;
        }
        case SVM_EXIT_MSR: {
            if (c->exitinfo1 == 0) {
                exit_rdmsr(v);
            } else {
                exit_wrmsr(v);
            }
            break;
        }
        case SVM_EXIT_INVD: {
            // Throwing away the host's dirty cache lines is not an option.
            wbinvd();
            vcpu_skip_instruction(v);
            break;
        }
        case SVM_EXIT_SHUTDOWN: {
            printf("Guest shutdown\n");
            svm_dump_guest(v);
            power_reset(false);
        }
        case SVM_EXIT_VMRUN:
        case SVM_EXIT_VMLOAD:
        case SVM_EXIT_VMSAVE:
        case SVM_EXIT_STGI:
        case SVM_EXIT_CLGI:
        case SVM_EXIT_SKINIT: {
            vcpu_inject_exception(v, 6, false, 0);
            break;
        }
        case SVM_EXIT_NPF: {
            if (event_cut_short(v)) {
                panic("Event delivery reached emulated memory at %#llx\n",
                      (unsigned long long)c->exitinfo2);
            }
            if (!(c->exitinfo1 & NPF_FETCH) && mmio_fault(v, c->exitinfo2)) {
                break;
            }
            printf("Nested page fault at GPA %#llx, error code %#llx\n",
                   (unsigned long long)c->exitinfo2, (unsigned long long)c->exitinfo1);
            svm_dump_guest(v);
            panic("Unhandled guest-physical access\n");
        }
        case SVM_EXIT_INVALID: {
            printf("VMRUN failed, invalid guest state\n");
            svm_dump_guest(v);
            panic("Cannot enter the guest\n");
        }
        default: {
            printf("Unhandled #VMEXIT, code %#llx, info %#llx %#llx\n",
                   (unsigned long long)c->exitcode, (unsigned long long)c->exitinfo1,
                   (unsigned long long)c->exitinfo2);
            svm_dump_guest(v);
            panic("Unhandled #VMEXIT\n");
        }
    }
}

void svm_run(struct vcpu *v) {
    for (;;) {
        vcpu_before_entry(v);
        v->vmcb->save.rax = v->gprs.rax;
        svm_enter(v->vmcb_pa, &v->gprs);
        v->gprs.rax = v->vmcb->save.rax;
        v->vmcb->control.tlb_control = 0;
        // What it cut short comes back in EXITINTINFO.
        v->vmcb->control.eventinj = 0;
        vlapic_after_exit(v);
        event_exit(v);
        svm_handle_exit(v);
    }
}
