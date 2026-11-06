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
#include <virt/nsvm.h>
#include <core.h>
#include <handoff.h>
#include <lib/heap.h>
#include <lib/log.h>
#include <memory.h>
#include <sys/cpu.h>
#include <sys/mm.h>
#include <sys/smp.h>
#include <sys/x86.h>
#include <virt/cpuid.h>
#include <virt/event.h>
#include <virt/gpt.h>
#include <virt/guest_mem.h>
#include <virt/insn.h>
#include <virt/irq.h>
#include <virt/mmio.h>
#include <virt/svm.h>
#include <virt/vcpu.h>
#include <virt/virt.h>
#include <virt/vlapic.h>

#define VECTOR_UD 6
#define VECTOR_GP 13
#define VECTOR_PF 14
#define VECTOR_SX 30
// The error code of the #SX that a redirected INIT raises (APM 24593 rev.
// 3.46, 15.28).
#define SX_INIT 1

// The bits of VM_CR the guest has (APM 24593 rev. 3.46, 15.30.1).
#define VM_CR_DEFINED \
    (VM_CR_DPD | VM_CR_R_INIT | VM_CR_DIS_A20M | VM_CR_LOCK | VM_CR_SVMDIS)

// The bits EVENTINJ reserves, which VMRUN ignores, as Linux 7.3's KVM has it
// (nested.c, __nested_copy_vmcb_control_to_cache()).
#define EVENT_RESERVED ((uint64_t)0x7ffff000)
#define EVENT_TYPE_NMI ((uint64_t)2 << 8)
#define EVENT_TYPE_SOFT_INT ((uint64_t)4 << 8)

// The virtual interrupt the guest gives its guest, which the hypervisor's own
// windows take the place of (APM 24593 rev. 3.46, 15.21.4).
#define VIRQ_BITS \
    (SVM_VINTR_IRQ | SVM_VINTR_PRIO_MASK | SVM_VINTR_IGN_TPR | SVM_VINTR_VECTOR_MASK)

#define IOPM_SIZE (3 * PAGE_SIZE)
#define MSRPM_SIZE (2 * PAGE_SIZE)
// The bytes of the I/O permission map the processor reads: a bit for each
// port, and those for accesses past port FFFFh (APM 24593 rev. 3.46, 15.10.1).
#define IOPM_USED (2 * PAGE_SIZE + 1)

// The VMCB's FRED state, from FRED_RSP1 to FRED_CONFIG, in the state save area
// (AMD 69191 rev. 1.10).
#define VMCB_FRED_STATE 0xcc0
#define VMCB_FRED_SIZE 0x40

// Nested paging's tables, in the formats of the guest's page tables (APM 24593
// rev. 3.46, 15.25.3 and 15.25.5).
#define NPT_P ((uint64_t)1 << 0)
#define NPT_RW ((uint64_t)1 << 1)
#define NPT_US ((uint64_t)1 << 2)
#define NPT_PWT ((uint64_t)1 << 3)
#define NPT_PCD ((uint64_t)1 << 4)
#define NPT_A ((uint64_t)1 << 5)
#define NPT_D ((uint64_t)1 << 6)
#define NPT_PS ((uint64_t)1 << 7)
#define NPT_PAT_4K ((uint64_t)1 << 7)
#define NPT_PAT_LARGE ((uint64_t)1 << 12)
#define NPT_NX ((uint64_t)1 << 63)
#define NPT_ADDRESS ((uint64_t)0x000ffffffffff000)

// A nested page fault's error code (APM 24593 rev. 3.46, 15.25.7).
#define NPF_P ((uint64_t)1 << 0)
#define NPF_RW ((uint64_t)1 << 1)
#define NPF_US ((uint64_t)1 << 2)
#define NPF_RSV ((uint64_t)1 << 3)
#define NPF_ID ((uint64_t)1 << 4)
#define NPF_FINAL ((uint64_t)1 << 32)
#define NPF_TABLE ((uint64_t)1 << 33)

#define NPT02_ROOTS 4

// A shadow of one of the guest's nested page tables, for one ASID of its
// guests: what it and the hypervisor's make of the guest's guest's addresses,
// filled in as the processor misses them.
struct npt02 {
    uint64_t ncr3;
    uint32_t asid;
    uint64_t *root;
};

// What a walk of the guest's nested page tables made of an address of its
// guest: the guest's physical address, the level of the leaf, 0 for a 4 KiB
// page, the leaf, and the rights of every entry used, ANDed.
struct npt12_walk {
    uintptr_t gpa;
    size_t level;
    uint64_t leaf;
    bool writable;
    bool executable;
};

// SVM as the guest gets it.
struct nsvm {
    // EFER.SVME of the guest, and of its guest while that runs, which the
    // processor's EFER always has set for VMRUN.
    bool svme;
    bool svme02;
    uint64_t vm_cr;
    uint64_t hsave;
    // The global interrupt flag, one for the vCPU, which the guest's guest's
    // CLGI and STGI change too where the guest does not intercept them.
    bool gif;
    // VMCB01, the vCPU's own, and VMCB02, which runs the guest's guest, each
    // with its copy as its last exit left it, for its clean bits.
    struct vmcb *vmcb01;
    uintptr_t vmcb01_pa;
    uint8_t *seen01;
    struct vmcb *vmcb02;
    uintptr_t vmcb02_pa;
    uint8_t *seen02;
    bool in_l2;
    // Whether an entry into VMCB02 ran, which the hypervisor's own nested page
    // tables then cached translations of in other ASIDs.
    bool ran;
    // Whether the guest's VMRUN waits for the entry into VMCB02.
    bool run_pending;
    // VMCB12, the guest's, and a copy of it as VMRUN read it.
    uintptr_t vmcb12;
    struct vmcb copy;
    // VMCB12's intercepts, as far as the guest is offered them.
    uint32_t intercepts[SVM_INTERCEPT_VECTORS];
    // VMCB12's virtual interrupt control, which VMCB02 holds but while the
    // hypervisor has its virtual interrupt for a window of its own.
    uint64_t vintr12;
    bool window;
    uint64_t tsc_offset12;
    // VMCB02's MSR permission map: the hypervisor's and VMCB12's.
    uint8_t *msrpm02;
    // Whether VMCB12 enables nested paging, and the guest's paging mode at its
    // VMRUN, which the nested page tables take (APM 24593 rev. 3.46, 15.25.3):
    // the levels, below the PDPT for PAE paging, 64-bit entries, and the
    // guest's EFER.NXE, CR4.PSE, and PAT.
    bool npt;
    uint64_t ncr3;
    size_t npt_levels;
    bool npt_wide;
    bool npt_pae;
    bool npt_nx;
    bool npt_pse;
    bool npt_smep;
    uint64_t npt_pat;
    struct npt02 npt02[NPT02_ROOTS];
    struct npt02 *current02;
    size_t npt02_pages;
    size_t npt02_next;
    // An exit of the guest's guest that what the hypervisor carried out for it
    // raised, for the next entry.
    bool pending;
    uint64_t pending_code;
    uint64_t pending_info1;
    uint64_t pending_info2;
};

bool nsvm_enabled;
// MAXPHYADDR, and the processor's number of ASIDs, the first two of which are
// the host's and the guest's own: the guest's ASID n is the processor's n + 1.
static unsigned int phys_width;
static uint32_t nasid;
// VMCB02's I/O permission map, which takes every port, for the hypervisor to
// tell the guest's from its own.
static uint8_t *iopm02;
// How many vCPUs have the guest's EFER.SVME set.
static _Atomic(size_t) svm_count;
// The NPT02 tables each vCPU may have, of the heap's part for them, and the
// largest pages they map: 2 MiB, or 1 GiB.
static size_t npt02_budget;
static size_t npt02_level_max;

void nsvm_init(void) {
    if (virt_vmx) {
        return;
    }
    nasid = cpuid(0x8000000a, 0).ebx;
    // The guest needs an ASID of its own and one for a guest of its own.
    if (nasid < 3) {
        printf("Too few ASIDs for nested SVM\n");
        return;
    }
    nsvm_enabled = true;
    phys_width = cpuid(0x80000008, 0).eax & 0xff;
    npt02_budget = core_handoff->nested_heap_size / PAGE_SIZE / smp_cpu_count();
    npt02_level_max = svm_huge_pages() ? 2 : 1;
    iopm02 = page_alloc(IOPM_SIZE / PAGE_SIZE);
    memset(iopm02, 0xff, IOPM_SIZE);
    printf("Nested SVM for the guest\n");
}

void nsvm_vcpu_init(struct vcpu *v) {
    if (!nsvm_enabled) {
        return;
    }
    struct nsvm *n = kmalloc(sizeof(struct nsvm));
    // VM_CR as the firmware left it, but for the hypervisor's INIT redirection
    // and SVM, which the firmware left enabled.
    *n = (struct nsvm){
        .gif = true,
        .vm_cr = svm_firmware_vm_cr & (VM_CR_DPD | VM_CR_DIS_A20M | VM_CR_LOCK),
        .vmcb01 = v->vmcb,
        .vmcb01_pa = v->vmcb_pa
    };
    n->vmcb02 = page_alloc(1);
    n->vmcb02_pa = (uintptr_t)n->vmcb02;
    n->msrpm02 = page_alloc(MSRPM_SIZE / PAGE_SIZE);
    v->nsvm = n;
}

