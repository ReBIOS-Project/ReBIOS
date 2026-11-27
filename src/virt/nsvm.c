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
// You should have received a copy of the GNU General Public License along with
// ReBIOS. If not, see <https://www.gnu.org/licenses/>.

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <virt/nsvm.h>
#include <core.h>
#include <handoff.h>
#include <hw/lapic.h>
#include <lib/heap.h>
#include <lib/log.h>
#include <lib/spinlock.h>
#include <lib/util.h>
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
#include <virt/msr.h>
#include <virt/pmu.h>
#include <virt/svm.h>
#include <virt/vcpu.h>
#include <virt/virt.h>
#include <virt/vlapic.h>

// The error code of the #SX that a redirected INIT raises (APM 24593 rev.
// 3.46, 15.28).
#define SX_INIT 1

// The bits of VM_CR the guest has (APM 24593 rev. 3.46, 15.30.1).
#define VM_CR_DEFINED \
    (VM_CR_DPD | VM_CR_R_INIT | VM_CR_DIS_A20M | VM_CR_LOCK | VM_CR_SVMDIS)

// TSC_RATIO's 1.0 in its 8.32 format, and its bits below the reserved bits
// 63:40 (APM 24593 rev. 3.46, 15.30.5).
#define TSC_RATIO_ONE ((uint64_t)1 << 32)
#define TSC_RATIO_DEFINED (((uint64_t)1 << 40) - 1)

// The bits EVENTINJ reserves, which VMRUN ignores, as Linux 7.3's KVM has it
// (nested.c, __nested_copy_vmcb_control_to_cache()).
#define EVENT_RESERVED ((uint64_t)0x7ffff000)

// The bits of FRED_CONFIG that VMRUN refuses (AMD 69191 rev. 1.10).
#define FRED_CONFIG_RESERVED ((uint64_t)0x834)

// The virtual interrupt the guest gives its guest, which the hypervisor's own
// windows take the place of (APM 24593 rev. 3.46, 15.21.4).
#define VIRQ_BITS \
    (SVM_VINTR_IRQ | SVM_VINTR_PRIO_MASK | SVM_VINTR_IGN_TPR | SVM_VINTR_VECTOR_MASK)

#define IOPM_SIZE (SVM_IOPM_PAGES * PAGE_SIZE)
#define MSRPM_SIZE (SVM_MSRPM_PAGES * PAGE_SIZE)
// The bytes of the I/O permission map the processor reads: a bit for each
// port, and those for accesses past port FFFFh (APM 24593 rev. 3.46, 15.10.1).
#define IOPM_USED (2 * PAGE_SIZE + 1)

// What the walk of the guest's nested page tables gives where page
// modification logging has no room for the page an access would dirty, and
// what marks its faults, whose error code may be 0, as that of a supervisor
// read of a page that is not present under GMET is: bits no error code has.
#define NPT12_PML_FULL ((uint64_t)1 << 63)
#define NPT12_FAULT ((uint64_t)1 << 62)
// Page modification logging, in CPUID Fn8000_000A_ECX, and the last index of
// its log (APM 24593 rev. 3.46, 15.25.16).
#define CPUID_8000000A_ECX_PML ((uint32_t)1 << 4)
#define PML_LAST 0x1ff

// x2AVIC's 4096 IDs, in CPUID Fn8000_000A_ECX, and the last index of the
// physical APIC ID table in xAVIC mode, in x2AVIC mode, and with 4096 IDs (APM
// 24593 rev. 3.46, 15.29.4.3); and the shadow table's pages for those.
#define CPUID_8000000A_ECX_X2AVIC_EXT ((uint32_t)1 << 6)
#define AVIC_LAST 0xff
#define X2AVIC_LAST 0x1ff
#define X2AVIC_EXT_LAST 0xfff
#define AVIC_TABLE_PAGES ((X2AVIC_EXT_LAST + 1) / (PAGE_SIZE / 8))

// GAAT, Paging-Write, and Verify Paging-Write, in CPUID Fn8000_000A_ECX, the
// maximum GAAT prefix width in its EAX, the bits of GAAT's pointer that must
// be zero, and Paging-Write's and Verify Paging-Write's bits in the nested
// page tables' leaves (APM 24593 rev. 3.46, 15.40, 15.25.6.2, and 15.25.6.3).
#define CPUID_8000000A_ECX_GAAT ((uint32_t)1 << 0)
#define CPUID_8000000A_EAX_GAAT_PREFIX ((uint32_t)0x3f00)
#define GAAT_POINTER_RESERVED ((uint64_t)0xfff0000000000000)
#define CPUID_8000000A_ECX_PW ((uint32_t)1 << 1)
#define CPUID_8000000A_ECX_VPW ((uint32_t)1 << 2)
// Guest PMC event filtering (APM 24593 rev. 3.46, 15.39.1).
#define CPUID_8000000A_ECX_PMC_FILTER ((uint32_t)1 << 3)
#define NPT_VPW ((uint64_t)1 << 57)
#define NPT_PW ((uint64_t)1 << 58)

#define NPT02_ROOTS 4

#define CPUID_7_0_ECX_BUSLOCKTRAP ((uint32_t)1 << 24)
#define CPUID_80000008_EBX_INVLPGB ((uint32_t)1 << 3)
#define CPUID_80000008_EBX_INVLPGB_NESTED ((uint32_t)1 << 21)

#define MSR_IA32_SPEC_CTRL 0x48

// A shadow of one of the guest's nested page tables, for one ASID of its
// guests: what it and the hypervisor's make of the guest's guest's addresses,
// filled in as the processor misses them.
struct npt02 {
    uint64_t ncr3;
    uint32_t asid;
    // Whether VMCB12 has GMET, which the leaves' U/S follows.
    bool gmet;
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
    bool user;
    // Whether it is a supervisor shadow-stack page that a supervisor
    // shadow-stack access reached, and whether paging writes may write it.
    bool sss;
    bool pw;
};

// SVM as the guest gets it.
struct nsvm {
    // EFER.SVME of the guest, and of its guest while that runs, which the
    // processor's EFER always has set for VMRUN.
    bool svme;
    bool svme02;
    uint64_t vm_cr;
    // The key that clears VM_CR.LOCK (APM 24593 rev. 3.46, 15.31.1).
    uint64_t svm_key;
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
    // The guest's TSC_RATIO, which scales its guest's TSC, and the one the
    // processor has.
    uint64_t tsc_ratio;
    uint64_t tsc_ratio_loaded;
    // Whether VMCB12 has the processor switch the last branch records.
    bool lbrv;
    // The processor's IA32_SPEC_CTRL host register while the guest's guest
    // runs with the guest's ORed in, and whether that changed it.
    uint64_t host_spec_ctrl;
    bool spec_ctrl_raised;
    // Whether VMCB12 has the processor virtualise the guest's guest's NMIs,
    // global interrupt flag, and VMLOAD and VMSAVE (APM 24593 rev. 3.46,
    // 15.21.10 and 15.33).
    bool vnmi12;
    bool vgif12;
    bool vls12;
    // Whether VMCB12 has the processor switch the guest's guest's FRED MSRs
    // (AMD 69191 rev. 1.10).
    bool fred12;
    // Whether VMCB12 has the processor virtualise VMLOAD and VMSAVE where its
    // checks refuse to, which has them exit (APM 24593 rev. 3.46, 15.33.1).
    bool vls12_refused;
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
    // Whether the guest's guest's page table walks are writes at the nested
    // page tables' level only where they set accessed or dirty flags, as
    // VMCB12 may have it (APM 24593 rev. 3.46, 15.25.6.1).
    bool rogpt;
    // Whether VMCB12 has the guest's guest's supervisor fetches from user
    // pages of the nested page tables fault (APM 24593 rev. 3.46, 15.25.14).
    bool gmet;
    // Whether VMCB12 has the guest's guest's page table walks write as paging
    // writes, and has the pages that its nested page tables mark be reached
    // through guest page tables that paging writes may write (APM 24593 rev.
    // 3.46, 15.25.6.2 and 15.25.6.3).
    bool pw;
    bool vpw;
    // Whether VMCB12 has GAAT paging translate the guest's guest's linear
    // addresses (APM 24593 rev. 3.46, 15.40).
    bool gaat;
    // Whether the guest's guest may be running on NPT02, and the flush of the
    // guest's nested translations that NPT02 has seen and, where an INVLPGB of
    // the guest's asked for one, the flush that its TLBSYNC waits for.
    _Atomic(bool) l2_live;
    _Atomic(uint64_t) nested_flush_seen;
    uint64_t nested_flush_owed;
    // Whether VMCB12 gives the guest's guest PMC registers of its own, and
    // filters the events they may count (APM 24593 rev. 3.46, 15.39).
    bool pmc;
    bool pmc_filter;
    // Whether VMCB12 has the guest's guest's supervisor shadow-stack accesses
    // reach only supervisor shadow-stack pages of the nested page tables,
    // which the guest's PAE paging and EFER.NXE allow (APM 24593 rev. 3.46,
    // 15.25.15).
    bool sss;
    // Whether VMCB12 has the guest's guest's writes that set dirty flags log
    // their pages, and where.
    bool pml;
    uint64_t pml_base;
    uint16_t pml_index;
    struct npt02 npt02[NPT02_ROOTS];
    struct npt02 *current02;
    size_t npt02_pages;
    size_t npt02_next;
    // A bit for each of the guest's guests' ASIDs whose translations the
    // guest's INVLPGA flushed since VMCB02 last had the ASID.
    uint64_t *invlpga_asids;
    // Whether VMCB12 has AVIC, and x2AVIC, run the guest's guest's local APIC
    // (APM 24593 rev. 3.46, 15.29); VMCB02's shadow of the guest's physical
    // APIC ID table then, the guest's backing page for its guest, the entry of
    // the shadow that names that page, else -1, and whether the guest's guest
    // runs with it, for the guest's doorbells; all under avic_lock.
    bool avic12;
    bool x2avic12;
    // The page of the guest's guest's that NPT02 maps to avic_apic_page, else
    // 1, which no page is.
    uint64_t avic_apic02;
    struct avic_table *avic_table;
    uint64_t avic_backing12;
    int32_t avic_own;
    _Atomic(bool) avic_running;
    // An exit of the guest's guest that what the hypervisor carried out for it
    // raised, for the next entry.
    bool pending;
    uint64_t pending_code;
    uint64_t pending_info1;
    uint64_t pending_info2;
};

static void avic_leave(struct nsvm *n);
static bool avic_doorbell(uint64_t value);

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
// The DebugCtl bits a guest's guest's last branch records may have: LBR, BTF,
// and BLCKDB with bus lock traps (APM 24593 rev. 3.46, 13.1.1.6).
static uint64_t debugctl_bits;
// Whether the processor has read-only guest page tables, page modification
// logging, GMET, the TSC ratio MSR, the idle HLT intercept, and the bus lock
// threshold, SSSCheck, which takes the host's EFER.NXE (APM 24593 rev. 3.46,
// 15.25.15), Paging-Write, Verify Paging-Write, GAAT, PMC virtualisation, and
// guest PMC event filtering.
static bool rogpt;
static bool pml;
static bool gmet;
static bool ssscheck;
static bool paging_write;
static bool verify_paging_write;
static bool gaat;
// INVLPGB's nested translations, where the processor has them and TlbiCtl, for
// the TLBSYNC intercept, and the flushes of the guest's that its INVLPGBs
// asked for.
static bool invlpgb_nested;
static _Atomic(uint64_t) nested_flushes;
static bool pmc_virt;
static bool pmc_filter;
static bool tsc_scaling;
static bool idle_hlt;
static bool bus_lock_threshold;
// The processor's AVIC, x2AVIC, and x2AVIC's 4096 IDs (APM 24594 rev. 3.39,
// E.4.9).
static bool avic;
static bool x2avic;
static bool x2avic_ext;
// What NPT02 maps the guest's guest's local APIC page to under xAVIC where the
// guest's nested page tables map it to no RAM: any page does, as AVIC takes
// the accesses to the backing page instead (APM 24593 rev. 3.46, 15.29.2).
static uint8_t *avic_apic_page;

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
    rogpt = (cpuid(0x8000000a, 0).edx & SVM_FEATURE_ROGPT) != 0;
    gmet = (cpuid(0x8000000a, 0).edx & SVM_FEATURE_GMET) != 0;
    ssscheck = (cpuid(0x8000000a, 0).edx & SVM_FEATURE_SSSCHECK) != 0
            && (rdmsr(MSR_EFER) & EFER_NXE) != 0;
    tsc_scaling = (cpuid(0x8000000a, 0).edx & SVM_FEATURE_TSC_RATIO) != 0;
    idle_hlt = (cpuid(0x8000000a, 0).edx & SVM_FEATURE_IDLE_HLT) != 0;
    bus_lock_threshold = (cpuid(0x8000000a, 0).edx & SVM_FEATURE_BUS_LOCK_THRESHOLD) != 0;
    pml = (cpuid(0x8000000a, 0).ecx & CPUID_8000000A_ECX_PML) != 0;
    paging_write = (cpuid(0x8000000a, 0).ecx & CPUID_8000000A_ECX_PW) != 0;
    verify_paging_write = (cpuid(0x8000000a, 0).ecx & CPUID_8000000A_ECX_VPW) != 0;
    gaat = (cpuid(0x8000000a, 0).ecx & CPUID_8000000A_ECX_GAAT) != 0;
    uint32_t sizes = cpuid(0x80000008, 0).ebx;
    invlpgb_nested = svm_tlbictl && (sizes & CPUID_80000008_EBX_INVLPGB) != 0
                  && (sizes & CPUID_80000008_EBX_INVLPGB_NESTED) != 0;
    pmc_virt = (cpuid(0x8000000a, 0).edx & SVM_FEATURE_PMC_VIRT) != 0;
    pmc_filter = pmc_virt && (cpuid(0x8000000a, 0).ecx & CPUID_8000000A_ECX_PMC_FILTER) != 0;
    avic = (cpuid(0x8000000a, 0).edx & SVM_FEATURE_AVIC) != 0;
    x2avic = avic && (cpuid(0x8000000a, 0).edx & SVM_FEATURE_X2AVIC) != 0;
    x2avic_ext = x2avic && (cpuid(0x8000000a, 0).ecx & CPUID_8000000A_ECX_X2AVIC_EXT) != 0;
    if (avic) {
        avic_apic_page = page_alloc(1);
    }
    debugctl_bits = DEBUGCTL_LBR | DEBUGCTL_BTF
                  | ((cpuid(7, 0).ecx & CPUID_7_0_ECX_BUSLOCKTRAP) != 0 ? DEBUGCTL_BLCKDB : 0);
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
        .tsc_ratio = TSC_RATIO_ONE,
        .tsc_ratio_loaded = TSC_RATIO_ONE,
        .vm_cr = svm_firmware_vm_cr & (VM_CR_DPD | VM_CR_DIS_A20M | VM_CR_LOCK),
        .vmcb01 = v->vmcb,
        .vmcb01_pa = v->vmcb_pa
    };
    n->vmcb02 = page_alloc(1);
    n->vmcb02_pa = (uintptr_t)n->vmcb02;
    n->msrpm02 = page_alloc(MSRPM_SIZE / PAGE_SIZE);
    n->invlpga_asids = page_alloc(page_count((nasid + 63) / 64 * sizeof(uint64_t)));
    v->nsvm = n;
}