// The guest's RAM at a guest-physical page, for the VMCBs and permission maps
// SVM instructions and VMCBs name: not the page behind its local APICs' pages.
static uint8_t *guest_page(uintptr_t gpa) {
    uintptr_t hpa;
    uint32_t prot;
    uint8_t memtype;
    if (!gpt_translate(gpa, &hpa, &prot, &memtype) || (prot & (GPT_R | GPT_W)) != (GPT_R | GPT_W)
     || memtype != MT_WB || vlapic_access_page(hpa & ~(uint64_t)0xfff)) {
        return NULL;
    }
    return (uint8_t *)(uintptr_t)hpa;
}

static bool valid_page(uint64_t address) {
    return (address & 0xfff) == 0 && (address >> phys_width) == 0;
}

// Whether the guest has shadow stacks, whose state VMCBs hold.
static bool shadow_stacks(void) {
    return (cpuid_cr4_allowed() & CR4_CET) != 0;
}

// The global interrupt flag, which the processor's copy of, where it keeps one
// for the guest's virtual NMIs, follows.
static void set_gif(struct vcpu *v, struct nsvm *n, bool on) {
    n->gif = on;
    if (svm_vnmi) {
        struct vmcb_control *c = &v->vmcb->control;
        c->vintr = on ? c->vintr | SVM_VINTR_GIF : c->vintr & ~SVM_VINTR_GIF;
    }
}

bool nsvm_gif(struct vcpu *v) {
    return v->nsvm == NULL || v->nsvm->gif;
}

bool nsvm_svme(struct vcpu *v) {
    struct nsvm *n = v->nsvm;
    if (n == NULL) {
        return false;
    }
    return n->in_l2 ? n->svme02 : n->svme;
}

void nsvm_set_svme(struct vcpu *v, bool on) {
    struct nsvm *n = v->nsvm;
    if (n == NULL) {
        return;
    }
    if (n->in_l2) {
        n->svme02 = on;
        return;
    }
    if (on == n->svme) {
        return;
    }
    n->svme = on;
    if (on) {
        atomic_fetch_add_explicit(&svm_count, 1, memory_order_acq_rel);
    } else {
        atomic_fetch_sub_explicit(&svm_count, 1, memory_order_acq_rel);
    }
    vlapic_nested_changed();
}

// VM_CR.SVMDIS makes EFER.SVME reserved (APM 24593 rev. 3.46, 15.30.1).
bool nsvm_svme_allowed(struct vcpu *v) {
    return v->nsvm != NULL && (v->nsvm->vm_cr & VM_CR_SVMDIS) == 0;
}

bool nsvm_any_svm(void) {
    return atomic_load_explicit(&svm_count, memory_order_acquire) != 0;
}

bool nsvm_in_l2(struct vcpu *v) {
    return v->nsvm != NULL && v->nsvm->in_l2;
}

// The processor's revision, its ASIDs but the guest's own, nested paging, and
// the processor's NRIP saving.
struct cpuid_regs nsvm_cpuid(void) {
    uint32_t edx = SVM_FEATURE_NP | (svm_nrips ? SVM_FEATURE_NRIPS : 0);
    return (struct cpuid_regs){ cpuid(0x8000000a, 0).eax & 0xff, nasid - 1, 0, edx };
}

uint64_t nsvm_tsc_offset(struct vcpu *v) {
    struct nsvm *n = v->nsvm;
    return v->tsc_offset + (n != NULL && n->in_l2 ? n->tsc_offset12 : 0);
}

bool nsvm_rdmsr(struct vcpu *v, uint32_t msr, uint64_t *value, bool *fault) {
    struct nsvm *n = v->nsvm;
    *fault = false;
    if (n == NULL) {
        return false;
    }
    if (msr == MSR_VM_CR) {
        *value = n->vm_cr;
        return true;
    }
    if (msr == MSR_VM_HSAVE_PA) {
        *value = n->hsave;
        return true;
    }
    return false;
}

// APM 24593 rev. 3.46, 15.30.1 and 15.30.4. Nothing reads the host save area,
// which the processor may leave unused.
bool nsvm_wrmsr(struct vcpu *v, uint32_t msr, uint64_t value, bool *fault) {
    struct nsvm *n = v->nsvm;
    *fault = false;
    if (n == NULL) {
        return false;
    }
    if (msr == MSR_VM_CR) {
        if ((value & ~(uint64_t)VM_CR_DEFINED) != 0
         || ((value & VM_CR_SVMDIS) != 0 && nsvm_svme(v))) {
            *fault = true;
            return true;
        }
        uint64_t writable = VM_CR_DEFINED;
        if ((n->vm_cr & VM_CR_LOCK) != 0) {
            writable &= ~(uint64_t)(VM_CR_LOCK | VM_CR_SVMDIS);
        }
        n->vm_cr = (n->vm_cr & ~writable) | (value & writable);
        return true;
    }
    if (msr == MSR_VM_HSAVE_PA) {
        *fault = !valid_page(value);
        if (!*fault) {
            n->hsave = value;
        }
        return true;
    }
    return false;
}

void nsvm_set_pat(struct vcpu *v, uint64_t pat) {
    struct nsvm *n = v->nsvm;
    v->vmcb->save.g_pat = pat;
    if (n != NULL && n->in_l2 && !n->npt) {
        n->vmcb01->save.g_pat = pat;
    }
}

// The state VMLOAD and VMSAVE move (APM 24593 rev. 3.46, 15.5.2), which the
// guest and its guest share.
static void copy_vmload_state(struct vmcb_save *to, const struct vmcb_save *from) {
    to->fs = from->fs;
    to->gs = from->gs;
    to->tr = from->tr;
    to->ldtr = from->ldtr;
    to->kernel_gs_base = from->kernel_gs_base;
    to->star = from->star;
    to->lstar = from->lstar;
    to->cstar = from->cstar;
    to->sfmask = from->sfmask;
    to->sysenter_cs = from->sysenter_cs;
    to->sysenter_esp = from->sysenter_esp;
    to->sysenter_eip = from->sysenter_eip;
}

// What else the guest and its guest share, as the processor keeps it but for
// VMCB12's controls, which the guest is not offered yet: the last branch
// records, and FRED's state.
static void copy_shared_state(struct vmcb *to, const struct vmcb *from) {
    copy_vmload_state(&to->save, &from->save);
    to->save.dbgctl = from->save.dbgctl;
    to->save.br_from = from->save.br_from;
    to->save.br_to = from->save.br_to;
    to->save.last_excp_from = from->save.last_excp_from;
    to->save.last_excp_to = from->save.last_excp_to;
    if (vcpu_fred()) {
        memcpy(
            (uint8_t *)to + VMCB_FRED_STATE, (const uint8_t *)from + VMCB_FRED_STATE,
            VMCB_FRED_SIZE
        );
    }
}

// A flush the VMCB left still owes is one of every ASID at the run of the
// other, which comes first.
static void switch_vmcb(struct vcpu *v, struct nsvm *n, bool to_l2) {
    struct vmcb *from = v->vmcb;
    if (to_l2) {
        n->seen01 = v->vmcb_seen;
        v->vmcb = n->vmcb02;
        v->vmcb_pa = n->vmcb02_pa;
        v->vmcb_seen = n->seen02;
    } else {
        n->seen02 = v->vmcb_seen;
        v->vmcb = n->vmcb01;
        v->vmcb_pa = n->vmcb01_pa;
        v->vmcb_seen = n->seen01;
    }
    if (from->control.tlb_control != 0) {
        v->vmcb->control.tlb_control = SVM_TLB_FLUSH_ALL;
        from->control.tlb_control = 0;
    }
    n->in_l2 = to_l2;
}

// After a #VMEXIT, or a VMRUN that failed, the guest goes on after its VMRUN
// with its global interrupt flag clear and its breakpoints disabled, and with
// the single-step trap of its own TF (APM 24593 rev. 3.46, 15.5.1 and 15.6).
static void resume_l1(struct vcpu *v, struct nsvm *n) {
    struct vmcb *vmcb01 = n->vmcb01;
    v->gprs.rax = vmcb01->save.rax;
    vmcb01->save.dr7 = 0x400;
    vmcb01->save.cpl = 0;
    vmcb01->control.interrupt_shadow = 0;
    set_gif(v, n, false);
    n->pending = false;
    n->run_pending = false;
    n->window = false;
    if (vcpu_guest_tf(v)) {
        vcpu_step_trap(v);
    }
}

// The #VMEXIT of the guest's guest, to the guest (APM 24593 rev. 3.46, 15.6;
// APM 24594 rev. 3.39, VMRUN): VMCB12 takes VMCB02's state and the exit's, and
// the guest goes on with what VMCB01 held.
static void exit_to_l1(
    struct vcpu *v, struct nsvm *n, uint64_t code, uint64_t info1, uint64_t info2,
    uint64_t intinfo, bool hardware
) {
    struct vmcb *vmcb02 = n->vmcb02;
    struct vmcb *vmcb12 = (struct vmcb *)guest_page(n->vmcb12);
    if (vmcb12 == NULL) {
        svm_shutdown(v, "The guest's VMCB is gone");
    }
    struct vmcb_save *s02 = &vmcb02->save;
    struct vmcb_save *s12 = &vmcb12->save;
    s12->es = s02->es;
    s12->cs = s02->cs;
    s12->ss = s02->ss;
    s12->ds = s02->ds;
    s12->gdtr = s02->gdtr;
    s12->idtr = s02->idtr;
    s12->efer = (s02->efer & ~(uint64_t)EFER_SVME) | (n->svme02 ? EFER_SVME : 0);
    s12->cr0 = s02->cr0;
    s12->cr2 = s02->cr2;
    s12->cr3 = s02->cr3;
    s12->cr4 = s02->cr4;
    s12->rflags = s02->rflags;
    s12->rip = s02->rip;
    s12->rsp = s02->rsp;
    s12->rax = v->gprs.rax;
    if (shadow_stacks()) {
        s12->s_cet = s02->s_cet;
        s12->ssp = s02->ssp;
        s12->isst_addr = s02->isst_addr;
    }
    s12->dr7 = s02->dr7;
    s12->dr6 = s02->dr6;
    s12->cpl = s02->cpl;
    if (n->npt) {
        s12->g_pat = s02->g_pat;
    }

    // Of the virtual interrupt control, the processor writes back V_TPR and
    // V_IRQ alone.
    struct vmcb_control *c02 = &vmcb02->control;
    struct vmcb_control *c12 = &vmcb12->control;
    uint64_t virq = n->window ? n->vintr12 : c02->vintr;
    c12->vintr = (c12->vintr & ~(SVM_VINTR_TPR_MASK | SVM_VINTR_IRQ))
               | (c02->vintr & SVM_VINTR_TPR_MASK) | (virq & SVM_VINTR_IRQ);
    c12->interrupt_shadow = (c12->interrupt_shadow & ~SVM_INTERRUPT_SHADOW)
                          | (c02->interrupt_shadow & SVM_INTERRUPT_SHADOW);
    c12->exitcode = code;
    c12->exitinfo1 = info1;
    c12->exitinfo2 = info2;
    c12->exitintinfo = intinfo;
    c12->eventinj = 0;
    // The next RIP of an exit the processor made, which it saves for the
    // instructions it intercepts, and 0 for the hypervisor's own exits (APM
    // 24593 rev. 3.46, 15.7.1).
    if (svm_nrips) {
        c12->nrip = hardware ? c02->nrip : 0;
    }

    // What the guest's guest left of NMIs: blocked as the processor still has
    // them, and one injected but not taken still pending.
    struct vmcb *vmcb01 = n->vmcb01;
    if (svm_vnmi) {
        if ((c02->vintr & SVM_VINTR_NMI) != 0) {
            v->nmi_pending = true;
        }
        vmcb01->control.vintr = (vmcb01->control.vintr & ~SVM_VINTR_NMI_MASK)
                              | (c02->vintr & SVM_VINTR_NMI_MASK);
    }
    copy_shared_state(vmcb01, vmcb02);
    if (svm_eraps) {
        vmcb01->control.rap_control |= SVM_RAP_CLEAR;
    }
    switch_vmcb(v, n, false);
    // The events of the guest's guest are not the guest's.
    v->redeliver = 0;
    v->injecting = false;
    resume_l1(v, n);
}

// The exit a failed VMRUN makes, before the guest's guest ran: VMCB12 takes
// the exit alone (APM 24593 rev. 3.46, 15.5.1).
static void vmrun_failed(struct vcpu *v, struct nsvm *n, struct vmcb *vmcb12) {
    vmcb12->control.exitcode = SVM_EXIT_INVALID;
    vmcb12->control.exitinfo1 = 0;
    vmcb12->control.exitinfo2 = 0;
    vmcb12->control.exitintinfo = 0;
    vmcb12->control.eventinj = 0;
    resume_l1(v, n);
}

// The vectors an exception injected with TYPE 3 may have: those of the
// exceptions the guest's processor raises, as Linux 7.3's KVM finds them
// (nested.c, nested_svm_event_inj_valid_exept()), #CP with shadow stacks, and
// the guest not having SEV-ES (APM 24593 rev. 3.46, 15.20).
#define INJECTABLE_EXCEPTIONS ((uint32_t)0x500f7dfb)
#define VECTOR_CP 21

static bool injection_valid(uint64_t event) {
    if ((event & SVM_EVENT_VALID) == 0) {
        return true;
    }
    uint64_t type = event & SVM_EVENT_TYPE_MASK;
    unsigned int vector = event & SVM_EVENT_VECTOR_MASK;
    if (type == SVM_EVENT_TYPE_EXCEPTION) {
        if (vector == VECTOR_CP) {
            return (cpuid_cr4_allowed() & CR4_CET) != 0;
        }
        return vector < 32 && ((INJECTABLE_EXCEPTIONS >> vector) & 1) != 0;
    }
    return type == SVM_EVENT_TYPE_INTR || type == EVENT_TYPE_NMI || type == EVENT_TYPE_SOFT_INT;
}

// A permission map's last byte is below the maximum physical address, which is
// the last one MAXPHYADDR has (APM 24593 rev. 3.46, 15.10.1 and 15.11). Each
// entry names one of the types 0, 1, 4, 5, 6, or 7 (APM 24593 rev. 3.46,
// 7.8.1).
static bool pat_valid(uint64_t pat) {
    for (size_t i = 0; i < 8; i++) {
        uint8_t type = pat >> (i * 8);
        if (type > 7 || type == 2 || type == 3) {
            return false;
        }
    }
    return true;
}

static bool table_fits(uint64_t base, size_t size) {
    return ((base & ~(uint64_t)0xfff) + size) >> phys_width == 0;
}

// VMRUN's checks of the state it loads (APM 24593 rev. 3.46, 15.5.1, and 15.20
// for the event it injects), as Linux 7.3's KVM makes them where the manual
// leaves them open (nested.c, nested_vmcb_check_controls() and
// nested_vmcb_check_save()).
static bool vmcb12_valid(struct vcpu *v, const struct vmcb *vmcb12) {
    const struct vmcb_control *c = &vmcb12->control;
    const struct vmcb_save *s = &vmcb12->save;
    if ((c->intercept_misc2 & SVM_INTERCEPT_VMRUN) == 0 || c->guest_asid == 0
     || c->guest_asid >= nasid - 1 || !table_fits(c->msrpm_base_pa, MSRPM_SIZE)
     || !table_fits(c->iopm_base_pa, IOPM_USED) || !injection_valid(c->eventinj)) {
        return false;
    }
    // S_CET has bits 1:0 alone, and shadow stacks need CR0.WP and, for user
    // ones, no virtual-8086 mode (APM 24593 rev. 3.46, 15.5.1 and 18.12).
    if ((shadow_stacks() && (s->s_cet & ~(uint64_t)3) != 0)
     || ((s->cr4 & CR4_CET) != 0
      && ((s->cr0 & CR0_WP) == 0
       || ((s->rflags & RFLAGS_VM) != 0 && (rdmsr(MSR_IA32_U_CET) & 1) != 0)))) {
        return false;
    }
    if ((s->efer & EFER_SVME) == 0 || ((s->cr0 & CR0_CD) == 0 && (s->cr0 & CR0_NW) != 0)
     || (s->cr0 >> 32) != 0 || (s->dr6 >> 32) != 0 || (s->dr7 >> 32) != 0
     || (s->cr4 & ~cpuid_cr4_allowed()) != 0 || (s->efer & ~cpuid_efer_allowed(v)) != 0) {
        return false;
    }
    // Nested paging takes the guest's paging, and a valid nCR3 and guest PAT
    // (APM 24593 rev. 3.46, 15.25.3 and 15.25.4).
    bool npt = (c->np_enable & SVM_NP_ENABLE) != 0;
    if (npt
     && ((v->vmcb->save.cr0 & CR0_PG) == 0 || (c->n_cr3 >> phys_width) != 0
      || !pat_valid(s->g_pat))) {
        return false;
    }
    if ((s->efer & EFER_LME) != 0 && (s->cr0 & CR0_PG) != 0) {
        if ((s->cr4 & CR4_PAE) == 0 || (s->cr0 & CR0_PE) == 0 || (s->cr3 >> phys_width) != 0
         || ((s->cs.attrib & SEG_L) != 0 && (s->cs.attrib & SEG_DB) != 0)) {
            return false;
        }
        return true;
    }
    // Without nested paging, VMRUN loads PAE paging's four PDPEs, which may
    // not set reserved bits (APM 24593 rev. 3.46, 15.5.1).
    if (!npt && (s->cr0 & CR0_PG) != 0 && (s->cr4 & CR4_PAE) != 0) {
        uint64_t pdpte[4];
        if (!guest_phys_read(s->cr3 & 0xffffffe0, pdpte, sizeof(pdpte))) {
            return false;
        }
        for (size_t i = 0; i < 4; i++) {
            if (!guest_pdpte_valid(pdpte[i])) {
                return false;
            }
        }
    }
    return true;
}