static void npt02_drop_all(struct nsvm *n);

// With SPEC_CTRL virtualisation, the guest's IA32_SPEC_CTRL is the host
// register to its guest, whose own is the guest register, in VMCB02 (APM 24593
// rev. 3.46, 15.37.1): the processor's host register has the guest's ORed in
// while the guest's guest runs.
static void spec_ctrl_switch(struct nsvm *n, bool to_l2) {
    if (to_l2) {
        n->host_spec_ctrl = rdmsr(MSR_IA32_SPEC_CTRL);
        uint64_t value = n->host_spec_ctrl | n->vmcb01->save.spec_ctrl;
        n->spec_ctrl_raised = value != n->host_spec_ctrl;
        if (n->spec_ctrl_raised) {
            wrmsr(MSR_IA32_SPEC_CTRL, value);
        }
    } else if (n->spec_ctrl_raised) {
        wrmsr(MSR_IA32_SPEC_CTRL, n->host_spec_ctrl);
        n->spec_ctrl_raised = false;
    }
}

void nsvm_vcpu_power_on(struct vcpu *v) {
    struct nsvm *n = v->nsvm;
    if (n == NULL) {
        return;
    }
    npt02_drop_all(n);
    spec_ctrl_switch(n, false);
    avic_leave(n);
    size_t asids = page_count((nasid + 63) / 64 * sizeof(uint64_t));
    memset(n->invlpga_asids, 0, asids * PAGE_SIZE);
    *n = (struct nsvm){
        .gif = true,
        .tsc_ratio = TSC_RATIO_ONE,
        .tsc_ratio_loaded = TSC_RATIO_ONE,
        .vm_cr = svm_firmware_vm_cr & (VM_CR_DPD | VM_CR_DIS_A20M | VM_CR_LOCK),
        .vmcb01 = v->vmcb,
        .vmcb01_pa = v->vmcb_pa,
        .vmcb02 = n->vmcb02,
        .vmcb02_pa = n->vmcb02_pa,
        .msrpm02 = n->msrpm02,
        .invlpga_asids = n->invlpga_asids,
        .seen01 = n->seen01,
        .seen02 = n->seen02,
        .avic_apic02 = 1,
        .avic_own = -1
    };
}

static bool valid_page(uint64_t address) {
    return (address & 0xfff) == 0 && (address >> phys_width) == 0;
}

// Whether the guest has shadow stacks, whose state VMCBs hold.
static bool shadow_stacks(void) {
    return (cpuid_cr4_allowed() & CR4_CET) != 0;
}

// The global interrupt flag, which the processor's copy of follows where it
// virtualises it: VMCB01's while VMCB02's is the guest's guest's own.
static void set_gif(struct vcpu *v, struct nsvm *n, bool on) {
    n->gif = on;
    if (svm_vgif) {
        bool l2s = n->in_l2 && n->vgif12;
        struct vmcb_control *c = l2s ? &n->vmcb01->control : &v->vmcb->control;
        c->vintr = on ? c->vintr | SVM_VINTR_GIF : c->vintr & ~SVM_VINTR_GIF;
    }
}

void nsvm_sync_gif(struct vcpu *v) {
    struct nsvm *n = v->nsvm;
    if (n != NULL && !n->in_l2 && svm_vgif) {
        n->gif = (n->vmcb01->control.vintr & SVM_VINTR_GIF) != 0;
    }
}