// VMCB02's intercepts: VMCB12's, and the hypervisor's but for the windows it
// opens for the guest itself, the hypercalls of the legacy BIOS, which are #UD
// and VMMCALL, and the guest's guest's CR8, which is its own where VMCB12
// masks interrupts virtually and is the guest's TPR otherwise (APM 24593 rev.
// 3.46, 15.21.2).
static void merge_intercepts(struct nsvm *n) {
    uint32_t own[SVM_INTERCEPT_VECTORS];
    uint32_t to[SVM_INTERCEPT_VECTORS];
    memcpy(own, &n->vmcb01->control, sizeof(own));
    for (size_t i = 0; i < SVM_INTERCEPT_VECTORS; i++) {
        to[i] = own[i] | n->intercepts[i];
    }
    to[2] = (own[2] & ~((uint32_t)1 << VECTOR_UD)) | n->intercepts[2];
    to[3] = (own[3] & ~SVM_INTERCEPT_VINTR) | n->intercepts[3];
    to[4] = (own[4] & ~SVM_INTERCEPT_VMMCALL) | n->intercepts[4];
    if ((n->vintr12 & SVM_VINTR_MASKING) == 0) {
        to[0] |= SVM_INTERCEPT_CR8_READ | SVM_INTERCEPT_CR8_WRITE;
    }
    memcpy(&n->vmcb02->control, to, sizeof(to));
}

// VMCB02's MSR permission map: an access exits where the hypervisor's or
// VMCB12's map has it exit.
static uintptr_t merge_msrpm(struct nsvm *n, uint64_t base12) {
    uintptr_t own = n->vmcb01->control.msrpm_base_pa;
    if ((n->intercepts[3] & SVM_INTERCEPT_MSR) == 0) {
        return own;
    }
    for (size_t page = 0; page < MSRPM_SIZE / PAGE_SIZE; page++) {
        const uint64_t *mine = (const uint64_t *)(own + page * PAGE_SIZE);
        const uint64_t *l1 = (const uint64_t *)guest_page(
            (base12 & ~(uint64_t)0xfff) + page * PAGE_SIZE
        );
        uint64_t *to = (uint64_t *)(n->msrpm02 + page * PAGE_SIZE);
        for (size_t i = 0; i < PAGE_SIZE / 8; i++) {
            to[i] = mine[i] | (l1 == NULL ? ~(uint64_t)0 : l1[i]);
        }
    }
    return (uintptr_t)n->msrpm02;
}

// An entry of the guest's nested page tables, which reads as all ones where
// nothing decodes, its reserved bits set.
static uint64_t npt12_read(const struct nsvm *n, uintptr_t gpa, void **at) {
    uint8_t *page = guest_page(gpa & ~(uint64_t)0xfff);
    *at = NULL;
    if (page == NULL) {
        return ~(uint64_t)0;
    }
    if (n->npt_wide) {
        *at = page + (gpa & 0xff8);
        return atomic_load_explicit((_Atomic(uint64_t) *)*at, memory_order_relaxed);
    }
    *at = page + (gpa & 0xffc);
    return atomic_load_explicit((_Atomic(uint32_t) *)*at, memory_order_relaxed);
}

// Sets bits in an entry the walk found as it was: false where the entry
// changed meanwhile, for the walk to start again.
static bool npt12_mark(const struct nsvm *n, void *at, uint64_t was, uint64_t set) {
    if (n->npt_wide) {
        uint64_t expected = was;
        return atomic_compare_exchange_strong_explicit(
            (_Atomic(uint64_t) *)at, &expected, was | set, memory_order_seq_cst,
            memory_order_seq_cst
        );
    }
    uint32_t expected = (uint32_t)was;
    return atomic_compare_exchange_strong_explicit(
        (_Atomic(uint32_t) *)at, &expected, (uint32_t)(was | set), memory_order_seq_cst,
        memory_order_seq_cst
    );
}

// The walk of the guest's nested page tables for an access of its guest's (APM
// 24593 rev. 3.46, 15.25.5, 15.25.7, and 15.25.8), as the guest's own page
// tables are walked in its paging mode at VMRUN, every access a user one: 0
// where the walk maps the access, which sets its accessed flags and, for a
// write, its dirty flag, else the nested page fault's error code.
static uint64_t npt12_walk(
    struct nsvm *n, uintptr_t gpa, bool write, bool fetch, struct npt12_walk *w
) {
    bool wide = n->npt_wide;
    // The fetch flag is defined only with SMEP or with PAE paging's NX (APM
    // 24593 rev. 3.46, 8.4.2), as for the guest's page faults (guest_mem.c).
    bool fetch_flag = fetch && (n->npt_smep || (wide && n->npt_nx));
    uint64_t error = NPF_US | (write ? NPF_RW : 0) | (fetch_flag ? NPF_ID : 0);
retry:;
    uintptr_t table;
    // The PDPT of PAE paging, which nested paging reads as it walks (APM 24593
    // rev. 3.46, 15.25.11), at nCR3 bits 31:5 as Linux 7.3's KVM finds
    // (nested.c, nested_svm_get_tdp_pdptr()).
    if (n->npt_pae) {
        void *at;
        if ((gpa >> 32) != 0) {
            return error;
        }
        uint64_t pdpte = npt12_read(n, (n->ncr3 & ~(uint64_t)0x1f) + ((gpa >> 30) & 3) * 8, &at);
        if ((pdpte & NPT_P) == 0) {
            return error;
        }
        if (!guest_pdpte_valid(pdpte)) {
            return error | NPF_P | NPF_RSV;
        }
        table = pdpte & NPT_ADDRESS;
    } else if (!wide) {
        if ((gpa >> 32) != 0) {
            return error;
        }
        table = n->ncr3 & 0xfffff000;
    } else {
        if ((gpa >> (12 + 9 * n->npt_levels)) != 0) {
            return error;
        }
        table = n->ncr3 & NPT_ADDRESS;
    }
    void *used[5];
    uint64_t entries[5];
    bool writable = true;
    bool user = true;
    bool executable = true;
    for (size_t level = n->npt_levels; level-- > 0; ) {
        unsigned int shift = wide ? 12 + 9 * level : 12 + 10 * level;
        size_t index = (gpa >> shift) & (wide ? 511 : 1023);
        uint64_t e = npt12_read(n, table + index * (wide ? 8 : 4), &used[level]);
        if ((e & NPT_P) == 0) {
            return error;
        }
        bool large = level > 0 && (e & NPT_PS) != 0 && (wide || n->npt_pse);
        if ((e & guest_pte_reserved(wide, n->npt_pae, level, large, n->npt_nx)) != 0) {
            return error | NPF_P | NPF_RSV;
        }
        entries[level] = e;
        writable = writable && (e & NPT_RW) != 0;
        user = user && (e & NPT_US) != 0;
        executable = executable && (!wide || !n->npt_nx || (e & NPT_NX) == 0);
        if (level > 0 && !large) {
            table = wide ? e & NPT_ADDRESS : e & 0xfffff000;
            continue;
        }
        if (!user || (write && !writable) || (fetch && !executable)) {
            return error | NPF_P;
        }
        for (size_t l = n->npt_levels; l-- > level; ) {
            uint64_t set = NPT_A | (l == level && write ? NPT_D : 0);
            if ((entries[l] & set) != set && !npt12_mark(n, used[l], entries[l], set)) {
                goto retry;
            }
        }
        uint64_t page_mask = ((uint64_t)1 << shift) - 1;
        if (wide) {
            w->gpa = (e & NPT_ADDRESS & ~page_mask) | (gpa & page_mask);
        } else if (large) {
            // PSE-36 puts physical address bits 39:32 in PDE bits 20:13.
            w->gpa = (e & 0xffc00000) | ((e >> 13) & 0xff) << 32 | (gpa & page_mask);
        } else {
            w->gpa = (e & 0xfffff000) | (gpa & page_mask);
        }
        w->level = level;
        w->leaf = e | NPT_A | (write ? NPT_D : 0);
        w->writable = writable;
        w->executable = executable;
        return 0;
    }
    return error;
}

static size_t level_size(size_t level) {
    return (uint64_t)1 << (12 + 9 * level);
}

static void npt02_free(struct nsvm *n, uint64_t *table, size_t level) {
    for (size_t i = 0; level > 0 && i < 512; i++) {
        uint64_t e = table[i];
        if ((e & NPT_P) != 0 && (e & NPT_PS) == 0) {
            npt02_free(n, (uint64_t *)(uintptr_t)(e & NPT_ADDRESS), level - 1);
        }
    }
    page_free(table, 1);
    n->npt02_pages--;
}

// The processor may hold translations from the tables until a flush of every
// ASID, which comes before the guest's guest runs again.
static void npt02_drop_all(struct nsvm *n) {
    for (size_t i = 0; i < NPT02_ROOTS; i++) {
        if (n->npt02[i].root != NULL) {
            npt02_free(n, n->npt02[i].root, 3);
            n->npt02[i].root = NULL;
        }
    }
    n->current02 = NULL;
    n->vmcb02->control.tlb_control = SVM_TLB_FLUSH_ALL;
}

static uint64_t *npt02_table(struct nsvm *n) {
    n->npt02_pages++;
    return page_alloc(1);
}

// The root of the shadow of VMCB12's nested page tables for its ASID, of the
// hypervisor's 4-level nested paging.
static uintptr_t npt02_root(struct nsvm *n) {
    uint32_t asid = n->copy.control.guest_asid;
    struct npt02 *r = NULL;
    for (size_t i = 0; i < NPT02_ROOTS && r == NULL; i++) {
        struct npt02 *at = &n->npt02[i];
        if (at->root != NULL && at->ncr3 == n->ncr3 && at->asid == asid) {
            r = at;
        }
    }
    for (size_t i = 0; i < NPT02_ROOTS && r == NULL; i++) {
        if (n->npt02[i].root == NULL) {
            r = &n->npt02[i];
        }
    }
    if (r == NULL) {
        r = &n->npt02[n->npt02_next];
        n->npt02_next = (n->npt02_next + 1) % NPT02_ROOTS;
        npt02_free(n, r->root, 3);
        r->root = NULL;
        n->vmcb02->control.tlb_control = SVM_TLB_FLUSH_ALL;
    }
    if (r->root == NULL) {
        r->ncr3 = n->ncr3;
        r->asid = asid;
        r->root = npt02_table(n);
    }
    n->current02 = r;
    return (uintptr_t)r->root;
}

// The memory type of a leaf of the guest's nested page tables, through its PAT
// (APM 24593 rev. 3.46, 15.25.9).
static uint8_t npt12_memtype(const struct nsvm *n, const struct npt12_walk *w) {
    uint64_t pat = w->level > 0 ? NPT_PAT_LARGE : NPT_PAT_4K;
    size_t index = ((w->leaf & pat) != 0 ? 4 : 0) | ((w->leaf & NPT_PCD) != 0 ? 2 : 0)
                 | ((w->leaf & NPT_PWT) != 0 ? 1 : 0);
    return (uint8_t)((n->npt_pat >> (index * 8)) & 7);
}

// An NPT02 leaf for the guest's mapping and the hypervisor's. Writes wait for
// the guest's dirty flag, which the leaf then takes as set until the guest
// flushes its guest's translations, as a processor's TLB may (APM 24593 rev.
// 3.46, 15.16). Where the hypervisor's memory type is not write-back, it wins
// but over uncacheable.
static uint64_t npt02_leaf(
    const struct nsvm *n, const struct npt12_walk *w, uintptr_t hpa, uint32_t prot,
    uint8_t memtype, size_t level
) {
    uint8_t type = npt12_memtype(n, w);
    if (memtype != MT_WB && type != MT_UC) {
        type = memtype;
    }
    bool writable = w->writable && (prot & GPT_W) != 0 && (w->leaf & NPT_D) != 0;
    uint64_t e = gpt_npt_leaf(
        hpa & ~(level_size(level) - 1), writable ? GPT_RWX : GPT_R | GPT_X, type, level
    );
    e |= NPT_A | (writable ? NPT_D : 0);
    if (!w->executable) {
        e |= NPT_NX;
    }
    return e;
}

// Maps an address of the guest's guest in NPT02 as the walk of the guest's
// nested page tables found it and the hypervisor's have the guest's memory,
// but for the page behind the local APICs' pages, which the hypervisor's
// emulation gives a meaning. Returns the leaf it made, or 0.
static uint64_t npt02_fill(struct nsvm *n, uintptr_t gpa, const struct npt12_walk *w) {
    uintptr_t hpa;
    uint32_t prot;
    uint8_t memtype;
    size_t level;
    if (!gpt_translate_leaf(w->gpa, &hpa, &prot, &memtype, &level)
     || vlapic_access_page(hpa & ~(uint64_t)0xfff)) {
        return 0;
    }
    // 32-bit paging's 4 MiB pages take 2 MiB ones.
    if (level > w->level) {
        level = w->level;
    }
    if (level > npt02_level_max) {
        level = npt02_level_max;
    }
    if (n->npt02_pages + 3 > npt02_budget) {
        npt02_drop_all(n);
        n->vmcb02->control.n_cr3 = npt02_root(n);
    }
    uint64_t *table = n->current02->root;
    for (size_t l = 3; l > level; l--) {
        uint64_t *slot = &table[(gpa >> (12 + 9 * l)) & 511];
        if ((*slot & NPT_P) == 0 || (*slot & NPT_PS) != 0) {
            *slot = (uint64_t)(uintptr_t)npt02_table(n) | NPT_P | NPT_RW | NPT_US | NPT_A;
        }
        table = (uint64_t *)(uintptr_t)(*slot & NPT_ADDRESS);
    }
    uint64_t *slot = &table[(gpa >> (12 + 9 * level)) & 511];
    if ((*slot & NPT_P) != 0 && (*slot & NPT_PS) == 0 && level > 0) {
        npt02_free(n, (uint64_t *)(uintptr_t)(*slot & NPT_ADDRESS), level - 1);
    }
    *slot = npt02_leaf(n, w, hpa, prot, memtype, level);
    return *slot;
}

static bool npt02_allows(uint64_t leaf, bool write, bool fetch) {
    return (leaf & NPT_P) != 0 && (!write || (leaf & NPT_RW) != 0)
        && (!fetch || (leaf & NPT_NX) == 0);
}

// VMCB02 from VMCB12 and the hypervisor's own controls in VMCB01 (APM 24594
// rev. 3.39, VMRUN): the guest's guest runs on the hypervisor's nested page
// tables, in the processor's ASID one past VMCB12's.
static void prepare_vmcb02(struct nsvm *n) {
    const struct vmcb_control *c12 = &n->copy.control;
    const struct vmcb_save *s12 = &n->copy.save;
    const struct vmcb *vmcb01 = n->vmcb01;
    struct vmcb *vmcb02 = n->vmcb02;
    struct vmcb_control *c = &vmcb02->control;
    struct vmcb_save *s = &vmcb02->save;

    memcpy(n->intercepts, c12, sizeof(n->intercepts));
    // Nothing of vector 5 but the INVLPGB and INVPCID intercepts yet.
    n->intercepts[5] &= SVM_INTERCEPT_INVLPGB | SVM_INTERCEPT_INVPCID;
    n->vintr12 = c12->vintr;
    n->window = false;
    n->tsc_offset12 = c12->tsc_offset;
    merge_intercepts(n);

    c->pause_filter_threshold = 0;
    c->pause_filter_count = 0;
    c->iopm_base_pa = (uintptr_t)iopm02;
    c->msrpm_base_pa = merge_msrpm(n, c12->msrpm_base_pa);
    c->guest_asid = c12->guest_asid + 1;
    // A flush of VMCB12's ASID alone, which the guest is not offered yet, is
    // one of every ASID, as bit 0 makes it where TLB_CONTROL was a flag, and
    // one VMCB02 still owes stays.
    if ((c12->tlb_control & 1) != 0) {
        c->tlb_control = SVM_TLB_FLUSH_ALL;
    }
    n->npt = (c12->np_enable & SVM_NP_ENABLE) != 0;
    // The predictor's entries from the guest are the guest's guest's to have
    // cleared, as Linux 7.3's KVM has it (nested.c,
    // nested_vmcb02_prepare_control()).
    c->rap_control = svm_eraps ? (c12->rap_control & SVM_RAP_ALLOW_LARGER) | SVM_RAP_CLEAR : 0;
    c->vintr = (c12->vintr & (SVM_VINTR_TPR_MASK | VIRQ_BITS))
             | (vmcb01->control.vintr
              & (SVM_VINTR_MASKING | SVM_VINTR_GIF_ENABLE | SVM_VINTR_NMI_ENABLE
               | SVM_VINTR_NMI_MASK));
    c->interrupt_shadow = c12->interrupt_shadow & SVM_INTERRUPT_SHADOW;
    c->exitcode = 0;
    c->exitinfo1 = 0;
    c->exitinfo2 = 0;
    c->exitintinfo = 0;
    c->np_enable = vmcb01->control.np_enable;
    c->n_cr3 = gpt_root();
    if (n->npt) {
        const struct vmcb_save *s01 = &vmcb01->save;
        n->ncr3 = c12->n_cr3;
        n->npt_wide = (s01->efer & EFER_LMA) != 0 || (s01->cr4 & CR4_PAE) != 0;
        n->npt_pae = (s01->efer & EFER_LMA) == 0 && (s01->cr4 & CR4_PAE) != 0;
        n->npt_levels = (s01->efer & EFER_LMA) == 0 ? 2 : (s01->cr4 & CR4_LA57) != 0 ? 5 : 4;
        n->npt_nx = (s01->efer & EFER_NXE) != 0;
        n->npt_pse = (s01->cr4 & CR4_PSE) != 0;
        n->npt_smep = (s01->cr4 & CR4_SMEP) != 0;
        n->npt_pat = s01->g_pat;
        // Its flushes drop the nested translations too.
        if (c->tlb_control != 0) {
            npt02_drop_all(n);
        }
        c->n_cr3 = npt02_root(n);
    }
    c->eventinj = c12->eventinj & ~EVENT_RESERVED;
    // Software interrupts injected take their return address from here (APM
    // 24593 rev. 3.46, 15.20): VMCB12's where the guest has NRIP saving, else
    // its RIP, which the guest moved past the instruction itself.
    c->nrip = svm_nrips ? c12->nrip : s12->rip;
    c->virt_ext = vmcb01->control.virt_ext;
    c->exitintdata = 0;
    c->eventinjdata = 0;

    s->es = s12->es;
    s->cs = s12->cs;
    s->ss = s12->ss;
    s->ds = s12->ds;
    s->gdtr = s12->gdtr;
    s->idtr = s12->idtr;
    s->cpl = s12->cpl;
    s->efer = s12->efer | EFER_SVME;
    n->svme02 = (s12->efer & EFER_SVME) != 0;
    s->cr0 = s12->cr0;
    s->cr2 = s12->cr2;
    s->cr3 = s12->cr3;
    s->cr4 = s12->cr4;
    s->dr6 = s12->dr6;
    s->dr7 = s12->dr7;
    // The guest's guest's shadow stacks, which VMRUN loads (APM 24593 rev.
    // 3.46, 15.15.3).
    bool cet = shadow_stacks();
    s->s_cet = cet ? s12->s_cet : 0;
    s->ssp = cet ? s12->ssp : 0;
    s->isst_addr = cet ? s12->isst_addr : 0;
    s->rflags = s12->rflags;
    s->rip = s12->rip;
    s->rsp = s12->rsp;
    s->rax = s12->rax;
    // Without nested paging of its own, the guest's guest has the guest's PAT.
    s->g_pat = n->npt ? s12->g_pat : vmcb01->save.g_pat;
    copy_shared_state(vmcb02, vmcb01);
}