// While the guest's EFER.SVME is set, its VMLOAD and VMSAVE run on the
// processor where it virtualises them, and so do its CLGI and STGI where it
// virtualises the global interrupt flag (APM 24593 rev. 3.46, 15.33), but for
// STGI while the flag holds back an INIT, or an NMI that the processor does
// not hold back itself: its exit lets that in, as in Linux 7.3's KVM (svm.c,
// svm_enable_nmi_window()).
static void l1_intercepts(struct vcpu *v, struct nsvm *n) {
    const uint32_t gif = SVM_INTERCEPT_STGI | SVM_INTERCEPT_CLGI;
    const uint32_t vls = SVM_INTERCEPT_VMLOAD | SVM_INTERCEPT_VMSAVE;
    struct vmcb_control *c = &n->vmcb01->control;
    struct cpu *cpu = v->cpu;
    uint32_t misc = c->intercept_misc2 & ~(gif | vls);
    if (!n->svme || !svm_vls) {
        misc |= vls;
    }
    if (!n->svme || !svm_vgif) {
        misc |= gif;
    } else if (!n->gif
            && (atomic_load_explicit(&cpu->init_pending, memory_order_acquire)
             || (!svm_vnmi && event_nmi_waiting(v)))) {
        misc |= SVM_INTERCEPT_STGI;
    }
    c->intercept_misc2 = misc;
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

// The SVM instructions of a guest whose own EFER.SVME is clear raise #UD,
// which the processor, finding EFER.SVME set, has as #GP outside CPL 0
// (svm.c): VMCB01 intercepts #GP while the guest's is clear, and VMCB02 while
// its guest's is or the guest intercepts #GP. Without the guest VMCB address
// check, both always do, for the #GP that VMRUN, VMLOAD, and VMSAVE may raise
// ahead of their intercepts (svm.c).
static void svme_gp_intercept(struct nsvm *n) {
    uint32_t gp = (uint32_t)1 << VECTOR_GP;
    struct vmcb_control *c = n->in_l2 ? &n->vmcb02->control : &n->vmcb01->control;
    bool intercept = !svm_vmcb_addr_check
                  || (n->in_l2 ? !n->svme02 || (n->intercepts[2] & gp) != 0 : !n->svme);
    if (intercept) {
        c->intercept_exceptions |= gp;
    } else {
        c->intercept_exceptions &= ~gp;
    }
}

void nsvm_set_svme(struct vcpu *v, bool on) {
    struct nsvm *n = v->nsvm;
    if (n == NULL) {
        return;
    }
    if (n->in_l2) {
        n->svme02 = on;
        svme_gp_intercept(n);
        msr_amd_counters_mode(v);
        return;
    }
    if (on == n->svme) {
        return;
    }
    n->svme = on;
    svme_gp_intercept(n);
    msr_amd_counters_mode(v);
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

// The processor's revision, its ASIDs but the guest's own, and the features of
// its SVM that the guest's has. VMCB12 is read whole at every VMRUN, as clean
// bits allow (APM 24593 rev. 3.46, 15.15.1). The guest VMCB address check
// holds on every processor, where the hypervisor takes the #GP that stands in
// for it (svm.c).
struct cpuid_regs nsvm_cpuid(void) {
    struct cpuid_regs svm = cpuid(0x8000000a, 0);
    uint32_t features = SVM_FEATURE_NP | SVM_FEATURE_NRIPS
                      | SVM_FEATURE_PMC_VIRT | SVM_FEATURE_VMCB_CLEAN
                      | SVM_FEATURE_FLUSH_BY_ASID | SVM_FEATURE_PAUSE_FILTER
                      | SVM_FEATURE_PAUSE_FILTER_THRESHOLD | SVM_FEATURE_LBRV
                      | SVM_FEATURE_ROGPT | SVM_FEATURE_TLBICTL
                      | SVM_FEATURE_DECODE_ASSISTS | SVM_FEATURE_VGIF
                      | SVM_FEATURE_GMET | SVM_FEATURE_TSC_RATIO
                      | SVM_FEATURE_SVML | SVM_FEATURE_IDLE_HLT
                      | SVM_FEATURE_BUS_LOCK_THRESHOLD | SVM_FEATURE_HOST_MCE_OVERRIDE
                      | SVM_FEATURE_V_SPEC_CTRL | (ssscheck ? SVM_FEATURE_SSSCHECK : 0)
                      | (svm_vls ? SVM_FEATURE_VLS : 0) | (svm_vnmi ? SVM_FEATURE_VNMI : 0)
                      | SVM_FEATURE_AVIC | SVM_FEATURE_X2AVIC
                      | SVM_FEATURE_EXT_LVT_AVIC_ACCESS;
    return (struct cpuid_regs){
        svm.eax & (0xff | (gaat ? CPUID_8000000A_EAX_GAAT_PREFIX : 0)), nasid - 1,
        svm.ecx & (CPUID_8000000A_ECX_PML | CPUID_8000000A_ECX_PW
                 | CPUID_8000000A_ECX_VPW | CPUID_8000000A_ECX_GAAT
                 | CPUID_8000000A_ECX_PMC_FILTER | CPUID_8000000A_ECX_X2AVIC_EXT),
        (svm.edx & features) | SVM_FEATURE_VMCB_ADDR_CHECK
    };
}

// The guest's guest reads the guest's TSC, scaled by the guest's TSC_RATIO and
// offset by VMCB12 (APM 24593 rev. 3.46, 15.30.5), as Linux 7.3's KVM composes
// them (kvm_calc_nested_tsc_offset()).
uint64_t nsvm_tsc_offset(struct vcpu *v) {
    struct nsvm *n = v->nsvm;
    if (n == NULL || !n->in_l2) {
        return v->tsc_offset;
    }
    uint64_t offset = v->tsc_offset;
    if (n->tsc_ratio != TSC_RATIO_ONE) {
        offset = (uint64_t)(((__int128)(int64_t)offset * n->tsc_ratio) >> 32);
    }
    return offset + n->tsc_offset12;
}

uint64_t nsvm_tsc(struct vcpu *v, uint64_t raw) {
    struct nsvm *n = v->nsvm;
    if (n != NULL && n->in_l2 && n->tsc_ratio != TSC_RATIO_ONE) {
        raw = (uint64_t)(((unsigned __int128)raw * n->tsc_ratio) >> 32);
    }
    return raw + nsvm_tsc_offset(v);
}

void nsvm_load_tsc_ratio(struct vcpu *v) {
    struct nsvm *n = v->nsvm;
    if (n == NULL || !tsc_scaling) {
        return;
    }
    uint64_t ratio = n->in_l2 ? n->tsc_ratio : TSC_RATIO_ONE;
    if (ratio != n->tsc_ratio_loaded) {
        wrmsr(MSR_TSC_RATIO, ratio);
        n->tsc_ratio_loaded = ratio;
    }
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
    // The key is kept secret (APM 24593 rev. 3.46, 15.31.1).
    if (msr == MSR_SVM_KEY && svm_svml) {
        *value = 0;
        return true;
    }
    if (msr == MSR_VM_HSAVE_PA) {
        *value = n->hsave;
        return true;
    }
    if (msr == MSR_TSC_RATIO && tsc_scaling) {
        *value = n->tsc_ratio;
        return true;
    }
    // AVIC's doorbell is write-only (APM 24593 rev. 3.46, 15.29.8.2).
    if (msr == SVM_AVIC_DOORBELL && avic) {
        *fault = true;
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
    // A write sets the key while VM_CR.LOCK is clear, and else clears LOCK
    // where it matches a key other than 0 (APM 24593 rev. 3.46, 15.31.1).
    if (msr == MSR_SVM_KEY && svm_svml) {
        if ((n->vm_cr & VM_CR_LOCK) == 0) {
            n->svm_key = value;
        } else if (value == n->svm_key && value != 0) {
            n->vm_cr &= ~(uint64_t)VM_CR_LOCK;
        }
        return true;
    }
    if (msr == MSR_TSC_RATIO && tsc_scaling) {
        *fault = (value & ~TSC_RATIO_DEFINED) != 0;
        if (!*fault) {
            n->tsc_ratio = value;
        }
        return true;
    }
    if (msr == SVM_AVIC_DOORBELL && avic) {
        *fault = !avic_doorbell(value);
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

// The last branch records, which the processor switches where VMCB12 has it,
// and the guest and its guest share else (APM 24593 rev. 3.46, 15.23.1).
static void copy_lbrs(struct vmcb_save *to, const struct vmcb_save *from) {
    to->dbgctl = from->dbgctl;
    to->br_from = from->br_from;
    to->br_to = from->br_to;
    to->last_excp_from = from->last_excp_from;
    to->last_excp_to = from->last_excp_to;
}

// The PMC registers of a guest with PMC virtualisation, which VMRUN loads and
// #VMEXIT saves (APM 24593 rev. 3.46, 15.39).
static void copy_pmcs(struct vmcb_save *to, const struct vmcb_save *from) {
    memcpy(to->perf_ctl_ctr, from->perf_ctl_ctr, sizeof(to->perf_ctl_ctr));
    to->instr_retired_ctr = from->instr_retired_ctr;
    to->perf_ctr_global_sts = from->perf_ctr_global_sts;
    to->perf_ctr_global_ctl = from->perf_ctr_global_ctl;
}

static void copy_fred_state(struct vmcb_save *to, const struct vmcb_save *from) {
    for (size_t i = 0; i < 3; i++) {
        to->fred_rsp[i] = from->fred_rsp[i];
        to->fred_ssp[i] = from->fred_ssp[i];
    }
    to->fred_stklvls = from->fred_stklvls;
    to->fred_config = from->fred_config;
}

// What else the guest and its guest share, as the processor keeps it: FRED's
// state too, but where VMCB12 has the processor switch it.
static void copy_shared_state(struct vmcb *to, const struct vmcb *from, bool fred) {
    copy_vmload_state(&to->save, &from->save);
    if (vcpu_fred() && fred) {
        copy_fred_state(&to->save, &from->save);
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
    if (svm_v_spec_ctrl) {
        spec_ctrl_switch(n, to_l2);
    }
    n->in_l2 = to_l2;
    msr_amd_counters_mode(v);
}

void nsvm_vcpu_leave(struct vcpu *v) {
    struct nsvm *n = v->nsvm;
    if (n != NULL && n->in_l2) {
        switch_vmcb(v, n, false);
    }
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
    struct vcpu *v,
    struct nsvm *n,
    uint64_t code,
    uint64_t info1,
    uint64_t info2,
    uint64_t intinfo,
    bool hardware
) {
    struct vmcb *vmcb02 = n->vmcb02;
    struct vmcb *vmcb12 = (struct vmcb *)guest_mem_page(n->vmcb12);
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
    if (n->fred12) {
        copy_fred_state(s12, s02);
    }
    s12->dr7 = s02->dr7;
    s12->dr6 = s02->dr6;
    s12->cpl = s02->cpl;
    if (n->npt) {
        s12->g_pat = s02->g_pat;
    }
    if (svm_v_spec_ctrl) {
        s12->spec_ctrl = s02->spec_ctrl;
    }
    if (n->pmc) {
        copy_pmcs(s12, s02);
    }
    if (n->pml) {
        vmcb12->control.pml_index = n->pml_index;
    }

    // Of the virtual interrupt control, the processor writes back V_TPR and
    // V_IRQ, and V_GIF and the virtual NMI state where VMCB12 has it
    // virtualise them (APM 24593 rev. 3.46, 15.21.10 and 15.33.2).
    struct vmcb_control *c02 = &vmcb02->control;
    struct vmcb_control *c12 = &vmcb12->control;
    uint64_t virq = n->window ? n->vintr12 : c02->vintr;
    uint64_t back = (n->vnmi12 ? SVM_VINTR_NMI | SVM_VINTR_NMI_MASK : 0)
                  | (n->vgif12 ? SVM_VINTR_GIF : 0);
    c12->vintr = (c12->vintr & ~(SVM_VINTR_TPR_MASK | SVM_VINTR_IRQ | back))
               | (c02->vintr & (SVM_VINTR_TPR_MASK | back))
               | (virq & SVM_VINTR_IRQ);
    c12->interrupt_shadow = (c12->interrupt_shadow & ~SVM_INTERRUPT_SHADOW)
                          | (c02->interrupt_shadow & SVM_INTERRUPT_SHADOW);
    if (bus_lock_threshold) {
        c12->bus_lock_counter = c02->bus_lock_counter;
    }
    c12->exitcode = code;
    c12->exitinfo1 = info1;
    c12->exitinfo2 = info2;
    c12->exitintinfo = intinfo;
    // FRED's event data of the event cut short: the processor's for the event
    // it gives, the hypervisor's for one it was delivering itself (AMD 69191
    // rev. 1.10).
    c12->exitintdata = 0;
    if ((intinfo & SVM_EVENT_VALID) != 0) {
        c12->exitintdata = intinfo == c02->exitintinfo ? c02->exitintdata : v->redeliver_data;
    }
    c12->eventinj = 0;
    // The next RIP of an exit the processor made, which it saves for the
    // instructions it intercepts, and 0 for the hypervisor's own exits (APM
    // 24593 rev. 3.46, 15.7.1).
    if (svm_nrips) {
        c12->nrip = hardware ? c02->nrip : 0;
    }
    // The bytes of the instruction whose data access made a nested page fault
    // or a page fault, which the processor fetched for the exit that led here;
    // every other exit clears their count (APM 24593 rev. 3.46, 15.8.4).
    if (svm_decode_assists) {
        bool fault = code == SVM_EXIT_NPF || code == SVM_EXIT_EXCEPTION_BASE + VECTOR_PF;
        uint32_t last = atomic_load_explicit(&v->last_exit, memory_order_relaxed);
        bool fetched = last == SVM_EXIT_NPF || last == SVM_EXIT_EXCEPTION_BASE + VECTOR_PF;
        c12->insn_len = fault && fetched ? c02->insn_len : 0;
        if (fault && fetched) {
            memcpy(c12->insn_bytes, c02->insn_bytes, sizeof(c12->insn_bytes));
        }
    }

    // What the guest's guest left of the guest's NMIs: blocked as the
    // processor still has them, and one injected but not taken still pending.
    struct vmcb *vmcb01 = n->vmcb01;
    if (svm_vnmi && !n->vnmi12) {
        if ((c02->vintr & SVM_VINTR_NMI) != 0) {
            v->nmi_pending = true;
        }
        vmcb01->control.vintr = (vmcb01->control.vintr & ~SVM_VINTR_NMI_MASK)
                              | (c02->vintr & SVM_VINTR_NMI_MASK);
    }
    copy_lbrs(n->lbrv ? s12 : &vmcb01->save, s02);
    copy_shared_state(vmcb01, vmcb02, !n->fred12);
    if (svm_eraps) {
        vmcb01->control.rap_control |= SVM_RAP_CLEAR;
    }
    avic_leave(n);
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

static bool injection_valid(uint64_t event, bool fred) {
    if ((event & SVM_EVENT_VALID) == 0) {
        return true;
    }
    uint64_t type = event & SVM_EVENT_TYPE_MASK;
    unsigned int vector = event & SVM_EVENT_VECTOR_MASK;
    // For a guest with FRED, INT1 and SYSCALL have vector 1, and only
    // exceptions have error codes or nest (AMD 69191 rev. 1.10).
    if (fred) {
        if ((event & (SVM_EVENT_ERROR_VALID | SVM_EVENT_NESTED)) != 0
         && type != SVM_EVENT_TYPE_EXCEPTION) {
            return false;
        }
        if (type == SVM_EVENT_TYPE_INT1 || type == SVM_EVENT_TYPE_SYSCALL) {
            return vector == 1;
        }
    }
    if (type == SVM_EVENT_TYPE_EXCEPTION) {
        if (vector == VECTOR_CP) {
            return (cpuid_cr4_allowed() & CR4_CET) != 0;
        }
        return vector < 32 && ((INJECTABLE_EXCEPTIONS >> vector) & 1) != 0;
    }
    return type == SVM_EVENT_TYPE_INTR || type == SVM_EVENT_TYPE_NMI
        || type == SVM_EVENT_TYPE_SOFT_INT;
}

// A permission map's last byte is below the maximum physical address, which is
// the last one MAXPHYADDR has (APM 24593 rev. 3.46, 15.10.1 and 15.11).
static bool table_fits(uint64_t base, size_t size) {
    return (page_down(base) + size) >> phys_width == 0;
}

static bool avic_pages_logged;

// AVIC's checks at VMRUN (APM 24593 rev. 3.46, 15.29.4.3), and the pages the
// processor reaches for the guest's guest, which it can only where they are
// RAM: the backing page, and the logical table in xAVIC mode.
static bool avic_valid(const struct vmcb_control *c, bool x2) {
    uint64_t last = c->avic_physical_table & SVM_AVIC_PHYSICAL_LAST;
    uint64_t limit = AVIC_LAST;
    if (x2) {
        limit = x2avic_ext ? X2AVIC_EXT_LAST : X2AVIC_LAST;
    }
    size_t pages = last / (PAGE_SIZE / 8) + 1;
    if (last > limit
     || !table_fits(c->avic_backing_page, PAGE_SIZE)
     || !table_fits(c->avic_logical_table, PAGE_SIZE)
     || !table_fits(c->avic_physical_table, pages * PAGE_SIZE)) {
        return false;
    }
    bool ram = guest_mem_page(page_down(c->avic_backing_page)) != NULL
            && (x2 || guest_mem_page(page_down(c->avic_logical_table)) != NULL);
    if (!ram && !avic_pages_logged) {
        avic_pages_logged = true;
        printf("Nested SVM: the guest's AVIC pages are not RAM, its VMRUN fails\n");
    }
    return ram;
}

// VMRUN's checks of the state it loads (APM 24593 rev. 3.46, 15.5.1, and 15.20
// for the event it injects), as Linux 7.3's KVM makes them where the manual
// leaves them open (nested.c, nested_vmcb_check_controls() and
// nested_vmcb_check_save()).
static bool vmcb12_valid(struct vcpu *v, const struct vmcb *vmcb12) {
    const struct vmcb_control *c = &vmcb12->control;
    const struct vmcb_save *s = &vmcb12->save;
    if ((c->intercept_misc2 & SVM_INTERCEPT_VMRUN) == 0
     || c->guest_asid == 0
     || c->guest_asid >= nasid - 1
     || !table_fits(c->msrpm_base_pa, MSRPM_SIZE)
     || !table_fits(c->iopm_base_pa, IOPM_USED)
     || !injection_valid(c->eventinj, (s->cr4 & CR4_FRED) != 0)) {
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
     || (s->cr0 >> 32) != 0 || (s->dr6 >> 32) != 0
     || (s->dr7 >> 32) != 0 || (s->cr4 & ~cpuid_cr4_allowed()) != 0
     || (s->efer & ~cpuid_efer_allowed(v)) != 0) {
        return false;
    }
    // NMI virtualisation needs the NMI intercept (APM 24593 rev. 3.46,
    // 15.21.10).
    if (svm_vnmi && (c->vintr & SVM_VINTR_NMI_ENABLE) != 0
     && (c->intercept_misc1 & SVM_INTERCEPT_NMI) == 0) {
        return false;
    }
    // A guest's guest with FRED runs at CPL 0 in 64-bit mode or at CPL 3 with
    // IOPL 0, and its SS.DPL is 0 in 64-bit mode or 3 with IOPL 0 too; FRED's
    // MSRs, where VMCB12 has the processor switch them, clear FRED_CONFIG's
    // reserved bits, the stack pointers' bits 5:0, and the shadow stack
    // pointers' bits 2:0 (AMD 69191 rev. 1.10).
    if ((s->cr4 & CR4_FRED) != 0) {
        unsigned int ss_dpl = (s->ss.attrib >> 5) & 3;
        bool long_cs = (s->cs.attrib & VCPU_SEG_L) != 0;
        bool iopl0 = (s->rflags & RFLAGS_IOPL) == 0;
        if (((s->cpl != 0 || !long_cs) && (s->cpl != 3 || !iopl0))
         || ((ss_dpl != 0 || !long_cs) && (ss_dpl != 3 || !iopl0))) {
            return false;
        }
    }
    if (vcpu_fred() && (c->virt_ext & SVM_VIRT_EXT_FRED) != 0) {
        if ((s->fred_config & FRED_CONFIG_RESERVED) != 0) {
            return false;
        }
        for (size_t i = 0; i < 3; i++) {
            if ((s->fred_rsp[i] & 0x3f) != 0 || (s->fred_ssp[i] & 7) != 0) {
                return false;
            }
        }
    }
    // Nested paging takes the guest's paging, and a valid nCR3 and guest PAT
    // (APM 24593 rev. 3.46, 15.25.3 and 15.25.4).
    bool npt = (c->np_enable & SVM_NP_ENABLE) != 0;
    if (npt
     && ((v->vmcb->save.cr0 & CR0_PG) == 0 || (c->n_cr3 >> phys_width) != 0
      || !x86_pat_valid(s->g_pat))) {
        return false;
    }
    // SSSCheck needs nested paging (15.25.15), and Paging-Write and Verify
    // Paging-Write need it and the guest in 64-bit mode (15.25.6.2 and
    // 15.25.6.3).
    if (ssscheck && !npt && (c->np_enable & SVM_NP_SSS) != 0) {
        return false;
    }
    uint64_t pws = (paging_write ? SVM_NP_PW : 0) | (verify_paging_write ? SVM_NP_VPW : 0);
    bool long_mode = (v->vmcb->save.efer & EFER_LMA) != 0
                  && (v->vmcb->save.cs.attrib & VCPU_SEG_L) != 0;
    if ((c->np_enable & pws) != 0 && (!npt || !long_mode)) {
        return false;
    }
    // So does GAAT, with a valid pointer (15.40.3).
    bool gaat_on = gaat && (c->np_enable & SVM_NP_GAAT) != 0;
    if (gaat_on && (!npt || !long_mode || (c->gaat_pointer & GAAT_POINTER_RESERVED) != 0)) {
        return false;
    }
    // x2AVIC needs AVIC, and AVIC nested paging, its pointers within the
    // physical address width, and its physical table's last index within its
    // mode's (15.29.4.1 and 15.29.4.3).
    bool avic_on = avic && (c->vintr & SVM_VINTR_AVIC) != 0;
    bool x2avic_on = x2avic && (c->vintr & SVM_VINTR_X2AVIC) != 0;
    if (x2avic_on && !avic_on) {
        return false;
    }
    if (avic_on && (!npt || !avic_valid(c, x2avic_on))) {
        return false;
    }
    if ((s->efer & EFER_LME) != 0 && (s->cr0 & CR0_PG) != 0) {
        if ((s->cr4 & CR4_PAE) == 0 || (s->cr0 & CR0_PE) == 0 || (s->cr3 >> phys_width) != 0
         || ((s->cs.attrib & VCPU_SEG_L) != 0 && (s->cs.attrib & VCPU_SEG_DB) != 0)) {
            return false;
        }
        return true;
    }
    // Without nested paging, VMRUN loads PAE paging's four PDPEs, which may
    // not set reserved bits (APM 24593 rev. 3.46, 15.5.1).
    if (!npt && (s->cr0 & CR0_PG) != 0 && (s->cr4 & CR4_PAE) != 0) {
        uint64_t pdpte[4];
        if (!guest_mem_phys_read(s->cr3 & 0xffffffe0, pdpte, sizeof(pdpte))) {
            return false;
        }
        for (size_t i = 0; i < 4; i++) {
            if (!guest_mem_pdpte_valid(pdpte[i])) {
                return false;
            }
        }
    }
    return true;
}

// VMCB02's intercepts: VMCB12's, and the hypervisor's but for the windows it
// opens for the guest itself, the hypercalls of the legacy BIOS, which are #UD
// and VMMCALL, those that keep the guest's own CR0.NE, which are its CR0 reads
// and writes and its #MF, and the guest's guest's CR8, which is its own where
// VMCB12 masks interrupts virtually and is the guest's TPR otherwise (APM
// 24593 rev. 3.46, 15.21.2). The guest's guest's VMLOAD, VMSAVE, CLGI, and
// STGI, which the hypervisor runs for it where the guest does not intercept
// them, exit whatever the guest's own do, but where VMCB12 has the processor
// virtualise them: the guest's nested paging then translates VMLOAD's and
// VMSAVE's addresses.
static void merge_intercepts(struct nsvm *n) {
    uint32_t own[SVM_INTERCEPT_VECTORS];
    uint32_t to[SVM_INTERCEPT_VECTORS];
    memcpy(own, &n->vmcb01->control, sizeof(own));
    for (size_t i = 0; i < SVM_INTERCEPT_VECTORS; i++) {
        to[i] = own[i] | n->intercepts[i];
    }
    to[0] = (own[0] & ~(SVM_INTERCEPT_CR0_READ | SVM_INTERCEPT_CR0_WRITE)) | n->intercepts[0];
    to[2] = (own[2] & ~((uint32_t)1 << VECTOR_MF)) | n->intercepts[2];
    to[3] = (own[3] & ~(SVM_INTERCEPT_VINTR | SVM_INTERCEPT_CR0_SEL_WRITE)) | n->intercepts[3];
    uint32_t svm = SVM_INTERCEPT_VMLOAD | SVM_INTERCEPT_VMSAVE | SVM_INTERCEPT_STGI
                 | SVM_INTERCEPT_CLGI;
    // The guest's guest's MONITOR and MWAIT run on the processor unless VMCB12
    // intercepts them, as with VMX, where VMCS12 alone has them exit.
    uint32_t idle = SVM_INTERCEPT_MONITOR | SVM_INTERCEPT_MWAIT;
    to[4] = (own[4] & ~(SVM_INTERCEPT_VMMCALL | svm | idle))
          | n->intercepts[4]
          | (n->vls12 ? 0 : SVM_INTERCEPT_VMLOAD | SVM_INTERCEPT_VMSAVE)
          | (n->vgif12 ? 0 : SVM_INTERCEPT_STGI | SVM_INTERCEPT_CLGI);
    // AVIC has CR8 be the guest's guest's TPR whatever VMCB12 masks (APM 24593
    // rev. 3.46, 15.29.4.1).
    if ((n->vintr12 & SVM_VINTR_MASKING) == 0 && !n->avic12) {
        to[0] |= SVM_INTERCEPT_CR8_READ | SVM_INTERCEPT_CR8_WRITE;
    }
    // Under AVIC, the guest's guest halts in the processor, as its doorbells
    // end the halt there alone, but where VMCB12 intercepts HLT, whose idle
    // intercept the processor then applies with AVIC's interrupts. Else
    // VMCB12's idle HLT intercept is HLT's in VMCB02, whose virtual interrupts
    // may be the hypervisor's (nsvm_l2_exit()).
    if (n->avic12) {
        to[3] = (to[3] & ~SVM_INTERCEPT_HLT) | (n->intercepts[3] & SVM_INTERCEPT_HLT);
        to[5] = (to[5] & ~SVM_INTERCEPT_IDLE_HLT) | (n->intercepts[5] & SVM_INTERCEPT_IDLE_HLT);
    } else if ((n->intercepts[5] & SVM_INTERCEPT_IDLE_HLT) != 0) {
        to[3] |= SVM_INTERCEPT_HLT;
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
        const uint64_t *l1 = (const uint64_t *)guest_mem_page(
            page_down(base12) + page * PAGE_SIZE
        );
        uint64_t *to = (uint64_t *)(n->msrpm02 + page * PAGE_SIZE);
        for (size_t i = 0; i < PAGE_SIZE / 8; i++) {
            to[i] = mine[i] | (l1 == NULL ? ~(uint64_t)0 : l1[i]);
        }
        // Under x2AVIC, the guest's guest's x2APIC MSRs are its own where
        // VMCB12's map passes them (APM 24593 rev. 3.46, 15.29.10).
        if (page == 0 && l1 != NULL && n->x2avic12) {
            memcpy(
                (uint8_t *)to + MSR_X2APIC_FIRST / 4, (const uint8_t *)l1 + MSR_X2APIC_FIRST / 4,
                (MSR_X2APIC_LAST + 1 - MSR_X2APIC_FIRST) / 4
            );
        }
    }
    return (uintptr_t)n->msrpm02;
}

// An entry of the guest's nested page tables, in the formats of its page
// tables (APM 24593 rev. 3.46, 15.25.3 and 15.25.5), which reads as all ones
// where nothing decodes, its reserved bits set.
static uint64_t npt12_read(const struct nsvm *n, uintptr_t gpa, void **at) {
    uint8_t *page = guest_mem_page(page_down(gpa));
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
// tables are walked in its paging mode at VMRUN, every access a user one where
// VMCB12 does not have GMET (15.25.14), but supervisor shadow-stack ones under
// SSSCheck (15.25.15), and the writes of its page table walks, GUEST_TABLE,
// paging writes under Paging-Write (15.25.6.2): 0 where the walk maps the
// access, which sets its accessed flags and, for a write, its dirty flag, else
// the nested page fault's error code. A supervisor shadow-stack page that such
// an access reaches, and a page of the guest's guest's page tables that paging
// writes may write, take their dirty flag at once, as NPT02 lets the writes
// that may follow through without a fault.
static uint64_t npt12_walk(
    struct nsvm *n,
    uintptr_t gpa,
    bool write,
    bool fetch,
    bool user,
    bool shadow_stack,
    bool guest_table,
    struct npt12_walk *w
) {
    bool wide = n->npt_wide;
    // The fetch flag is defined only with SMEP or with PAE paging's NX (APM
    // 24593 rev. 3.46, 8.4.2), as for the guest's page faults (guest_mem.c).
    bool fetch_flag = fetch && (n->npt_smep || (wide && n->npt_nx));
    uint64_t error = NPT12_FAULT | (n->gmet && !user ? 0 : NPF_US) | (write ? NPF_RW : 0)
                   | (fetch_flag ? NPF_ID : 0);
    bool dirties = write || shadow_stack;
    bool paging = n->pw && guest_table && write;
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
        if ((pdpte & PTE_P) == 0) {
            return error;
        }
        if (!guest_mem_pdpte_valid(pdpte)) {
            return error | NPF_P | NPF_RSV;
        }
        table = pdpte & PTE_ADDRESS;
    } else if (!wide) {
        if ((gpa >> 32) != 0) {
            return error;
        }
        table = n->ncr3 & 0xfffff000;
    } else {
        if ((gpa >> (12 + 9 * n->npt_levels)) != 0) {
            return error;
        }
        table = n->ncr3 & PTE_ADDRESS;
    }
    void *used[5];
    uint64_t entries[5];
    bool writable = true;
    bool user_page = true;
    bool executable = true;
    for (size_t level = n->npt_levels; level-- > 0; ) {
        unsigned int shift = wide ? 12 + 9 * level : 12 + 10 * level;
        size_t index = (gpa >> shift) & (wide ? 511 : 1023);
        uint64_t e = npt12_read(n, table + index * (wide ? 8 : 4), &used[level]);
        if ((e & PTE_P) == 0) {
            return error;
        }
        bool large = level > 0 && (e & PTE_PS) != 0 && (wide || n->npt_pse);
        if ((e & guest_mem_pte_reserved(wide, n->npt_pae, level, large, n->npt_nx)) != 0) {
            return error | NPF_P | NPF_RSV;
        }
        entries[level] = e;
        bool tables_writable = writable;
        writable = writable && (e & PTE_W) != 0;
        user_page = user_page && (e & PTE_U) != 0;
        executable = executable && (!wide || !n->npt_nx || (e & PTE_NX) == 0);
        if (level > 0 && !large) {
            table = wide ? e & PTE_ADDRESS : e & 0xfffff000;
            continue;
        }
        // Under SSSCheck, a supervisor shadow-stack page has NX set and U/S
        // clear in its leaf, and R/W set in every entry above: supervisor
        // shadow-stack accesses reach those alone, whatever the leaf's R/W,
        // and the faults of other accesses there say it is one (APM 24593 rev.
        // 3.46, 15.25.15 and 15.25.6).
        bool sss_page = n->sss && (e & PTE_NX) != 0 && (e & PTE_U) == 0 && tables_writable;
        // Paging writes need R/W in the entries above the leaf and PW in it.
        bool pw_page = n->pw && tables_writable && (e & NPT_PW) != 0;
        if (shadow_stack) {
            if (!sss_page) {
                return error | NPF_P;
            }
        } else {
            // With GMET, U/S keeps the guest's guest's supervisor code off
            // user pages, and nothing off pages for the supervisor.
            bool denied = n->gmet ? fetch && !user && user_page : !user_page;
            bool unwritable = paging ? !pw_page : write && !writable;
            if (denied || unwritable || (fetch && !executable)) {
                return error | NPF_P | (sss_page ? NPF_SSS_PAGE : 0);
            }
        }
        dirties = dirties || (guest_table && pw_page);
        // A write that sets the dirty flag logs its 4-KByte page, where VMCB12
        // has it, or is not done for want of room (APM 24593 rev. 3.46,
        // 15.25.16).
        bool logs = n->pml && dirties && (e & PTE_D) == 0;
        if (logs && n->pml_index > PML_LAST) {
            return NPT12_PML_FULL;
        }
        for (size_t l = n->npt_levels; l-- > level; ) {
            uint64_t set = PTE_A | (l == level && dirties ? PTE_D : 0);
            if ((entries[l] & set) != set && !npt12_mark(n, used[l], entries[l], set)) {
                goto retry;
            }
        }
        if (logs) {
            uint64_t page = page_down(gpa);
            guest_mem_phys_write(page_down(n->pml_base) + n->pml_index * 8, &page, 8);
            n->pml_index--;
        }
        uint64_t page_mask = ((uint64_t)1 << shift) - 1;
        if (wide) {
            w->gpa = (e & PTE_ADDRESS & ~page_mask) | (gpa & page_mask);
        } else if (large) {
            // PSE-36 puts physical address bits 39:32 in PDE bits 20:13.
            w->gpa = (e & 0xffc00000) | ((e >> 13) & 0xff) << 32 | (gpa & page_mask);
        } else {
            w->gpa = (e & 0xfffff000) | (gpa & page_mask);
        }
        w->level = level;
        w->leaf = e | PTE_A | (dirties ? PTE_D : 0);
        w->writable = writable;
        w->executable = executable;
        w->user = user_page;
        w->sss = shadow_stack;
        w->pw = pw_page;
        return 0;
    }
    return error;
}

static void npt02_free(struct nsvm *n, uint64_t *table, size_t level) {
    for (size_t i = 0; level > 0 && i < 512; i++) {
        uint64_t e = table[i];
        if ((e & PTE_P) != 0 && (e & PTE_PS) == 0) {
            npt02_free(n, (uint64_t *)(uintptr_t)(e & PTE_ADDRESS), level - 1);
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

// The tables of one ASID, for a flush of its translations alone, which VMCB02
// then makes.
static void npt02_drop_asid(struct nsvm *n, uint32_t asid) {
    for (size_t i = 0; i < NPT02_ROOTS; i++) {
        if (n->npt02[i].root != NULL && n->npt02[i].asid == asid) {
            npt02_free(n, n->npt02[i].root, 3);
            n->npt02[i].root = NULL;
        }
    }
    n->current02 = NULL;
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
        if (at->root != NULL && at->ncr3 == n->ncr3 && at->asid == asid && at->gmet == n->gmet) {
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
        r->gmet = n->gmet;
        r->root = npt02_table(n);
    }
    n->current02 = r;
    return (uintptr_t)r->root;
}

// The memory type of a leaf of the guest's nested page tables, through its PAT
// (APM 24593 rev. 3.46, 15.25.9).
static uint8_t npt12_memtype(const struct nsvm *n, const struct npt12_walk *w) {
    uint64_t pat = w->level > 0 ? PTE_PAT_LARGE : PTE_PAT_4K;
    size_t index = ((w->leaf & pat) != 0 ? 4 : 0) | ((w->leaf & PTE_PCD) != 0 ? 2 : 0)
                 | ((w->leaf & PTE_PWT) != 0 ? 1 : 0);
    return (uint8_t)((n->npt_pat >> (index * 8)) & 7);
}

// An NPT02 leaf for the guest's mapping and the hypervisor's. Writes wait for
// the guest's dirty flag, which the leaf then takes as set until the guest
// flushes its guest's translations, as a processor's TLB may (APM 24593 rev.
// 3.46, 15.16). Where the hypervisor's memory type is not write-back, it wins
// but over uncacheable.
static uint64_t npt02_leaf(
    const struct nsvm *n,
    const struct npt12_walk *w,
    uintptr_t hpa,
    uint32_t prot,
    uint8_t memtype,
    size_t level
) {
    uint8_t type = npt12_memtype(n, w);
    if (memtype != MT_WB && type != MT_UC) {
        type = memtype;
    }
    bool writable = w->writable && (prot & GPT_W) != 0 && (w->leaf & PTE_D) != 0;
    uint64_t e = gpt_npt_leaf(
        hpa & ~(x86_level_size(level) - 1), writable ? GPT_RWX : GPT_R | GPT_X, type, level
    );
    e |= PTE_A | (writable ? PTE_D : 0);
    if (!w->executable) {
        e |= PTE_NX;
    }
    if (n->gmet && !w->user) {
        e &= ~PTE_U;
    }
    // A supervisor shadow-stack page of the guest's stays one where the
    // hypervisor's nested page tables have the page writable, as shadow-stack
    // writes need no write access in the leaf.
    if (w->sss && (prot & GPT_W) != 0) {
        e = (e | PTE_NX | PTE_D) & ~PTE_U;
    }
    // Paging writes, where the guest's nested page tables allow them, and its
    // Verify Paging-Write, which the processor carries out on NPT02 and which
    // looks at the PW bit alone where VMCB12 has no Paging-Write.
    bool pw = n->pw
            ? w->pw && (prot & GPT_W) != 0 && (w->leaf & PTE_D) != 0
            : n->vpw && (w->leaf & NPT_PW) != 0;
    if (pw) {
        e |= NPT_PW;
    }
    if (n->vpw && (w->leaf & NPT_VPW) != 0) {
        e |= NPT_VPW;
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
    bool translated = gpt_translate_leaf(w->gpa, &hpa, &prot, &memtype, &level)
                   && !vlapic_access_page(page_down(hpa));
    uint64_t apic = n->avic12 && !n->x2avic12 ? page_down(n->copy.control.avic_apic_bar) : 1;
    if (!translated && page_down(gpa) == apic) {
        hpa = (uintptr_t)avic_apic_page;
        prot = GPT_R | GPT_W | GPT_X;
        memtype = MT_WB;
        level = 0;
        translated = true;
        n->avic_apic02 = apic;
    }
    if (!translated) {
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
        uint64_t *slot = &table[x86_level_index(gpa, l)];
        if ((*slot & PTE_P) == 0 || (*slot & PTE_PS) != 0) {
            *slot = (uint64_t)(uintptr_t)npt02_table(n) | PTE_P | PTE_W | PTE_U | PTE_A;
        }
        table = (uint64_t *)(uintptr_t)(*slot & PTE_ADDRESS);
    }
    uint64_t *slot = &table[x86_level_index(gpa, level)];
    if ((*slot & PTE_P) != 0 && (*slot & PTE_PS) == 0 && level > 0) {
        npt02_free(n, (uint64_t *)(uintptr_t)(*slot & PTE_ADDRESS), level - 1);
    }
    *slot = npt02_leaf(n, w, hpa, prot, memtype, level);
    return *slot;
}

static bool npt02_allows(uint64_t leaf, bool write, bool fetch) {
    return (leaf & PTE_P) != 0 && (!write || (leaf & PTE_W) != 0)
        && (!fetch || (leaf & PTE_NX) == 0);
}

// AVIC of the guest's guests (APM 24593 rev. 3.46, 15.29). The guest's
// physical APIC ID tables hold its physical addresses, which the processor
// cannot take: VMCB02 has a shadow of the guest's table, made again from it at
// each of its guest's entries, whose entries name the same backing pages. The
// guest's IsRunning bits and physical APIC IDs give way to the vCPUs whose
// guests' guests run with those pages, as their APIC IDs are the processors'
// own; an IPI to any other exits, and reaches the guest where its own entry is
// not running, as with the guest's table. The guest's logical table and its
// backing pages are the processor's own.
struct avic_table {
    bool used;
    uint64_t table12;
    // Where the guest's table is, by page, the shadow, and of each of the
    // guest's entries, the valid bit and backing page the shadow has.
    uint8_t *pages12[AVIC_TABLE_PAGES];
    uint64_t *entries;
    uint64_t *seen12;
    // The vCPUs whose guests' guests run with it.
    uint32_t users;
};

static struct spinlock avic_lock;
static struct avic_table *avic_tables;
static bool avic_memory_logged;

static size_t avic_pages(void) {
    return x2avic_ext ? AVIC_TABLE_PAGES : 1;
}

// The guest's entry of its table, as the processor reads it, else 0 where its
// page is not RAM.
static uint64_t avic_entry12(const struct avic_table *t, size_t index) {
    const uint8_t *page = t->pages12[index / (PAGE_SIZE / 8)];
    if (page == NULL) {
        return 0;
    }
    return atomic_load_explicit(
        (const _Atomic(uint64_t) *)(page + index % (PAGE_SIZE / 8) * 8), memory_order_seq_cst
    );
}

// Whether the processor's APIC ID fits the shadow's entries, and its doorbells
// before x2AVIC (APM 24593 rev. 3.46, Figure 15-22).
static bool avic_id_fits(const struct cpu *cpu) {
    return cpu->apic_id <= (x2avic ? SVM_AVIC_ENTRY_HOST_ID : AVIC_LAST);
}

// The processor whose vCPU's guest's guest runs with a backing page, else
// NULL.
static struct cpu *avic_runner(uint64_t backing12) {
    for (size_t i = 0; i < smp_cpu_count(); i++) {
        struct cpu *cpu = smp_cpu(i);
        struct nsvm *n = cpu->vcpu != NULL ? cpu->vcpu->nsvm : NULL;
        if (n != NULL && atomic_load_explicit(&n->avic_running, memory_order_relaxed)
         && n->avic_backing12 == backing12 && avic_id_fits(cpu)) {
            return cpu;
        }
    }
    return NULL;
}

// The shadow of one of the guest's entries, its valid bit and backing page:
// none where the page is not RAM, as AVIC cannot reach it.
static uint64_t avic_shadow_entry(uint64_t entry12) {
    if ((entry12 & SVM_AVIC_ENTRY_VALID) == 0) {
        return 0;
    }
    uint64_t backing12 = entry12 & SVM_AVIC_ENTRY_BACKING;
    uint8_t *page = guest_mem_page(backing12);
    if (page == NULL) {
        if (!avic_memory_logged) {
            avic_memory_logged = true;
            printf(
                "Nested SVM: a backing page of the guest's AVIC at %#llx is not RAM, IPIs to it "
                "exit\n",
                (unsigned long long)backing12
            );
        }
        return 0;
    }
    uint64_t shadow = SVM_AVIC_ENTRY_VALID | (uint64_t)(uintptr_t)page;
    struct cpu *runner = avic_runner(backing12);
    if (runner != NULL) {
        shadow |= SVM_AVIC_ENTRY_RUNNING | runner->apic_id;
    }
    return shadow;
}

// The shadow made again, up to an index, where the guest's entries changed.
static void avic_sync(struct avic_table *t, size_t last) {
    for (size_t i = 0; i <= last; i++) {
        uint64_t entry12 = avic_entry12(t, i) & (SVM_AVIC_ENTRY_VALID | SVM_AVIC_ENTRY_BACKING);
        if (entry12 == t->seen12[i]) {
            continue;
        }
        t->seen12[i] = entry12;
        atomic_store_explicit(
            (_Atomic(uint64_t) *)&t->entries[i], avic_shadow_entry(entry12), memory_order_seq_cst
        );
    }
}

// The shadow of a guest's table, which another that no processor runs a
// guest's guest with gives way to, made again in full.
static struct avic_table *avic_table_for(uint64_t table12) {
    if (avic_tables == NULL) {
        avic_tables = kmalloc(smp_cpu_count() * sizeof(struct avic_table));
    }
    struct avic_table *spare = NULL;
    for (size_t i = 0; i < smp_cpu_count(); i++) {
        struct avic_table *t = &avic_tables[i];
        if (t->used && t->table12 == table12) {
            return t;
        }
        if (t->users == 0 && (spare == NULL || !t->used)) {
            spare = t;
        }
    }
    // Each vCPU holds one at most.
    if (spare == NULL) {
        panic("No shadow physical APIC ID table is free\n");
    }
    if (spare->entries == NULL) {
        spare->entries = page_alloc(avic_pages());
        spare->seen12 = page_alloc(avic_pages());
    }
    memset(spare->entries, 0, avic_pages() * PAGE_SIZE);
    memset(spare->seen12, 0, avic_pages() * PAGE_SIZE);
    spare->used = true;
    spare->table12 = table12;
    for (size_t i = 0; i < avic_pages(); i++) {
        spare->pages12[i] = guest_mem_page(table12 + i * PAGE_SIZE);
    }
    return spare;
}

// The entry of the shadow that names the guest's guest's own backing page: by
// its APIC ID, as its backing page holds it, else the first, else -1.
static int32_t avic_own_index(const struct nsvm *n, const struct avic_table *t, size_t last) {
    const uint32_t *regs = (const uint32_t *)guest_mem_page(n->avic_backing12);
    uint32_t id = regs[APIC_ID / 4];
    id = n->x2avic12 ? id : id >> 24;
    uint64_t own = SVM_AVIC_ENTRY_VALID | n->avic_backing12;
    if (id <= last && t->seen12[id] == own) {
        return (int32_t)id;
    }
    for (size_t i = 0; i <= last; i++) {
        if (t->seen12[i] == own) {
            return (int32_t)i;
        }
    }
    return -1;
}

// Whether the shadow entry runs on a processor, and which.
static void avic_set_running(struct avic_table *t, int32_t index, const struct cpu *cpu) {
    _Atomic(uint64_t) *entry = (_Atomic(uint64_t) *)&t->entries[index];
    uint64_t shadow = atomic_load_explicit(entry, memory_order_relaxed)
                    & ~(SVM_AVIC_ENTRY_RUNNING | SVM_AVIC_ENTRY_HOST_ID);
    if (cpu != NULL) {
        shadow |= SVM_AVIC_ENTRY_RUNNING | cpu->apic_id;
    }
    atomic_store_explicit(entry, shadow, memory_order_seq_cst);
}

// At an entry of the guest's guest with AVIC: VMCB02 gets the shadow of the
// guest's table, the guest's own pages, and its guest's entry running here.
static void avic_enter(struct nsvm *n) {
    const struct vmcb_control *c12 = &n->copy.control;
    struct vmcb_control *c = &n->vmcb02->control;
    struct cpu *cpu = this_cpu();
    size_t last = c12->avic_physical_table & SVM_AVIC_PHYSICAL_LAST;
    n->avic_backing12 = page_down(c12->avic_backing_page);
    spinlock_acquire(&avic_lock);
    struct avic_table *t = avic_table_for(page_down(c12->avic_physical_table));
    avic_sync(t, last);
    t->users++;
    n->avic_table = t;
    n->avic_own = avic_own_index(n, t, last);
    if (n->avic_own >= 0 && avic_id_fits(cpu)) {
        avic_set_running(t, n->avic_own, cpu);
    }
    atomic_store_explicit(&n->avic_running, true, memory_order_seq_cst);
    spinlock_release(&avic_lock);
    c->avic_apic_bar = c12->avic_apic_bar;
    c->avic_backing_page = (uint64_t)(uintptr_t)guest_mem_page(n->avic_backing12);
    // x2AVIC has no logical table (15.29.10), whose page need not be RAM then.
    uint8_t *logical = guest_mem_page(page_down(c12->avic_logical_table));
    if (logical == NULL) {
        logical = (uint8_t *)t->entries;
    }
    c->avic_logical_table = (uint64_t)(uintptr_t)logical;
    c->avic_physical_table = (uint64_t)(uintptr_t)t->entries | last;
}

// At an exit to the guest, or where the vCPU starts again.
static void avic_leave(struct nsvm *n) {
    struct avic_table *t = n->avic_table;
    if (t == NULL) {
        return;
    }
    spinlock_acquire(&avic_lock);
    atomic_store_explicit(&n->avic_running, false, memory_order_seq_cst);
    if (n->avic_own >= 0) {
        uint64_t shadow = atomic_load_explicit(
            (_Atomic(uint64_t) *)&t->entries[n->avic_own], memory_order_relaxed
        );
        if ((shadow & SVM_AVIC_ENTRY_RUNNING) != 0
         && (shadow & SVM_AVIC_ENTRY_HOST_ID) == this_cpu()->apic_id) {
            avic_set_running(t, n->avic_own, avic_runner(n->avic_backing12));
        }
    }
    t->users--;
    n->avic_table = NULL;
    n->avic_own = -1;
    spinlock_release(&avic_lock);
}

// An IPI of the guest's guest that AVIC could not complete on the shadow: its
// targets, the IPI delivered to those whose entries the guest made since, as
// AVIC delivers it; and the guest's own exit where its table has a target not
// running, or not valid for a directed IPI (APM 24593 rev. 3.46, 15.29.6.1).
struct avic_delivery {
    struct avic_table *t;
    size_t last;
    bool deliver;
    uint8_t vector;
    bool directed;
    bool refused;
    int64_t not_running;
};

static void avic_target(struct avic_delivery *d, uint64_t index, uint32_t reported) {
    uint64_t shadow = 0;
    if (index <= d->last) {
        shadow = atomic_load_explicit(
            (_Atomic(uint64_t) *)&d->t->entries[index], memory_order_seq_cst
        );
    }
    if ((shadow & SVM_AVIC_ENTRY_VALID) == 0) {
        d->refused = d->refused || d->directed;
        return;
    }
    if (d->deliver) {
        uint8_t *backing = (uint8_t *)(uintptr_t)(shadow & SVM_AVIC_ENTRY_BACKING);
        _Atomic(uint32_t) *irr = (_Atomic(uint32_t) *)(backing + APIC_IRR + d->vector / 32 * 0x10);
        atomic_fetch_or_explicit(irr, (uint32_t)1 << (d->vector % 32), memory_order_seq_cst);
        if ((shadow & SVM_AVIC_ENTRY_RUNNING) != 0) {
            wrmsr(SVM_AVIC_DOORBELL, shadow & SVM_AVIC_ENTRY_HOST_ID);
        }
    }
    if ((shadow & SVM_AVIC_ENTRY_RUNNING) == 0 && d->not_running < 0
     && (avic_entry12(d->t, index) & SVM_AVIC_ENTRY_RUNNING) == 0) {
        d->not_running = reported;
    }
}

// The IPI's targets, as AVIC finds them in the tables (15.29.5.3 and
// 15.29.6.1): each valid entry of the physical one for a broadcast, but the
// sender's for one to others; the destination for a directed one; and for a
// logical one in x2AVIC mode, the x2APIC IDs of its cluster's bits, else the
// entries of the logical table that its bits name, in flat or cluster mode as
// the sender's DFR has it.
static void avic_visit(struct nsvm *n, struct avic_delivery *d, uint32_t low, uint32_t high) {
    uint32_t dest = n->x2avic12 ? high : high >> 24;
    uint32_t shorthand = low & ICR_SHORTHAND_MASK;
    uint32_t all = n->x2avic12 ? ~(uint32_t)0 : 0xff;
    bool broadcast = shorthand == ICR_SHORTHAND_ALL || shorthand == ICR_SHORTHAND_OTHERS
                  || (shorthand == ICR_SHORTHAND_NONE && dest == all);
    d->directed = !broadcast;
    if (broadcast) {
        for (size_t i = 0; i <= d->last; i++) {
            if (shorthand != ICR_SHORTHAND_OTHERS || (int64_t)i != n->avic_own) {
                avic_target(d, i, (uint32_t)i);
            }
        }
        return;
    }
    if (shorthand != ICR_SHORTHAND_NONE) {
        d->refused = true;
        return;
    }
    if ((low & ICR_LOGICAL) == 0) {
        avic_target(d, dest, dest);
        return;
    }
    if (n->x2avic12) {
        for (size_t bit = 0; bit < 16; bit++) {
            uint32_t id = (dest >> 16) << 4 | (uint32_t)bit;
            if ((dest >> bit & 1) != 0) {
                avic_target(d, id, id);
            }
        }
        return;
    }
    const uint32_t *regs = (const uint32_t *)guest_mem_page(n->avic_backing12);
    const uint32_t *table = (const uint32_t *)guest_mem_page(
        page_down(n->copy.control.avic_logical_table)
    );
    if (table == NULL) {
        d->refused = true;
        return;
    }
    bool flat = regs[APIC_DFR / 4] >> 28 == 0xf;
    for (size_t bit = 0; bit < (flat ? 8 : 4) && !d->refused; bit++) {
        uint32_t index = (uint32_t)(flat ? bit : (dest >> 4) * 4 + bit);
        if ((dest >> bit & 1) == 0) {
            continue;
        }
        uint32_t entry = atomic_load_explicit(
            (const _Atomic(uint32_t) *)&table[index], memory_order_seq_cst
        );
        if ((entry & SVM_AVIC_LOGICAL_VALID) == 0) {
            d->refused = true;
        } else {
            avic_target(d, entry & 0xff, index);
        }
    }
}

static bool avic_ipi(struct vcpu *v, struct nsvm *n, uint64_t info1, uint64_t info2) {
    uint32_t why = (uint32_t)(info2 >> 32);
    struct avic_table *t = n->avic_table;
    if ((why != SVM_AVIC_IPI_NOT_RUNNING && why != SVM_AVIC_IPI_BAD_TARGET) || t == NULL) {
        return false;
    }
    uint32_t low = (uint32_t)info1;
    uint32_t high = (uint32_t)(info1 >> 32);
    struct avic_delivery d = {
        .t = t, .last = n->vmcb02->control.avic_physical_table & SVM_AVIC_PHYSICAL_LAST,
        .vector = (uint8_t)(low & ICR_VECTOR_MASK), .not_running = -1
    };
    spinlock_acquire(&avic_lock);
    // The guest may have made the entries since its guest's entry. The
    // processor finds every target before it delivers to any.
    if (why == SVM_AVIC_IPI_BAD_TARGET) {
        avic_sync(t, d.last);
    }
    avic_visit(n, &d, low, high);
    if (!d.refused && why == SVM_AVIC_IPI_BAD_TARGET) {
        d.deliver = true;
        d.not_running = -1;
        avic_visit(n, &d, low, high);
    }
    spinlock_release(&avic_lock);
    if (d.refused) {
        return false;
    }
    if (d.not_running >= 0) {
        uint64_t info = (uint64_t)SVM_AVIC_IPI_NOT_RUNNING << 32 | (uint64_t)d.not_running;
        exit_to_l1(
            v, n, SVM_EXIT_AVIC_INCOMPLETE_IPI, info1, info, v->vmcb->control.exitintinfo, true
        );
    }
    return true;
}

// The guest's doorbell, to a processor whose vCPU's guest's guest runs with
// AVIC; at others it does nothing, as on those that run no guest (15.29.8.2).
static bool avic_doorbell(uint64_t value) {
    if ((value & ~(x2avic ? SVM_AVIC_ENTRY_HOST_ID : AVIC_LAST)) != 0) {
        return false;
    }
    struct cpu *cpu = smp_cpu_by_apic_id((uint32_t)value);
    struct nsvm *n = cpu != NULL && cpu->vcpu != NULL ? cpu->vcpu->nsvm : NULL;
    if (n != NULL && atomic_load_explicit(&n->avic_running, memory_order_seq_cst)) {
        wrmsr(SVM_AVIC_DOORBELL, value);
    }
    return true;
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
    // Of vector 5, the INVLPGB intercepts where the processor has INVLPGB, the
    // TLBSYNC one where it has TlbiCtl, the idle HLT and bus lock ones where
    // it has them, and the INVPCID one.
    uint32_t invlpgb = n->vmcb01->control.intercept_misc3 & SVM_INTERCEPT_INVLPGB;
    n->intercepts[5] &= (invlpgb != 0 ? SVM_INTERCEPT_INVLPGB | SVM_INTERCEPT_INVLPGB_ILLEGAL : 0)
                      | (svm_tlbictl ? SVM_INTERCEPT_TLBSYNC : 0)
                      | (idle_hlt ? SVM_INTERCEPT_IDLE_HLT : 0)
                      | (bus_lock_threshold ? SVM_INTERCEPT_BUS_LOCK : 0)
                      | SVM_INTERCEPT_INVPCID;
    n->vintr12 = c12->vintr;
    n->window = false;
    n->tsc_offset12 = c12->tsc_offset;
    n->vnmi12 = svm_vnmi && (c12->vintr & SVM_VINTR_NMI_ENABLE) != 0;
    n->vgif12 = svm_vgif && (c12->vintr & SVM_VINTR_GIF_ENABLE) != 0;
    // Virtual VMLOAD and VMSAVE need VMCB12's nested paging and the guest in
    // 64-bit mode (APM 24593 rev. 3.46, 15.33.1).
    bool vls = svm_vls && (c12->virt_ext & SVM_VIRT_EXT_VLS) != 0;
    n->vls12 = vls && (c12->np_enable & SVM_NP_ENABLE) != 0 && (vmcb01->save.efer & EFER_LMA) != 0
            && (vmcb01->save.cs.attrib & VCPU_SEG_L) != 0;
    n->vls12_refused = vls && !n->vls12;
    n->fred12 = vcpu_fred() && (c12->virt_ext & SVM_VIRT_EXT_FRED) != 0;
    n->avic12 = avic && (c12->vintr & SVM_VINTR_AVIC) != 0;
    n->x2avic12 = n->avic12 && x2avic && (c12->vintr & SVM_VINTR_X2AVIC) != 0;
    // Without xAVIC, or with the page elsewhere, NPT02's mapping of the local
    // APIC's page to avic_apic_page goes.
    uint64_t apic = n->avic12 && !n->x2avic12 ? page_down(c12->avic_apic_bar) : 1;
    if (n->avic_apic02 != 1 && n->avic_apic02 != apic) {
        npt02_drop_all(n);
        n->avic_apic02 = 1;
    }
    merge_intercepts(n);

    // The guest's PAUSE filter, as the hypervisor intercepts no PAUSE itself
    // (APM 24593 rev. 3.46, 15.14.4).
    c->pause_filter_threshold = c12->pause_filter_threshold;
    c->pause_filter_count = c12->pause_filter_count;
    // The count of the guest's bus lock threshold, which the processor takes
    // at VMRUN and writes back at #VMEXIT (APM 24593 rev. 3.46, 15.14.5).
    if (bus_lock_threshold) {
        c->bus_lock_counter = c12->bus_lock_counter;
    }
    c->iopm_base_pa = (uintptr_t)iopm02;
    c->msrpm_base_pa = merge_msrpm(n, c12->msrpm_base_pa);
    c->guest_asid = c12->guest_asid + 1;
    // VMCB12's flush, of its ASID where the guest has flush by ASID, else of
    // every ASID where bit 0 is set, as where TLB_CONTROL was a flag (APM
    // 24593 rev. 3.46, 15.16.1), but for one of every ASID VMCB02 still owes.
    uint8_t flush = c12->tlb_control;
    if (!svm_flush_by_asid || (flush != SVM_TLB_FLUSH_ASID && flush != SVM_TLB_FLUSH_ASID_LOCAL)) {
        flush = (flush & 1) != 0 ? SVM_TLB_FLUSH_ALL : 0;
    }
    if (c->tlb_control == 0) {
        c->tlb_control = flush;
    }
    n->npt = (c12->np_enable & SVM_NP_ENABLE) != 0;
    n->gmet = gmet && n->npt && (c12->np_enable & SVM_NP_GMET) != 0;
    n->sss = false;
    n->pw = paging_write && n->npt && (c12->np_enable & SVM_NP_PW) != 0;
    n->vpw = verify_paging_write && n->npt && (c12->np_enable & SVM_NP_VPW) != 0;
    n->gaat = gaat && n->npt && (c12->np_enable & SVM_NP_GAAT) != 0;
    c->gaat_pointer = n->gaat ? c12->gaat_pointer : 0;
    c->gaat_prefix_width = n->gaat ? c12->gaat_prefix_width : 0;
    // The predictor's entries from the guest are the guest's guest's to have
    // cleared, as Linux 7.3's KVM has it (nested.c,
    // nested_vmcb02_prepare_control()).
    c->rap_control = svm_eraps ? (c12->rap_control & SVM_RAP_ALLOW_LARGER) | SVM_RAP_CLEAR : 0;
    // VMCB12's NMI virtualisation and virtual GIF where the guest enables them
    // for its guest, else the hypervisor's, for the guest's own NMIs and flag
    // (APM 24593 rev. 3.46, 15.21.10 and 15.33.2).
    uint64_t nmi = SVM_VINTR_NMI | SVM_VINTR_NMI_MASK | SVM_VINTR_NMI_ENABLE;
    uint64_t gif = SVM_VINTR_GIF | SVM_VINTR_GIF_ENABLE;
    uint64_t avic_bits = n->avic12 ? SVM_VINTR_AVIC | SVM_VINTR_X2AVIC : 0;
    uint64_t from12 = SVM_VINTR_TPR_MASK | VIRQ_BITS | (n->vnmi12 ? nmi : 0)
                    | (n->vgif12 ? gif : 0) | avic_bits;
    uint64_t from01 = SVM_VINTR_MASKING | (n->vgif12 ? 0 : SVM_VINTR_GIF_ENABLE)
                    | (n->vnmi12 ? 0 : SVM_VINTR_NMI_ENABLE | SVM_VINTR_NMI_MASK);
    c->vintr = (c12->vintr & from12) | (vmcb01->control.vintr & from01);
    c->interrupt_shadow = c12->interrupt_shadow & SVM_INTERRUPT_SHADOW;
    c->exitcode = 0;
    c->exitinfo1 = 0;
    c->exitinfo2 = 0;
    c->exitintinfo = 0;
    // VMCB12's TlbiCtl, where the processor has it.
    c->np_enable = (vmcb01->control.np_enable & ~SVM_NP_INVLPGB)
                 | (svm_tlbictl ? c12->np_enable & SVM_NP_INVLPGB : 0);
    c->n_cr3 = gpt_root();
    if (n->npt) {
        const struct vmcb_save *s01 = &vmcb01->save;
        n->ncr3 = c12->n_cr3;
        n->npt_wide = (s01->efer & EFER_LMA) != 0 || (s01->cr4 & CR4_PAE) != 0;
        n->npt_pae = (s01->efer & EFER_LMA) == 0 && (s01->cr4 & CR4_PAE) != 0;
        n->npt_levels = 4;
        if ((s01->efer & EFER_LMA) == 0) {
            n->npt_levels = 2;
        } else if ((s01->cr4 & CR4_LA57) != 0) {
            n->npt_levels = 5;
        }
        n->npt_nx = (s01->efer & EFER_NXE) != 0;
        n->sss = ssscheck && (c12->np_enable & SVM_NP_SSS) != 0 && n->npt_wide && n->npt_nx;
        n->npt_pse = (s01->cr4 & CR4_PSE) != 0;
        n->npt_smep = (s01->cr4 & CR4_SMEP) != 0;
        n->npt_pat = s01->g_pat;
        n->rogpt = rogpt && (c12->np_enable & SVM_NP_ROGPT) != 0;
        n->pml = pml && (c12->np_enable & SVM_NP_PML) != 0;
        n->pml_base = c12->pml_base;
        n->pml_index = c12->pml_index;
        if (n->rogpt) {
            c->np_enable |= SVM_NP_ROGPT;
        }
        if (n->gmet) {
            c->np_enable |= SVM_NP_GMET;
        }
        if (n->sss) {
            c->np_enable |= SVM_NP_SSS;
        }
        c->np_enable |= (n->pw ? SVM_NP_PW : 0) | (n->vpw ? SVM_NP_VPW : 0)
                      | (n->gaat ? SVM_NP_GAAT : 0);
        // Its flushes drop the nested translations too, but for those of its
        // ASID's non-global translations alone, which a change of its nested
        // page tables does not take (APM 24593 rev. 3.46, 15.16.1).
        if (c->tlb_control == SVM_TLB_FLUSH_ALL) {
            npt02_drop_all(n);
        } else if (c->tlb_control == SVM_TLB_FLUSH_ASID) {
            npt02_drop_asid(n, c12->guest_asid);
        }
        c->n_cr3 = npt02_root(n);
    }
    // A flush for the guest's INVLPGAs for the ASID, of all of its
    // translations, global ones too, but not the nested ones.
    uint64_t *owed = &n->invlpga_asids[c12->guest_asid / 64];
    uint64_t bit = (uint64_t)1 << (c12->guest_asid % 64);
    if ((*owed & bit) != 0) {
        *owed &= ~bit;
        if (c->tlb_control != SVM_TLB_FLUSH_ALL) {
            c->tlb_control = svm_flush_by_asid ? SVM_TLB_FLUSH_ASID : SVM_TLB_FLUSH_ALL;
        }
    }
    // FRED's nested-exception bit is not reserved where the guest's guest has
    // CR4.FRED (AMD 69191 rev. 1.10).
    uint64_t reserved = EVENT_RESERVED & ~((s12->cr4 & CR4_FRED) != 0 ? SVM_EVENT_NESTED : 0);
    c->eventinj = c12->eventinj & ~reserved;
    // Software interrupts injected take their return address from here (APM
    // 24593 rev. 3.46, 15.20): VMCB12's where the guest has NRIP saving, else
    // its RIP, which the guest moved past the instruction itself.
    c->nrip = svm_nrips ? c12->nrip : s12->rip;
    // VMCB12's PMC virtualisation and its event filtering, where the processor
    // has them.
    n->pmc = pmc_virt && (c12->virt_ext & SVM_VIRT_EXT_PMC) != 0;
    n->pmc_filter = n->pmc && pmc_filter && (c12->virt_ext & SVM_VIRT_EXT_PMC_FILTER) != 0;
    c->virt_ext = vmcb01->control.virt_ext | (n->pmc ? SVM_VIRT_EXT_PMC : 0)
                | (n->pmc_filter ? SVM_VIRT_EXT_PMC_FILTER : 0);
    if (n->pmc_filter) {
        memcpy(c->allowed_perf_ctl, c12->allowed_perf_ctl, sizeof(c->allowed_perf_ctl));
        c->allowed_perf_ctl_mask = c12->allowed_perf_ctl_mask;
    }
    c->exitintdata = 0;
    c->eventinjdata = c12->eventinjdata;
    if (n->avic12) {
        avic_enter(n);
    }

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
    if (svm_v_spec_ctrl) {
        s->spec_ctrl = s12->spec_ctrl;
    }
    if (n->pmc) {
        copy_pmcs(s, s12);
    }
    n->lbrv = svm_lbr_virtualisation && (c12->virt_ext & SVM_VIRT_EXT_LBR) != 0;
    copy_lbrs(s, n->lbrv ? s12 : &vmcb01->save);
    if (n->lbrv) {
        s->dbgctl &= debugctl_bits;
    }
    copy_shared_state(vmcb02, vmcb01, !n->fred12);
    if (n->fred12) {
        copy_fred_state(s, s12);
    }
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
// rev. 3.39, VMRUN, STGI): #UD without EFER.SVME, but for STGI where the
// processor has SVM-Lock, or outside protected mode, #GP above CPL 0.
static bool allowed(struct vcpu *v, bool lockable) {
    struct vmcb_save *s = &v->vmcb->save;
    bool svme = nsvm_svme(v) || (lockable && svm_svml);
    if (!svme || (s->cr0 & CR0_PE) == 0 || (s->rflags & RFLAGS_VM) != 0) {
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
    if (!allowed(v, false) || !decode(v, &insn)) {
        return;
    }
    uint64_t gpa = rax_address(v, &insn);
    struct vmcb *vmcb12 = valid_page(gpa) ? (struct vmcb *)guest_mem_page(gpa) : NULL;
    if (vmcb12 == NULL) {
        vcpu_inject_exception(v, VECTOR_GP, true, 0);
        return;
    }
    struct vmcb *vmcb01 = n->vmcb01;
    uint64_t rip = vmcb01->save.rip;
    uint64_t next = svm_nrips ? vmcb01->control.nrip : rip + insn.length;
    if ((next >> 32) != (rip >> 32)
     && ((vmcb01->save.efer & EFER_LMA) == 0 || (vmcb01->save.cs.attrib & VCPU_SEG_L) == 0)) {
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
        pmu_retired(v, 1, 0);
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
    // An instruction retired, counted in its guest's mode as the processor
    // counts it, as Linux 7.3's KVM has it (svm/nested.c, nested_svm_vmrun()).
    pmu_retired(v, 1, 0);
}

// VMLOAD and VMSAVE (APM 24593 rev. 3.46, 15.5.2), from the guest or a guest
// of its that runs them, with addresses in the guest's physical memory.
static void vmload_vmsave(struct vcpu *v, bool load) {
    struct insn insn;
    if (!allowed(v, false) || !decode(v, &insn)) {
        return;
    }
    uint64_t gpa = rax_address(v, &insn);
    struct vmcb *vmcb = valid_page(gpa) ? (struct vmcb *)guest_mem_page(gpa) : NULL;
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

// INVLPGA (APM 24593 rev. 3.46, 15.16.2), as a flush of every translation of
// ECX's ASID, which Linux 7.3's KVM makes for it, INVLPGA having errata on
// Genoa and Turin (svm.c, svm_flush_tlb_gva()): of the guest's own, 0, at
// once, of another at its next VMRUN. ASIDs past those the guest has hold no
// translations.
static void invlpga(struct vcpu *v) {
    struct insn insn;
    if (!allowed(v, false) || !decode(v, &insn)) {
        return;
    }
    uint32_t asid = (uint32_t)v->gprs.rcx;
    if (asid == 0) {
        svm_flush_guest_tlb(v);
    } else if (asid < nasid - 1) {
        v->nsvm->invlpga_asids[asid / 64] |= (uint64_t)1 << (asid % 64);
    }
    vcpu_advance(v, insn.length);
}

// The guest's guest's INVLPGB, as VMCB12's TlbiCtl enables it where the
// processor has TlbiCtl (APM 24594 rev. 3.39, INVLPGB).
bool nsvm_tlbi_enabled(struct vcpu *v) {
    struct nsvm *n = v->nsvm;
    return n == NULL || !n->in_l2 || !svm_tlbictl
        || (n->copy.control.np_enable & SVM_NP_INVLPGB) != 0;
}

// An ASID needs EFER.SVME, and one the processor has (APM 24594 rev. 3.39,
// INVLPGB).
uint32_t nsvm_invlpgb_asids(struct vcpu *v) {
    return nsvm_svme(v) ? nasid - 1 : 1;
}

bool nsvm_invlpgb_illegal(struct vcpu *v) {
    struct nsvm *n = v->nsvm;
    if (n == NULL || !n->in_l2 || (n->intercepts[5] & SVM_INTERCEPT_INVLPGB_ILLEGAL) == 0) {
        return false;
    }
    exit_to_l1(v, n, SVM_EXIT_INVLPGB_ILLEGAL, 0, 0, v->vmcb->control.exitintinfo, true);
    return true;
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
    // hypervisor cannot run for the guest, where the guest's own EFER.SVME or
    // SVM-Lock enables it whatever CPUID says (APM 24594 rev. 3.39, SKINIT).
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
            if (allowed(v, code == SVM_EXIT_STGI)) {
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
    switch (size) {
        case 2:
        case 4: {
            break;
        }
        default: {
            size = 1;
            break;
        }
    }
    uintptr_t base = page_down(n->copy.control.iopm_base_pa);
    for (size_t port = info >> 16; port < (info >> 16) + size; port++) {
        const uint8_t *page = guest_mem_page(base + page_down(port / 8));
        if (page == NULL || ((page[port / 8 & 0xfff] >> (port % 8)) & 1) != 0) {
            return true;
        }
    }
    return false;
}

// The bit of an MSR permission map for an access, false for MSRs past its
// ranges (APM 24593 rev. 3.46, 15.11).
static bool msrpm_bit(uint32_t msr, bool write, size_t *bit) {
    size_t offset;
    if (msr <= 0x1fff) {
        offset = 0;
    } else if (msr >= 0xc0000000 && msr <= 0xc0001fff) {
        offset = 0x800;
    } else if (msr >= 0xc0010000 && msr <= 0xc0011fff) {
        offset = 0x1000;
    } else {
        return false;
    }
    *bit = offset * 8 + (msr & 0x1fff) * 2 + (write ? 1 : 0);
    return true;
}

// Whether VMCB12's MSR permission map has the access exit, as accesses to MSRs
// past its ranges always do (APM 24593 rev. 3.46, 15.11).
static bool msr_wanted(struct vcpu *v, struct nsvm *n, bool write) {
    if ((n->intercepts[3] & SVM_INTERCEPT_MSR) == 0) {
        return false;
    }
    size_t bit;
    if (!msrpm_bit((uint32_t)v->gprs.rcx, write, &bit)) {
        return true;
    }
    uintptr_t base = page_down(n->copy.control.msrpm_base_pa);
    const uint8_t *page = guest_mem_page(base + page_down(bit / 8));
    return page == NULL || ((page[bit / 8 & 0xfff] >> (bit % 8)) & 1) != 0;
}

// A WRMSR exit that VMCB02's MSR permission map did not make: guest PMC event
// filtering's, for a write that would enable an event it does not allow, which
// goes to the guest (APM 24593 rev. 3.46, 15.39.1).
static bool pmc_filtered(struct vcpu *v, const struct nsvm *n, bool write) {
    size_t bit;
    return n->pmc_filter && write && msrpm_bit((uint32_t)v->gprs.rcx, write, &bit)
        && ((n->msrpm02[bit / 8] >> (bit % 8)) & 1) == 0;
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
        case SVM_EXIT_INVALID:
        case SVM_EXIT_INVALID_PMC: {
            return true;
        }
        case SVM_EXIT_AVIC_INCOMPLETE_IPI:
        case SVM_EXIT_AVIC_NOACCEL: {
            return n->avic12;
        }
        case SVM_EXIT_IOIO: {
            return io_wanted(n, c->exitinfo1);
        }
        case SVM_EXIT_MSR: {
            bool write = (c->exitinfo1 & 1) != 0;
            return msr_wanted(v, n, write) || pmc_filtered(v, n, write);
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
    // A machine check exits where the guest's guest lacks CR4.MCE whether
    // intercepted or not, where the processor has HOST_MCE_OVERRIDE (APM 24593
    // rev. 3.46, 15.12.18): it is the guest's where VMCB12 intercepts it, and,
    // as the guest's SVM has HOST_MCE_OVERRIDE too, where the guest's own
    // CR4.MCE is set.
    if (code == SVM_EXIT_EXCEPTION_BASE + VECTOR_MC) {
        return (n->intercepts[2] & ((uint32_t)1 << VECTOR_MC)) != 0
            || ((v->vmcb->save.cr4 & CR4_MCE) == 0 && (n->vmcb01->save.cr4 & CR4_MCE) != 0);
    }
    if (code >= SVM_EXIT_EXCEPTION_BASE && code < SVM_EXIT_EXCEPTION_BASE + 32) {
        uint32_t own = n->vmcb01->control.intercept_exceptions & ~((uint32_t)1 << VECTOR_MF);
        return ((own >> (code - SVM_EXIT_EXCEPTION_BASE)) & 1) == 0;
    }
    // VMLOAD and VMSAVE that VMCB12 has virtualised where the processor's
    // checks refuse exit as though intercepted (APM 24593 rev. 3.46, 15.33.1).
    if ((code == SVM_EXIT_VMLOAD || code == SVM_EXIT_VMSAVE) && n->vls12_refused) {
        return true;
    }
    if (code >= 32 * SVM_INTERCEPT_VECTORS) {
        return false;
    }
    return ((n->intercepts[code / 32] >> (code % 32)) & 1) != 0;
}

// Whether a shadow-stack access of the guest's guest that faulted is a
// supervisor one: one of event delivery, or of an instruction at CPL 0 to 2
// but WRUSS, whose accesses are user ones (SDM 325462-093 Vol. 2D, WRUSSD/
// WRUSSQ; Intel APX 355828-009, 6.73 and 6.74). False where the instruction
// cannot be read, to run it again.
static bool supervisor_shadow_stack(struct vcpu *v, bool *supervisor) {
    *supervisor = true;
    if ((v->vmcb->control.exitintinfo & SVM_EVENT_VALID) != 0) {
        return true;
    }
    if (v->vmcb->save.cpl == 3) {
        *supervisor = false;
        return true;
    }
    struct insn insn;
    if (!insn_decode(v, &insn)) {
        return false;
    }
    bool legacy = insn.space == (INSN_SPACE_LEGACY | 2) && insn.opcode_byte == 0xf5;
    bool apx = insn.space == (INSN_SPACE_EVEX | 4) && insn.opcode_byte == 0x65;
    *supervisor = !((legacy || apx) && insn.prefix == INSN_PREFIX_66);
    return true;
}

// A nested page fault of NPT02: the guest's where its nested page tables do
// not allow the access, else mapped as the hypervisor's nested page tables
// have the guest's memory, or emulated where that is the hypervisor's own
// device. The guest's guest's page table walks are writes, but those that set
// no flag where VMCB12 has read-only guest page tables, as VMCB02 then has
// (APM 24593 rev. 3.46, 15.25.5 and 15.25.6.1).
static void npt_fault(struct vcpu *v, struct nsvm *n) {
    struct vmcb_control *c = &v->vmcb->control;
    uintptr_t gpa = c->exitinfo2;
    uint64_t info = c->exitinfo1;
    bool walk = (info & NPF_TABLE) != 0;
    bool write = (walk && !n->rogpt) || (info & NPF_RW) != 0;
    bool fetch = !walk && (info & NPF_ID) != 0;
    // Where VMCB02 has GMET, EXITINFO1 tells the access's privilege, which for
    // a fetch is the CPL's, read from VMCB02 as some processors may tell user
    // mode for supervisor mode (APM 24593 rev. 3.46, 15.25.14; 56323 rev. 1.03
    // and 56683 rev. 1.08, erratum 1218).
    bool user = fetch ? v->vmcb->save.cpl == 3 : (info & NPF_US) != 0;
    bool shadow_stack = false;
    if (n->sss && (info & NPF_SS) != 0 && !supervisor_shadow_stack(v, &shadow_stack)) {
        return;
    }
    // A verification that failed on NPT02's copies of the guest's bits failed
    // on the guest's (APM 24593 rev. 3.46, 15.25.6.3).
    if ((info & NPF_VPW) != 0) {
        uint64_t error = info & (NPF_P | NPF_RW | NPF_US | NPF_ID | NPF_FINAL | NPF_VPW);
        exit_to_l1(v, n, SVM_EXIT_NPF, error, gpa, c->exitintinfo, true);
        return;
    }
    struct npt12_walk w;
    uint64_t error = npt12_walk(n, gpa, write, fetch, user, shadow_stack, walk, &w);
    if (error == NPT12_PML_FULL) {
        exit_to_l1(v, n, SVM_EXIT_PML_FULL, 0, 0, c->exitintinfo, false);
        return;
    }
    // Whether the access was one to a shadow stack, and of which privilege,
    // the processor's error code tells (APM 24593 rev. 3.46, 15.25.7).
    if (error != 0) {
        error = (error & ~NPT12_FAULT) | (info & (NPF_FINAL | NPF_TABLE | NPF_SS));
        if ((info & NPF_SS) != 0) {
            error = (error & ~NPF_US) | (info & NPF_US);
        }
        exit_to_l1(v, n, SVM_EXIT_NPF, error, gpa, c->exitintinfo, true);
        return;
    }
    uint64_t leaf = npt02_fill(n, gpa, &w);
    uint64_t sss = PTE_P | PTE_U | PTE_NX;
    bool paging = n->pw && walk && write;
    bool mapped = npt02_allows(leaf, write, fetch);
    if (w.sss) {
        mapped = (leaf & sss) == (PTE_P | PTE_NX);
    } else if (paging) {
        mapped = (leaf & PTE_P) != 0 && (leaf & (PTE_W | NPT_PW)) != 0;
    }
    if (mapped) {
        return;
    }
    if (event_cut_short(v)) {
        if (!event_deliver_real_mode(v)) {
            event_undeliverable(v, w.gpa);
        }
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

// With a prefix width, GAAT paging translates the addresses with bit 63 set
// alone (APM 24593 rev. 3.46, 15.40.1).
bool nsvm_gaat_root(struct vcpu *v, uintptr_t linear, uint64_t *gaatp) {
    struct nsvm *n = v->nsvm;
    if (n == NULL || !n->in_l2 || !n->gaat
     || (n->copy.control.gaat_prefix_width != 0 && (linear >> 63) == 0)) {
        return false;
    }
    *gaatp = n->copy.control.gaat_pointer;
    return true;
}

bool nsvm_npt_translate(
    struct vcpu *v,
    uintptr_t gpa,
    bool write,
    bool fetch,
    bool walk,
    bool user,
    uint64_t *out
) {
    struct nsvm *n = v->nsvm;
    if (n->pending) {
        return false;
    }
    struct npt12_walk w;
    uint64_t error = npt12_walk(
        n, gpa, write || (walk && !n->rogpt), fetch && !walk, user, false, walk, &w
    );
    if (error == 0) {
        *out = w.gpa;
        return true;
    }
    n->pending = true;
    if (error == NPT12_PML_FULL) {
        n->pending_code = SVM_EXIT_PML_FULL;
        n->pending_info1 = 0;
        n->pending_info2 = 0;
        return false;
    }
    n->pending_code = SVM_EXIT_NPF;
    n->pending_info1 = (error & ~NPT12_FAULT) | (walk ? NPF_TABLE : NPF_FINAL);
    n->pending_info2 = gpa;
    return false;
}

bool nsvm_l2_exit(struct vcpu *v) {
    struct nsvm *n = v->nsvm;
    if (n == NULL || !n->in_l2) {
        return false;
    }
    atomic_store_explicit(&n->l2_live, false, memory_order_seq_cst);
    struct vmcb_control *c = &v->vmcb->control;
    uint64_t code = c->exitcode;
    n->run_pending = false;
    if (code == SVM_EXIT_NPF && n->npt) {
        npt_fault(v, n);
        return true;
    }
    if (code == SVM_EXIT_AVIC_INCOMPLETE_IPI && n->avic12
     && avic_ipi(v, n, c->exitinfo1, c->exitinfo2)) {
        return true;
    }
    // Where the guest's guest's own EFER.SVME is clear, the SVM instructions
    // raise #UD before their intercepts, but VMMCALL, whose intercept comes
    // first (APM 24593 rev. 3.46, Table 15-7), and STGI and SKINIT where the
    // processor has SVM-Lock (15.31), and their #GP outside CPL 0 is #UD too
    // (svm.c); the guest may intercept #UD instead.
    bool lockable = svm_svml && (code == SVM_EXIT_STGI || code == SVM_EXIT_SKINIT);
    if (!n->svme02 && !lockable && (code == SVM_EXIT_VMRUN || code == SVM_EXIT_INVLPGA
                                 || (code >= SVM_EXIT_VMLOAD && code <= SVM_EXIT_SKINIT))) {
        vcpu_inject_exception(v, VECTOR_UD, false, 0);
        return true;
    }
    if (code == SVM_EXIT_EXCEPTION_BASE + VECTOR_GP && !n->svme02) {
        return false;
    }
    // For VMCB12's idle HLT intercept, the guest's guest's HLT exits to the
    // guest where none of VMCB12's own virtual interrupts is pending: V_IRQ,
    // or V_NMI where VMCB12 virtualises NMIs (APM 24593 rev. 3.46, Table
    // 15-7). Its HLT intercept comes first.
    if (code == SVM_EXIT_HLT && (n->intercepts[3] & SVM_INTERCEPT_HLT) == 0
     && (n->intercepts[5] & SVM_INTERCEPT_IDLE_HLT) != 0) {
        uint64_t virq = n->window ? n->vintr12 : c->vintr;
        if ((virq & SVM_VINTR_IRQ) == 0 && (!n->vnmi12 || (c->vintr & SVM_VINTR_NMI) == 0)) {
            exit_to_l1(v, n, SVM_EXIT_IDLE_HLT, 0, 0, c->exitintinfo, true);
            return true;
        }
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
    struct vcpu *v,
    uint8_t vector,
    bool has_error,
    uint32_t error,
    uint64_t info2
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
    // EXITINFO1 has the error code, and EXITINFO2 a page fault's address or,
    // in a guest's guest with FRED, a debug exception's event data, the rest
    // being undefined (APM 24593 rev. 3.46, 15.12; AMD 69191 rev. 1.10).
    bool event_data = vector == VECTOR_DB && (v->vmcb->save.cr4 & CR4_FRED) != 0;
    n->pending = true;
    n->pending_code = SVM_EXIT_EXCEPTION_BASE + vector;
    n->pending_info1 = has_error ? error : 0;
    n->pending_info2 = vector == VECTOR_PF || event_data ? info2 : 0;
    return true;
}

bool nsvm_intercepts_exception(struct vcpu *v, uint8_t vector) {
    struct nsvm *n = v->nsvm;
    return n != NULL && n->in_l2 && ((n->intercepts[2] >> vector) & 1) != 0;
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

bool nsvm_shutdown(struct vcpu *v) {
    struct nsvm *n = v->nsvm;
    if (n == NULL || !n->in_l2 || (n->intercepts[3] & SVM_INTERCEPT_SHUTDOWN) == 0) {
        return false;
    }
    exit_to_l1(v, n, SVM_EXIT_SHUTDOWN, 0, 0, 0, false);
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
    // AVIC ignores V_IRQ (APM 24593 rev. 3.46, 15.29.4.1): the guest's guest
    // exits again once it has run a while instead, for the window to be looked
    // at again.
    if (on && n->avic12) {
        vcpu_exit_after_turn(v);
        vlapic_timer_program(v);
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
    if (atomic_load_explicit(&cpu->init_pending, memory_order_acquire) || event_nmi_waiting(v)) {
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

// Before the guest's guest runs on NPT02, it drops the tables where the guest
// flushed its nested translations since, a flush meanwhile seeing it live.
static void nested_flush_catch_up(struct nsvm *n) {
    atomic_store_explicit(&n->l2_live, true, memory_order_seq_cst);
    uint64_t flushes = atomic_load_explicit(&nested_flushes, memory_order_seq_cst);
    if (atomic_load_explicit(&n->nested_flush_seen, memory_order_relaxed) == flushes) {
        return;
    }
    npt02_drop_all(n);
    if (n->npt) {
        n->vmcb02->control.n_cr3 = npt02_root(n);
    }
    n->vmcb02->control.tlb_control = SVM_TLB_FLUSH_ALL;
    atomic_store_explicit(&n->nested_flush_seen, flushes, memory_order_release);
}

bool nsvm_invlpgb_nested(void) {
    return invlpgb_nested;
}

// An INVLPGB of the guest's that includes nested translations drops what its
// guests' NPT02 tables made of them, on every vCPU: each drops its own before
// its guest's guest next runs, which the guest's next TLBSYNC waits for (APM
// 24594 rev. 3.39, INVLPGB and TLBSYNC). Any ASID's flush drops them all.
void nsvm_invlpgb_nested_flush(struct vcpu *v) {
    struct nsvm *n = v->nsvm;
    if (n == NULL || n->in_l2) {
        return;
    }
    n->nested_flush_owed = atomic_fetch_add_explicit(&nested_flushes, 1, memory_order_seq_cst) + 1;
    n->vmcb01->control.intercept_misc3 |= SVM_INTERCEPT_TLBSYNC;
    for (size_t i = 0; i < smp_cpu_count(); i++) {
        struct vcpu *to = smp_cpu(i)->vcpu;
        if (to != NULL && to != v && to->nsvm != NULL
         && atomic_load_explicit(&to->nsvm->l2_live, memory_order_seq_cst)) {
            vcpu_kick(to->cpu);
        }
    }
}

// The guest's TLBSYNC after such an INVLPGB waits until no vCPU runs its
// guest's guest on tables from before the flush, as each does once the kick
// makes it exit.
void nsvm_tlbsync(struct vcpu *v) {
    struct nsvm *n = v->nsvm;
    if (n != NULL && !n->in_l2 && n->nested_flush_owed != 0) {
        for (size_t i = 0; i < smp_cpu_count(); i++) {
            struct vcpu *to = smp_cpu(i)->vcpu;
            if (to == NULL || to == v || to->nsvm == NULL) {
                continue;
            }
            struct nsvm *other = to->nsvm;
            while (atomic_load_explicit(&other->l2_live, memory_order_seq_cst)
                && atomic_load_explicit(&other->nested_flush_seen, memory_order_acquire)
                     < n->nested_flush_owed) {
                cpu_relax();
            }
        }
        n->nested_flush_owed = 0;
        n->vmcb01->control.intercept_misc3 &= ~SVM_INTERCEPT_TLBSYNC;
    }
    asm volatile ("tlbsync" ::: "memory");
    vcpu_skip_instruction(v);
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
    if (n == NULL) {
        return false;
    }
    if (!n->in_l2) {
        l1_intercepts(v, n);
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
    event_nmi_collect(v);
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
    nested_flush_catch_up(n);
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