// The address in rAX, as wide as the instruction's addresses.
static uint64_t rax_address(struct vcpu *v, const struct insn *insn) {
    uint64_t rax = v->gprs.rax;
    if (insn->address_size < 8) {
        rax &= insn->address_size == 4 ? 0xffffffff : 0xffff;
    }
    return rax;
}

// The faults the SVM instructions raise ahead of their intercepts (APM 24594
// rev. 3.39, VMRUN): #UD without EFER.SVME or outside protected mode, #GP
// above CPL 0.
static bool allowed(struct vcpu *v) {
    struct vmcb_save *s = &v->vmcb->save;
    if (!nsvm_svme(v) || (s->cr0 & CR0_PE) == 0 || (s->rflags & RFLAGS_VM) != 0) {
        vcpu_inject_exception(v, VECTOR_UD, false, 0);
        return false;
    }
    if (s->cpl != 0) {
        vcpu_inject_exception(v, VECTOR_GP, true, 0);
        return false;
    }
    return true;
}

// The instruction at hand, which runs again where the hypervisor cannot read
// it, as where another processor just remapped it.
static bool decode(struct vcpu *v, struct insn *insn) {
    if (!insn_decode(v, insn) || insn->opcode != (INSN_OPCODE_0F | 0x01)) {
        printf("Cannot decode the SVM instruction at %#llx\n", (unsigned long long)vcpu_rip(v));
        return false;
    }
    return true;
}

// VMRUN (APM 24593 rev. 3.46, 15.5.1; APM 24594 rev. 3.39, VMRUN). VMCB01
// keeps the guest's state, which VMRUN would save, with RIP past the VMRUN,
// where the guest goes on at the #VMEXIT.
static void vmrun(struct vcpu *v, struct nsvm *n) {
    struct insn insn;
    if (!allowed(v) || !decode(v, &insn)) {
        return;
    }
    uint64_t gpa = rax_address(v, &insn);
    struct vmcb *vmcb12 = valid_page(gpa) ? (struct vmcb *)guest_page(gpa) : NULL;
    if (vmcb12 == NULL) {
        vcpu_inject_exception(v, VECTOR_GP, true, 0);
        return;
    }
    struct vmcb *vmcb01 = n->vmcb01;
    uint64_t rip = vmcb01->save.rip;
    uint64_t next = svm_nrips ? vmcb01->control.nrip : rip + insn.length;
    if ((next >> 32) != (rip >> 32)
     && ((vmcb01->save.efer & EFER_LMA) == 0 || (vmcb01->save.cs.attrib & SEG_L) == 0)) {
        next = (uint32_t)next;
    }
    vmcb01->save.rip = next;
    vmcb01->save.rflags &= ~(uint64_t)RFLAGS_RF;
    vmcb01->save.rax = v->gprs.rax;
    n->vmcb12 = gpa;
    // A copy, which the guest's other processors cannot change under the
    // checks.
    memcpy(&n->copy, vmcb12, sizeof(struct vmcb));
    if (!vmcb12_valid(v, &n->copy)) {
        vmrun_failed(v, n, vmcb12);
        return;
    }
    // A virtual NMI for the guest that it did not take yet is the guest's
    // guest's to take or exit on.
    if (svm_vnmi && (vmcb01->control.vintr & SVM_VINTR_NMI) != 0) {
        vmcb01->control.vintr &= ~SVM_VINTR_NMI;
        v->nmi_pending = true;
    }
    prepare_vmcb02(n);
    v->gprs.rax = n->copy.save.rax;
    switch_vmcb(v, n, true);
    n->ran = true;
    n->run_pending = true;
    set_gif(v, n, true);
}

// VMLOAD and VMSAVE (APM 24593 rev. 3.46, 15.5.2), from the guest or a guest
// of its that runs them, with addresses in the guest's physical memory.
static void vmload_vmsave(struct vcpu *v, bool load) {
    struct insn insn;
    if (!allowed(v) || !decode(v, &insn)) {
        return;
    }
    uint64_t gpa = rax_address(v, &insn);
    struct vmcb *vmcb = valid_page(gpa) ? (struct vmcb *)guest_page(gpa) : NULL;
    if (vmcb == NULL) {
        vcpu_inject_exception(v, VECTOR_GP, true, 0);
        return;
    }
    if (load) {
        copy_vmload_state(&v->vmcb->save, &vmcb->save);
    } else {
        copy_vmload_state(&vmcb->save, &v->vmcb->save);
    }
    vcpu_advance(v, insn.length);
}

// INVLPGA (APM 24593 rev. 3.46, 15.16.2) in the processor's ASID for ECX's,
// one past it, the guest's own for 0; ASIDs past those the guest has hold no
// translations.
static void invlpga(struct vcpu *v) {
    struct insn insn;
    if (!allowed(v) || !decode(v, &insn)) {
        return;
    }
    uint32_t asid = (uint32_t)v->gprs.rcx;
    if (asid < nasid - 1) {
        uint64_t address = rax_address(v, &insn);
        uint32_t own = asid == 0 ? 1 : asid + 1;
        asm volatile ("invlpga %0, %1" :: "a"(address), "c"(own) : "memory");
    }
    vcpu_advance(v, insn.length);
}

// An ASID needs EFER.SVME, and one the processor has (APM 24594 rev. 3.39,
// INVLPGB).
uint32_t nsvm_invlpgb_asids(struct vcpu *v) {
    return nsvm_svme(v) ? nasid - 1 : 1;
}

// A guest's INVLPGB is for its own ASID (APM 24594 rev. 3.39, INVLPGB), so is
// that of a guest without SVM, and the guest's own ASIDs are one past those it
// names, its 0 being its own.
bool nsvm_invlpgb_asid(struct vcpu *v, bool named, uint32_t asid, uint32_t *processor) {
    struct nsvm *n = v->nsvm;
    *processor = v->vmcb->control.guest_asid;
    if (n == NULL || n->in_l2 || !n->svme) {
        return true;
    }
    if (asid != 0) {
        *processor = asid + 1;
    }
    return named;
}

void nsvm_instruction(struct vcpu *v, uint64_t code) {
    struct nsvm *n = v->nsvm;
    // Without SVM for the guest, they are all #UD, and so is SKINIT, which the
    // hypervisor cannot run for the guest, where the guest's own EFER.SVME
    // enables it whatever CPUID says (APM 24594 rev. 3.39, SKINIT).
    if (n == NULL || code == SVM_EXIT_SKINIT) {
        vcpu_inject_exception(v, VECTOR_UD, false, 0);
        return;
    }
    switch (code) {
        case SVM_EXIT_VMRUN: {
            vmrun(v, n);
            break;
        }
        case SVM_EXIT_VMLOAD:
        case SVM_EXIT_VMSAVE: {
            vmload_vmsave(v, code == SVM_EXIT_VMLOAD);
            break;
        }
        case SVM_EXIT_STGI:
        case SVM_EXIT_CLGI: {
            if (allowed(v)) {
                set_gif(v, n, code == SVM_EXIT_STGI);
                vcpu_skip_instruction(v);
            }
            break;
        }
        default: {
            invlpga(v);
            break;
        }
    }
}

// Whether VMCB12's I/O permission map has a bit set for a port of the access
// (APM 24593 rev. 3.46, 15.10.1), past FFFFh too for one that wraps.
static bool io_wanted(struct nsvm *n, uint64_t info) {
    if ((n->intercepts[3] & SVM_INTERCEPT_IOIO) == 0) {
        return false;
    }
    size_t size = (info >> 4) & 7;
    size = size == 4 ? 4 : size == 2 ? 2 : 1;
    uintptr_t base = n->copy.control.iopm_base_pa & ~(uint64_t)0xfff;
    for (size_t port = info >> 16; port < (info >> 16) + size; port++) {
        const uint8_t *page = guest_page(base + (port / 8 & ~(size_t)0xfff));
        if (page == NULL || ((page[port / 8 & 0xfff] >> (port % 8)) & 1) != 0) {
            return true;
        }
    }
    return false;
}

// Whether VMCB12's MSR permission map has the access exit, as accesses to MSRs
// past its ranges always do (APM 24593 rev. 3.46, 15.11).
static bool msr_wanted(struct vcpu *v, struct nsvm *n, bool write) {
    if ((n->intercepts[3] & SVM_INTERCEPT_MSR) == 0) {
        return false;
    }
    uint32_t msr = (uint32_t)v->gprs.rcx;
    size_t offset;
    if (msr <= 0x1fff) {
        offset = 0;
    } else if (msr >= 0xc0000000 && msr <= 0xc0001fff) {
        offset = 0x800;
    } else if (msr >= 0xc0010000 && msr <= 0xc0011fff) {
        offset = 0x1000;
    } else {
        return true;
    }
    size_t bit = offset * 8 + (msr & 0x1fff) * 2 + (write ? 1 : 0);
    uintptr_t base = n->copy.control.msrpm_base_pa & ~(uint64_t)0xfff;
    const uint8_t *page = guest_page(base + (bit / 8 & ~(size_t)0xfff));
    return page == NULL || ((page[bit / 8 & 0xfff] >> (bit % 8)) & 1) != 0;
}

// Whether the guest's guest takes the virtual interrupt VMCB12 gives it now
// (APM 24593 rev. 3.46, 15.21.4).
static bool virq_due(struct vcpu *v, uint64_t vintr) {
    struct vmcb *vmcb = v->vmcb;
    unsigned int priority = (vintr & SVM_VINTR_PRIO_MASK) >> SVM_VINTR_PRIO_SHIFT;
    unsigned int tpr = vmcb->control.vintr & SVM_VINTR_TPR_MASK;
    return (vintr & SVM_VINTR_IRQ) != 0 && (vmcb->save.rflags & RFLAGS_IF) != 0
        && ((vintr & SVM_VINTR_IGN_TPR) != 0 || priority > tpr);
}

// The guest's guest's MOV from and to CR8, where VMCB12 has it the guest's TPR
// and the guest does not intercept it: V_TPR follows the writes too (APM 24593
// rev. 3.46, 15.21.2). Decode assists give the register; else the instruction
// does, 0F 20 /r or 0F 22 /r with REX.R or LOCK.
static void cr8_access(struct vcpu *v, bool write) {
    struct insn insn;
    if (!insn_decode(v, &insn)) {
        printf("Cannot decode the CR8 access at %#llx\n", (unsigned long long)vcpu_rip(v));
        return;
    }
    unsigned int reg = (insn.modrm & 7) | (insn.rex & 1) << 3 | (insn.rex_upper & 1) << 4;
    if (write) {
        uint64_t value = vcpu_gpr(v, reg);
        if (value >> 4 != 0) {
            vcpu_inject_exception(v, VECTOR_GP, true, 0);
            return;
        }
        vlapic_set_cr8(v, (uint32_t)value);
        v->vmcb->control.vintr = (v->vmcb->control.vintr & ~SVM_VINTR_TPR_MASK) | value;
    } else {
        vcpu_set_gpr(v, reg, vlapic_cr8(v));
    }
    vcpu_advance(v, insn.length);
}

// Whether an exit of the guest's guest is the guest's: those the hypervisor's
// own intercepts make but for VMCB12's, interrupts, NMIs, nested page faults,
// and what the hypervisor's windows and the guest's guest's CR8 make for the
// hypervisor (Linux 7.3's KVM, nested.c, nested_svm_exit_special() and
// nested_svm_intercept()).
static bool l1_wants(struct vcpu *v, struct nsvm *n, uint64_t code) {
    struct vmcb_control *c = &v->vmcb->control;
    switch (code) {
        case SVM_EXIT_INTR:
        case SVM_EXIT_NMI:
        case SVM_EXIT_NPF: {
            return false;
        }
        case SVM_EXIT_INVALID: {
            return true;
        }
        case SVM_EXIT_IOIO: {
            return io_wanted(n, c->exitinfo1);
        }
        case SVM_EXIT_MSR: {
            return msr_wanted(v, n, (c->exitinfo1 & 1) != 0);
        }
        // The guest's own virtual interrupt, but where the hypervisor's window
        // has its place, for the entry to see whether the guest's interrupts
        // go first (APM 24593 rev. 3.46, 15.21.4).
        case SVM_EXIT_VINTR: {
            return (n->intercepts[3] & SVM_INTERCEPT_VINTR) != 0 && !n->window;
        }
        default: {
            break;
        }
    }
    if (code >= SVM_EXIT_EXCEPTION_BASE && code < SVM_EXIT_EXCEPTION_BASE + 32) {
        uint32_t own = n->vmcb01->control.intercept_exceptions & ~((uint32_t)1 << VECTOR_UD);
        return ((own >> (code - SVM_EXIT_EXCEPTION_BASE)) & 1) == 0;
    }
    if (code >= 32 * SVM_INTERCEPT_VECTORS) {
        return false;
    }
    return ((n->intercepts[code / 32] >> (code % 32)) & 1) != 0;
}

// A nested page fault of NPT02: the guest's where its nested page tables do
// not allow the access, else mapped as the hypervisor's nested page tables
// have the guest's memory, or emulated where that is the hypervisor's own
// device. The guest's guest's page table walks are writes (APM 24593 rev.
// 3.46, 15.25.5).
static void npt_fault(struct vcpu *v, struct nsvm *n) {
    struct vmcb_control *c = &v->vmcb->control;
    uintptr_t gpa = c->exitinfo2;
    uint64_t info = c->exitinfo1;
    bool walk = (info & NPF_TABLE) != 0;
    bool write = walk || (info & NPF_RW) != 0;
    bool fetch = !walk && (info & NPF_ID) != 0;
    struct npt12_walk w;
    uint64_t error = npt12_walk(n, gpa, write, fetch, &w);
    if (error != 0) {
        error |= info & (NPF_FINAL | NPF_TABLE);
        exit_to_l1(v, n, SVM_EXIT_NPF, error, gpa, c->exitintinfo, true);
        return;
    }
    if (npt02_allows(npt02_fill(n, gpa, &w), write, fetch)) {
        return;
    }
    // An event the hypervisor cannot deliver itself ends the guest's guest as
    // a shutdown would.
    if (event_cut_short(v)) {
        if (event_deliver_real_mode(v)) {
            return;
        }
        if ((n->intercepts[3] & SVM_INTERCEPT_SHUTDOWN) == 0) {
            svm_shutdown(v, "The guest's guest's event cannot be delivered");
        }
        exit_to_l1(v, n, SVM_EXIT_SHUTDOWN, 0, 0, 0, false);
        return;
    }
    if (fetch) {
        mmio_fetch(v, w.gpa);
    } else {
        mmio_fault(v, w.gpa);
    }
}

bool nsvm_npt_active(struct vcpu *v) {
    return v->nsvm != NULL && v->nsvm->in_l2 && v->nsvm->npt;
}

bool nsvm_npt_translate(
    struct vcpu *v, uintptr_t gpa, bool write, bool fetch, bool walk, uint64_t *out
) {
    struct nsvm *n = v->nsvm;
    if (n->pending) {
        return false;
    }
    struct npt12_walk w;
    uint64_t error = npt12_walk(n, gpa, write || walk, fetch && !walk, &w);
    if (error == 0) {
        *out = w.gpa;
        return true;
    }
    n->pending = true;
    n->pending_code = SVM_EXIT_NPF;
    n->pending_info1 = error | (walk ? NPF_TABLE : NPF_FINAL);
    n->pending_info2 = gpa;
    return false;
}

bool nsvm_l2_exit(struct vcpu *v) {
    struct nsvm *n = v->nsvm;
    if (n == NULL || !n->in_l2) {
        return false;
    }
    struct vmcb_control *c = &v->vmcb->control;
    uint64_t code = c->exitcode;
    n->run_pending = false;
    if (code == SVM_EXIT_NPF && n->npt) {
        npt_fault(v, n);
        return true;
    }
    // Where the guest's guest's own EFER.SVME is clear, the SVM instructions
    // raise #UD before their intercepts, but VMMCALL, whose intercept comes
    // first (APM 24593 rev. 3.46, Table 15-7); the guest may intercept #UD
    // instead.
    if (!n->svme02 && (code == SVM_EXIT_VMRUN || code == SVM_EXIT_INVLPGA
                    || (code >= SVM_EXIT_VMLOAD && code <= SVM_EXIT_SKINIT))) {
        vcpu_inject_exception(v, VECTOR_UD, false, 0);
        return true;
    }
    if (l1_wants(v, n, code)) {
        exit_to_l1(v, n, code, c->exitinfo1, c->exitinfo2, c->exitintinfo, true);
        return true;
    }
    if (code == SVM_EXIT_CR8_READ || code == SVM_EXIT_CR8_WRITE) {
        cr8_access(v, code == SVM_EXIT_CR8_WRITE);
        return true;
    }
    return false;
}

bool nsvm_exception(
    struct vcpu *v, uint8_t vector, bool has_error, uint32_t error, uint64_t info2
) {
    struct nsvm *n = v->nsvm;
    if (n == NULL || !n->in_l2) {
        return false;
    }
    if (n->pending) {
        return true;
    }
    if (((n->intercepts[2] >> vector) & 1) == 0) {
        return false;
    }
    // EXITINFO1 has the error code, and EXITINFO2 a page fault's address, the
    // rest being undefined (APM 24593 rev. 3.46, 15.12).
    n->pending = true;
    n->pending_code = SVM_EXIT_EXCEPTION_BASE + vector;
    n->pending_info1 = has_error ? error : 0;
    n->pending_info2 = vector == VECTOR_PF ? info2 : 0;
    return true;
}

// The guest's guest has no instruction that raises again a software event that
// VMRUN injected, while it is still at VMRUN's RIP: the event goes in again
// with VMRUN's next RIP, which the exit reset (APM 24593 rev. 3.46, 15.7.1),
// as Linux 7.3's KVM has it (svm.c, svm_complete_soft_interrupt()).
bool nsvm_inject_again(struct vcpu *v, uint64_t event) {
    struct nsvm *n = v->nsvm;
    const uint64_t mask = SVM_EVENT_VALID | SVM_EVENT_TYPE_MASK | SVM_EVENT_VECTOR_MASK;
    if (n == NULL || !n->in_l2 || (event & mask) != (n->copy.control.eventinj & mask)
     || v->vmcb->save.rip != n->copy.save.rip) {
        return false;
    }
    v->vmcb->control.nrip = svm_nrips ? n->copy.control.nrip : n->copy.save.rip;
    return true;
}

bool nsvm_exit_pending(struct vcpu *v) {
    return v->nsvm != NULL && v->nsvm->pending;
}

// Whether the guest's interrupts can reach its guest: the guest's RFLAGS.IF
// masks them where VMCB12 masks interrupts virtually, the guest's guest's
// otherwise (APM 24593 rev. 3.46, 15.21.1).
static bool l1_interrupts_open(struct vcpu *v, struct nsvm *n) {
    const struct vmcb *vmcb = (n->vintr12 & SVM_VINTR_MASKING) != 0 ? n->vmcb01 : v->vmcb;
    return (vmcb->save.rflags & RFLAGS_IF) != 0;
}

bool nsvm_interruptible(struct vcpu *v) {
    struct nsvm *n = v->nsvm;
    bool shadow = (v->vmcb->control.interrupt_shadow & SVM_INTERRUPT_SHADOW) != 0;
    if (n == NULL || !n->in_l2) {
        return nsvm_gif(v) && (v->vmcb->save.rflags & RFLAGS_IF) != 0 && !shadow;
    }
    return n->gif && l1_interrupts_open(v, n) && !shadow;
}

void nsvm_interrupt_window(struct vcpu *v, bool on) {
    struct nsvm *n = v->nsvm;
    struct vmcb_control *c = &v->vmcb->control;
    // None opens in the guest's guest while its global interrupt flag holds
    // the guest's interrupts back, which the STGI that sets it, intercepted,
    // ends, or while the guest's IF does where it masks them, as the guest's
    // guest cannot change it, as Linux 7.3's KVM finds (nested.c,
    // nested_vmcb02_recalc_intercepts()).
    if (!n->gif
     || ((n->vintr12 & SVM_VINTR_MASKING) != 0 && (n->vmcb01->save.rflags & RFLAGS_IF) == 0)) {
        on = false;
    }
    if (!n->window) {
        n->vintr12 = (n->vintr12 & ~VIRQ_BITS) | (c->vintr & VIRQ_BITS);
    }
    c->vintr &= ~VIRQ_BITS;
    c->intercept_misc1 &= ~SVM_INTERCEPT_VINTR;
    if (on) {
        c->vintr |= SVM_VINTR_IRQ | SVM_VINTR_IGN_TPR;
        c->intercept_misc1 |= SVM_INTERCEPT_VINTR;
    } else {
        c->vintr |= n->vintr12 & VIRQ_BITS;
        c->intercept_misc1 |= n->intercepts[3] & SVM_INTERCEPT_VINTR;
    }
    n->window = on;
}

bool nsvm_halt_ends(struct vcpu *v) {
    struct nsvm *n = v->nsvm;
    struct cpu *cpu = v->cpu;
    if (atomic_load_explicit(&cpu->init_pending, memory_order_acquire) || v->nmi_pending
     || atomic_load_explicit(&cpu->nmi_pending, memory_order_acquire)) {
        return true;
    }
    uint64_t vintr = v->vmcb->control.vintr;
    if (svm_vnmi && (vintr & (SVM_VINTR_NMI | SVM_VINTR_NMI_MASK)) == SVM_VINTR_NMI) {
        return true;
    }
    if ((irq_extint_pending(v) || vlapic_pending(v) >= 0) && l1_interrupts_open(v, n)) {
        return true;
    }
    return virq_due(v, n->window ? n->vintr12 : vintr);
}

bool nsvm_holds_init(struct vcpu *v) {
    struct nsvm *n = v->nsvm;
    return n != NULL && (!n->gif || n->in_l2);
}

bool nsvm_init_signal(struct vcpu *v) {
    struct nsvm *n = v->nsvm;
    if (n == NULL) {
        return false;
    }
    if ((n->vm_cr & VM_CR_R_INIT) != 0) {
        vcpu_inject_exception(v, VECTOR_SX, true, SX_INIT);
        return true;
    }
    // The guest's guest is gone, its state with it, and so is SVM, which
    // INIT's clearing of EFER disables (APM 24593 rev. 3.46, 15.30.1).
    if (n->in_l2) {
        switch_vmcb(v, n, false);
        v->gprs.rax = n->vmcb01->save.rax;
    }
    n->pending = false;
    n->run_pending = false;
    n->window = false;
    nsvm_set_svme(v, false);
    if ((n->vm_cr & VM_CR_LOCK) == 0) {
        n->vm_cr &= ~(uint64_t)VM_CR_SVMDIS;
    }
    return false;
}

// The event whose delivery the guest's guest was cut short in, for EXITINTINFO
// (APM 24593 rev. 3.46, 15.7.2).
static uint64_t cut_short(struct vcpu *v) {
    if ((v->redeliver & SVM_EVENT_VALID) == 0) {
        return 0;
    }
    return v->redeliver | (uint64_t)v->redeliver_error << 32;
}

// The guest's INIT, NMIs, and interrupts are exits of its guest's where VMCB12
// intercepts them and its global interrupt flag lets them through: INIT and
// NMIs stay pending for the guest, interrupts in its local APIC (APM 24593
// rev. 3.46, 15.13, 15.21.8, and 15.21.9). They wait for an instruction
// boundary, past the delivery of what VMRUN injects, what an exit cut short,
// or an exception the hypervisor raised, as on hardware and in Linux 7.3's
// KVM: an interrupt of the hypervisor's own then ends the entry. Else the
// guest's guest goes on, taking what the guest does not intercept.
bool nsvm_before_entry(struct vcpu *v) {
    struct nsvm *n = v->nsvm;
    if (n == NULL || !n->in_l2) {
        return false;
    }
    if (n->pending) {
        n->pending = false;
        exit_to_l1(v, n, n->pending_code, n->pending_info1, n->pending_info2, cut_short(v), false);
        return false;
    }
    bool waiting = n->run_pending || v->injecting || (v->redeliver & SVM_EVENT_VALID) != 0;
    uint32_t misc = n->intercepts[3];
    struct cpu *cpu = v->cpu;
    if (atomic_exchange_explicit(&cpu->nmi_pending, false, memory_order_acquire)) {
        v->nmi_pending = true;
    }
    bool window = false;
    if (n->gif && atomic_load_explicit(&cpu->init_pending, memory_order_acquire)) {
        if (waiting) {
            vcpu_exit_soon(v);
        } else if ((misc & SVM_INTERCEPT_INIT) != 0) {
            exit_to_l1(v, n, SVM_EXIT_INIT, 0, 0, 0, false);
            return false;
        } else {
            atomic_store_explicit(&cpu->init_pending, false, memory_order_release);
            vcpu_init_signal(v);
            // A redirected one is #SX, which VMCB12 may intercept.
            return nsvm_before_entry(v);
        }
    }
    if (n->gif && v->nmi_pending && (misc & SVM_INTERCEPT_NMI) != 0) {
        if (waiting) {
            vcpu_exit_soon(v);
        } else {
            exit_to_l1(v, n, SVM_EXIT_NMI, 0, 0, 0, false);
            return false;
        }
    }
    if (n->gif && (misc & SVM_INTERCEPT_INTR) != 0
     && (irq_extint_pending(v) || vlapic_pending(v) >= 0)) {
        // The interrupt shadow holds back interrupts, not the exits they make.
        if (!l1_interrupts_open(v, n)) {
            window = (n->vintr12 & SVM_VINTR_MASKING) == 0;
        } else if (waiting) {
            vcpu_exit_soon(v);
        } else {
            exit_to_l1(v, n, SVM_EXIT_INTR, 0, 0, 0, false);
            return false;
        }
    }
    nsvm_interrupt_window(v, false);
    // A stepped instruction is to run, not an event's handler.
    if (v->stepping) {
        event_hold(v);
        return true;
    }
    event_entry_for(
        v, n->gif && (misc & SVM_INTERCEPT_NMI) == 0, n->gif && (misc & SVM_INTERCEPT_INTR) == 0
    );
    if (window) {
        nsvm_interrupt_window(v, true);
    }
    return true;
}

void nsvm_gpt_changed(struct vcpu *v) {
    struct nsvm *n = v->nsvm;
    if (n == NULL) {
        return;
    }
    n->vmcb01->control.n_cr3 = gpt_root();
    if (n->ran) {
        v->vmcb->control.tlb_control = SVM_TLB_FLUSH_ALL;
    }
    npt02_drop_all(n);
    if (n->in_l2 && n->npt) {
        n->vmcb02->control.n_cr3 = npt02_root(n);
    }
}
