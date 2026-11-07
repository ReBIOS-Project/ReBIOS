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
#include <virt/vlapic.h>
#include <hw/lapic.h>
#include <hw/lapic_legacy.h>
#include <hw/power.h>
#include <iommu/iommu_tables.h>
#include <iommu/remap.h>
#include <lib/heap.h>
#include <lib/log.h>
#include <lib/spinlock.h>
#include <lib/util.h>
#include <memory.h>
#include <sys/cpu.h>
#include <sys/fixup.h>
#include <sys/mm.h>
#include <sys/smp.h>
#include <sys/x86.h>
#include <virt/gpt.h>
#include <virt/hv_timer.h>
#include <virt/insn.h>
#include <virt/irq.h>
#include <virt/mmio.h>
#include <virt/nsvm.h>
#include <virt/nvmx.h>
#include <virt/pci_virt.h>
#include <virt/svm.h>
#include <virt/vcpu.h>
#include <virt/virt.h>
#include <virt/vmx.h>

#define MSR_IA32_XAPIC_DISABLE_STATUS 0xbd
#define MSR_X2APIC_FIRST 0x800
#define MSR_X2APIC_LAST 0x8ff
#define MSR_X2APIC_ICR 0x830
#define MSR_X2APIC_SELF_IPI 0x83f

#define APIC_BASE_BSP ((uint64_t)1 << 8)
#define APIC_BASE_EXTD ((uint64_t)1 << 10)
// VT-d D51397-019 rev. 5.20, 8.1: INTR_REMAP and X2APIC_OPT_OUT.
#define DMAR_X2APIC_OPT_OUT_FLAGS 0x03
#define APIC_BASE_EN ((uint64_t)1 << 11)
#define APIC_BASE_ADDRESS ((uint64_t)0x000ffffffffff000)

#define APIC_APR 0x90
#define APIC_PPR 0xa0
#define APIC_RRD 0xc0
#define APIC_SELF_IPI 0x3f0

#define VERSION_EOI_SUPPRESSION ((uint32_t)1 << 24)
#define SVR_WRITABLE 0x1ff
#define SVR_SUPPRESS_EOI_BROADCAST ((uint32_t)1 << 12)
#define LVT_DELIVERY_MASK ((uint32_t)7 << 8)
#define LVT_DELIVERY_FIXED ((uint32_t)0 << 8)
#define LVT_DELIVERY_SMI ((uint32_t)2 << 8)
#define LVT_DELIVERY_NMI ((uint32_t)4 << 8)
#define LVT_DELIVERY_EXTINT ((uint32_t)7 << 8)
#define DFR_FLAT ((uint32_t)0xf0000000)
#define ESR_RECEIVE_ILLEGAL_VECTOR ((uint32_t)1 << 6)
#define XAPIC_BROADCAST ((uint32_t)0xff)
#define X2APIC_BROADCAST ((uint32_t)0xffffffff)
#define TIMER_DIVIDE_WRITABLE ((uint32_t)0xb)
#define LVT_TIMER_MODE_MASK ((uint32_t)3 << 17)
#define LVT_TIMER_PERIODIC ((uint32_t)1 << 17)
#define LVT_TIMER_TSC_DEADLINE ((uint32_t)2 << 17)
#define CPUID_1_ECX_TSC_DEADLINE ((uint32_t)1 << 24)
// What AMD's extended feature register says the space has, each turned on by
// the control register's bit at its position (APM 24593 rev. 3.46, 16.3.5,
// 16.3.6): the interrupt enable registers, specific EOIs, and 8-bit APIC IDs.
#define EXT_IER ((uint32_t)1 << 0)
#define EXT_SEOI ((uint32_t)1 << 1)
#define EXT_APIC_ID ((uint32_t)1 << 2)
#define EXT_CONTROL_BITS (EXT_IER | EXT_SEOI | EXT_APIC_ID)
// The vector, message type, and mask (APM 24593 rev. 3.46, 16.4.5).
#define EILVT_WRITABLE ((uint32_t)0x107ff)

#define MSR_AVIC_DOORBELL 0xc001011b
// The physical APIC ID table's entries and the logical one's (APM 24593 rev.
// 3.45, Tables 15-23 and 15-24), and the IDs the physical one holds in xAPIC
// mode, FFh being broadcast, and in x2APIC mode (APM 24593 rev. 3.45,
// 15.29.4.3).
#define AVIC_ENTRY_VALID ((uint64_t)1 << 63)
#define AVIC_ENTRY_RUNNING ((uint64_t)1 << 62)
#define AVIC_LOGICAL_VALID ((uint32_t)1 << 31)
#define AVIC_LOGICAL_ENTRIES 64
#define AVIC_MAX_ID 0xfe
#define X2AVIC_MAX_ID 0x1ff
// Why an IPI AVIC took on exits (APM 24593 rev. 3.45, Table 15-27).
#define AVIC_IPI_NOT_RUNNING 1
#define AVIC_IPI_BAD_BACKING_PAGE 3
#define AVIC_IPI_BAD_VECTOR 4

#define LVT_CMCI 0
#define LVT_TIMER 1
#define LVT_THERMAL 2
#define LVT_PERF 3
#define LVT_LINT0 4
#define LVT_LINT1 5
#define LVT_ERROR 6
#define LVT_COUNT 7

static const uint32_t lvt_regs[LVT_COUNT] = {
    APIC_LVT_CMCI, APIC_LVT_TIMER, APIC_LVT_THERMAL, APIC_LVT_PERF,
    APIC_LVT_LINT0, APIC_LVT_LINT1, APIC_LVT_ERROR
};

// The bits of each entry that software writes: the vector and mask, the
// delivery mode where there is one, the timer mode, and LINT's polarity and
// trigger mode.
static const uint32_t lvt_writable[LVT_COUNT] = {
    0x107ff, 0x700ff, 0x107ff, 0x107ff, 0x1a7ff, 0x1a7ff, 0x100ff
};

// How AVIC runs a vCPU's local APIC: on its page in xAPIC mode, through its
// MSRs in x2APIC mode, or, where it cannot, with them emulated but interrupts
// still delivered from the registers, as the hybrid mode of Linux 7.3's KVM
// has it.
#define AVIC_OFF 0
#define AVIC_XAPIC 1
#define AVIC_X2APIC 2
#define AVIC_HYBRID 3

// Interrupts other processors post for a vCPU, which VMX takes into its
// registers itself while it runs (SDM 325462-093 Vol. 3C, 32.6, Table 32-2).
struct posted {
    _Atomic(uint32_t) pir[8];
    // ON, then the vector and the destination of the notifications IPI
    // virtualisation and the IOMMUs send (D51397-019 rev. 5.20, 9.11).
    _Atomic(uint32_t) notification;
    _Atomic(uint32_t) destination;
    uint32_t reserved[6];
};

#define POSTED_ON ((uint32_t)1 << 0)
#define POSTED_VECTOR_SHIFT 16

struct vlapic {
    uintptr_t base;
    uint32_t id;
    // The registers at their xAPIC offsets, which VMX's virtual-APIC page also
    // is: only the vCPU's own processor changes them.
    uint8_t *regs;
    // What any processor requests of the vCPU, for its own to take into its
    // registers, and which of those requests are level-triggered.
    _Atomic(uint32_t) pending[8];
    _Atomic(uint32_t) pending_level[8];
    _Atomic(uint32_t) esr_pending;
    // Edge-triggered requests where VMX processes posted interrupts: a level-
    // triggered one must be in the registers before its vector's EOI exit is.
    struct posted *posted;
    _Atomic(int) avic;
    // Whether VMX virtualises the vCPU's IPIs.
    bool ipiv;
    // Something that decides where the guest's messages can be posted changed:
    // they are routed again before the vCPU enters the guest.
    bool post_recheck;
    // When the guest's timer next expires by the processor's TSC, or 0: the
    // processor's timer is the hypervisor's, which has its own uses for it.
    uint64_t timer_deadline;
    // The deadline as the guest's TSC has it, in TSC-deadline mode.
    uint64_t timer_guest_deadline;
    // What the processor's timer is armed for.
    uint64_t timer_armed;
    // Until when a deadline that passed during an exit waits.
    uint64_t timer_held_until;
};

static uint32_t regs_get(const struct vlapic *vl, uint32_t offset) {
    return *(const volatile uint32_t *)(vl->regs + offset);
}

static void regs_set(struct vlapic *vl, uint32_t offset, uint32_t value) {
    *(volatile uint32_t *)(vl->regs + offset) = value;
}

// AVIC sets bits of the IRR and TMR in the guest, and other processors set
// them for it.
static void regs_or(struct vlapic *vl, uint32_t offset, uint32_t bits) {
    atomic_fetch_or_explicit((_Atomic(uint32_t) *)(vl->regs + offset), bits, memory_order_seq_cst);
}

static void regs_and(struct vlapic *vl, uint32_t offset, uint32_t bits) {
    atomic_fetch_and_explicit(
        (_Atomic(uint32_t) *)(vl->regs + offset), bits, memory_order_seq_cst
    );
}

static uint32_t host_version;
static bool tsc_deadline_mode;
// Whether the guest is kept from x2APIC mode, as the DMAR table asks.
static bool x2apic_withheld;
// AMD's extended APIC space, where the processor has one: its feature register
// as the guest reads it, and its extended LVT entries.
static bool ext_space;
static uint32_t ext_feature;
static size_t eilvt_count;

// The Max LVT Entry of the version register each LVT entry needs (SDM
// 325462-093 Vol. 3A, 13.4.8): those beyond it are not there.
static const uint8_t lvt_max_needed[LVT_COUNT] = { 6, 0, 5, 4, 0, 0, 0 };

static bool lvt_present(size_t i) {
    return ((host_version >> 16) & 0xff) >= lvt_max_needed[i];
}

static bool lvt_reg_present(uint32_t reg) {
    for (size_t i = 0; i < LVT_COUNT; i++) {
        if (reg == lvt_regs[i]) {
            return lvt_present(i);
        }
    }
    return true;
}

static uint32_t eilvt_reg(size_t i) {
    return APIC_EILVT + (uint32_t)i * 0x10;
}

static uint32_t ier_reg(size_t i) {
    return APIC_IER + (uint32_t)i * 0x10;
}

// What a legacy BIOS hands the boot processor's local APIC over with.
static uint32_t legacy_svr;
static uint32_t legacy_lint0;
static uint32_t legacy_lint1;
static uint32_t legacy_thermal;
static uint32_t legacy_ext_control;
static uint32_t legacy_ier[8];
static uint32_t legacy_eilvt[APIC_EILVT_MAX];

// Pages the guest has had its local APICs at, which all exit, but for the
// accesses the processor virtualises.
#define MAX_PAGES 4
static uint64_t pages[MAX_PAGES];
static size_t page_count;
static struct spinlock page_lock;
// Where VMX virtualises accesses to those pages, they are this page (SDM
// 325462-093 Vol. 3C, 32.4), whose contents the processor then leaves alone:
// vCPUs not in xAPIC mode find nothing there.
static uintptr_t apic_access_page;
// VMX also virtualises reads of the registers and the delivery of interrupts
// (SDM 325462-093 Vol. 3C, 32.2, 32.4), which leaves writes with side effects
// to exits.
static bool apicv;
static bool posted_interrupts;
// x2APIC mode's MSRs, where the processor virtualises them, read from the
// registers and write to them, but for those MSRs whose access exits.
static bool x2apic_virtualisation;
// VMX posts IPIs between vCPUs itself (SDM 325462-093 Vol. 3C, 32.1.6), to
// PIDs a table finds by APIC ID, and sends their notifications to destinations
// it reads in the mode of the sender's local APIC: a vCPU does so only while
// its processor's is the mode they are written in, which follows the vCPUs'.
static bool ipi_virtualization;
static uint64_t *pid_table;
static uint32_t pid_table_last;
static struct spinlock ipiv_lock;
static _Atomic(bool) ipiv_x2apic;
static _Atomic(bool) ipiv_recheck;
// Nothing is posted while the destinations change format.
static _Atomic(bool) post_paused;

// The x2APIC MSRs whose registers hold what they read as, and those whose
// writes VMX virtualises with virtual-interrupt delivery (SDM 325462-093 Vol.
// 3C, 32.5), as AVIC does with the ICR's too (APM 24593 rev. 3.45, Table
// 15-22).
static const uint32_t x2apic_passed_reads[] = {
    APIC_ID, APIC_VERSION, APIC_TPR, APIC_PPR, APIC_LDR, APIC_SVR, APIC_ESR,
    APIC_LVT_CMCI, APIC_LVT_TIMER, APIC_LVT_THERMAL, APIC_LVT_PERF,
    APIC_LVT_LINT0, APIC_LVT_LINT1, APIC_LVT_ERROR, APIC_TIMER_INITIAL,
    APIC_TIMER_DIVIDE
};
static const uint32_t x2apic_passed_writes[] = { APIC_TPR, APIC_EOI, APIC_SELF_IPI };

// SVM's AVIC runs the local APICs (APM 24593 rev. 3.45, 15.29), all of them or
// none: it checks the permissions of the page the shared nested page tables
// map behind a local APIC's page (APM 24593 rev. 3.45, 15.29.3), which a vCPU
// it does not run would reach instead. That page is mapped only while every
// local APIC is in xAPIC mode, as AVIC does not redirect accesses to it in
// x2APIC mode (APM 24593 rev. 3.45, 15.29.10). AVIC cannot run them while a
// vCPU can take the 8259's interrupts, whose interrupt windows it ignores (APM
// 24593 rev. 3.45, 15.29.4.1), or while their IDs do not fit its tables.
static bool avic_supported;
static _Atomic(bool) avic_wanted;
static _Atomic(bool) avic_mapped;
// Something that decides it changed: it is decided again before the next
// entry, out of the exit handlers, as the other vCPUs leave the guest for it.
static _Atomic(bool) avic_recheck;
static struct spinlock avic_lock;
static uint64_t *avic_physical_table;
static uint32_t *avic_logical_table;

// The writes AVIC completes before it exits (APM 24593 rev. 3.45, Table
// 15-22), as Linux 7.3's KVM has them.
static const uint32_t avic_trapped_writes[] = {
    APIC_ID, APIC_EOI, APIC_RRD, APIC_LDR, APIC_DFR, APIC_SVR, APIC_ESR,
    APIC_ICR_LOW, APIC_LVT_TIMER, APIC_LVT_THERMAL, APIC_LVT_PERF,
    APIC_LVT_LINT0, APIC_LVT_LINT1, APIC_LVT_ERROR, APIC_TIMER_INITIAL,
    APIC_TIMER_DIVIDE
};

static const struct mmio_ops mmio_ops;

static void trap_page(uintptr_t page) {
    spinlock_acquire(&page_lock);
    for (size_t i = 0; i < page_count; i++) {
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
    if (apic_access_page != 0
     && (virt_vmx || atomic_load_explicit(&avic_mapped, memory_order_acquire))) {
        gpt_map(page, apic_access_page, PAGE_SIZE, GPT_RWX, MT_WB);
    } else {
        gpt_map(page, 0, PAGE_SIZE, 0, 0);
    }
    gpt_flush();
}

uintptr_t vlapic_access_address(void) {
    return virt_vmx ? apic_access_page : 0;
}

bool vlapic_access_page(uintptr_t hpa) {
    return apic_access_page != 0 && hpa == apic_access_page;
}

bool vlapic_x2apic_withheld(void) {
    return x2apic_withheld;
}

void vlapic_init(void) {
    // The DMAR's X2APIC_OPT_OUT, valid with INTR_REMAP, asks system software
    // not to turn x2APIC mode on, for firmware that cannot cope with it (VT-d
    // D51397-019 rev. 5.20, 8.1). Where the firmware has not turned it on
    // itself, the guest is not offered it, as Linux 7.3 (dmar_x2apic_optout())
    // and Limine keep to xAPIC mode there.
    const struct iommu_tables *tables = iommu_tables_parse();
    x2apic_withheld = tables->kind == IOMMU_KIND_VTD
                   && (tables->vtd_flags & DMAR_X2APIC_OPT_OUT_FLAGS) == DMAR_X2APIC_OPT_OUT_FLAGS
                   && !lapic_legacy_firmware_x2apic();
    if (x2apic_withheld) {
        printf("The DMAR table opts out of x2APIC mode, so the guest gets xAPIC mode only\n");
    }
    tsc_deadline_mode = (cpuid(1, 0).ecx & CPUID_1_ECX_TSC_DEADLINE) != 0;
    host_version = lapic_read(APIC_VERSION);
    legacy_svr = lapic_read(APIC_SVR);
    legacy_lint0 = lapic_read(APIC_LVT_LINT0);
    legacy_lint1 = lapic_read(APIC_LVT_LINT1);
    if (lvt_present(LVT_THERMAL)) {
        legacy_thermal = lapic_read(APIC_LVT_THERMAL);
    }
    if (!virt_vmx && (host_version & APIC_VERSION_EXT_SPACE) != 0) {
        uint32_t feature = lapic_read(APIC_EXT_FEATURE);
        ext_space = true;
        eilvt_count = (feature >> 16) & 0xff;
        if (eilvt_count > APIC_EILVT_MAX) {
            eilvt_count = APIC_EILVT_MAX;
        }
        ext_feature = (feature & EXT_CONTROL_BITS) | (uint32_t)eilvt_count << 16;
        legacy_ext_control = lapic_read(APIC_EXT_CONTROL) & ext_feature & EXT_CONTROL_BITS;
        for (size_t i = 0; i < 8; i++) {
            legacy_ier[i] = (feature & EXT_IER) != 0 ? lapic_read(ier_reg(i)) : 0xffffffff;
        }
        for (size_t i = 0; i < eilvt_count; i++) {
            legacy_eilvt[i] = lapic_read(eilvt_reg(i)) & EILVT_WRITABLE;
        }
    }
    if (virt_vmx && vmx_secondary_allowed(PROC2_VIRT_APIC_ACCESSES)) {
        uint8_t *page = page_alloc(1);
        memset(page, 0xff, PAGE_SIZE);
        apic_access_page = (uintptr_t)page;
        apicv = vmx_secondary_allowed(PROC2_APIC_REG_VIRT | PROC2_VIRT_INTR_DELIVERY);
        posted_interrupts = apicv && vmx_pin_allowed(PIN_POSTED_INTR);
        x2apic_virtualisation = apicv && vmx_secondary_allowed(PROC2_VIRT_X2APIC);
        ipi_virtualization = posted_interrupts && vmx_tertiary_allowed(PROC3_IPI_VIRT);
    }
    if (ipi_virtualization) {
        for (size_t i = 0; i < smp_cpu_count(); i++) {
            if (smp_cpu(i)->apic_id > pid_table_last) {
                pid_table_last = smp_cpu(i)->apic_id;
            }
        }
        // The last index is a 16-bit field.
        if (pid_table_last > 0xffff) {
            ipi_virtualization = false;
        } else {
            pid_table = page_alloc(((pid_table_last + 1) * 8 + PAGE_SIZE - 1) / PAGE_SIZE);
            atomic_store_explicit(&ipiv_x2apic, lapic_x2apic(), memory_order_release);
            atomic_store_explicit(&ipiv_recheck, true, memory_order_release);
        }
    }
    if (!virt_vmx && svm_avic) {
        // RAM, as AVIC may fail to redirect accesses to MMIO (AMD 56683 rev.
        // 1.08, erratum 1308).
        uint8_t *page = page_alloc(1);
        memset(page, 0xff, PAGE_SIZE);
        apic_access_page = (uintptr_t)page;
        avic_physical_table = page_alloc(1);
        avic_logical_table = page_alloc(1);
        avic_supported = true;
    }
    trap_page(VLAPIC_DEFAULT_BASE);
    virt_intercept_msr(MSR_IA32_APIC_BASE, true, true);
    virt_intercept_msr(MSR_IA32_XAPIC_DISABLE_STATUS, true, true);
    for (size_t msr = MSR_X2APIC_FIRST; msr <= MSR_X2APIC_LAST; msr++) {
        virt_intercept_msr(msr, true, true);
    }
    if (x2apic_virtualisation || svm_x2avic) {
        for (size_t i = 0; i < ARRAY_COUNT(x2apic_passed_reads); i++) {
            uint32_t reg = x2apic_passed_reads[i];
            if (lvt_reg_present(reg)) {
                virt_x2apic_msr_passed(MSR_X2APIC_FIRST + (reg >> 4), true, false);
            }
        }
        for (size_t reg = APIC_ISR; reg < APIC_IRR + 0x80; reg += 0x10) {
            virt_x2apic_msr_passed(MSR_X2APIC_FIRST + (reg >> 4), true, false);
        }
        for (size_t i = 0; i < ARRAY_COUNT(x2apic_passed_writes); i++) {
            virt_x2apic_msr_passed(MSR_X2APIC_FIRST + (x2apic_passed_writes[i] >> 4), false, true);
        }
        if (svm_x2avic) {
            virt_x2apic_msr_passed(MSR_X2APIC_ICR, false, true);
        }
    }
    // Without IPI virtualisation, a write to the ICR's MSR would be the
    // processor's (SDM 325462-093 Vol. 3C, 32.5).
    if (ipi_virtualization && x2apic_virtualisation) {
        virt_x2apic_icr_passed(MSR_X2APIC_ICR);
    }
}

static bool enabled(const struct vlapic *vl) {
    return (vl->base & APIC_BASE_EN) != 0;
}

static bool x2apic(const struct vlapic *vl) {
    return (vl->base & (APIC_BASE_EN | APIC_BASE_EXTD)) == (APIC_BASE_EN | APIC_BASE_EXTD);
}

static bool sw_enabled(const struct vlapic *vl) {
    return (regs_get(vl, APIC_SVR) & APIC_SVR_ENABLED) != 0;
}

// The processor virtualises the vCPU's accesses to the APIC-access page while
// its local APIC is in xAPIC mode, and its x2APIC MSRs in x2APIC mode.
static void access_virtualisation(const struct vlapic *vl) {
    if (!virt_vmx || apic_access_page == 0) {
        return;
    }
    uint64_t controls = vmx_read(VMCS_PROC_CONTROLS2);
    uint64_t was = controls;
    controls &= ~(uint64_t)(PROC2_VIRT_APIC_ACCESSES | PROC2_VIRT_X2APIC);
    bool x2 = x2apic_virtualisation && x2apic(vl);
    if (enabled(vl) && !x2apic(vl)) {
        // Translations from before it may not know the page is special (SDM
        // 325462-093 Vol. 3C, 32.4.5).
        if ((was & PROC2_VIRT_APIC_ACCESSES) == 0) {
            gpt_flush();
        }
        controls |= PROC2_VIRT_APIC_ACCESSES;
    } else if (x2) {
        controls |= PROC2_VIRT_X2APIC;
    }
    vmx_write(VMCS_PROC_CONTROLS2, controls);
    if (x2apic_virtualisation) {
        uintptr_t bitmap = virt_msr_bitmap();
        if (x2) {
            bitmap = vl->ipiv ? virt_msr_bitmap_x2apic_icr() : virt_msr_bitmap_x2apic();
        }
        vmx_write(VMCS_MSR_BITMAP, bitmap);
    }
}

static uint32_t x2apic_ldr(uint32_t id) {
    return (id >> 4) << 16 | (uint32_t)1 << (id & 0xf);
}

// The IRR, ISR, and TMR keep 32 of their 256 bits in each 16-byte slot.
static uint32_t vector_reg(uint32_t base, unsigned int vector) {
    return base + vector / 32 * 0x10;
}

static bool vector_set(const struct vlapic *vl, uint32_t base, unsigned int vector) {
    return ((regs_get(vl, vector_reg(base, vector)) >> (vector % 32)) & 1) != 0;
}

static void set_vector(struct vlapic *vl, uint32_t base, unsigned int vector, bool on) {
    uint32_t offset = vector_reg(base, vector);
    uint32_t bit = (uint32_t)1 << (vector % 32);
    if (on) {
        regs_or(vl, offset, bit);
    } else {
        regs_and(vl, offset, ~bit);
    }
}

static int highest(const struct vlapic *vl, uint32_t base) {
    for (size_t i = 8; i-- > 0; ) {
        uint32_t word = regs_get(vl, base + (uint32_t)i * 0x10);
        if (word != 0) {
            return i * 32 + 31 - __builtin_clz(word);
        }
    }
    return -1;
}

// Whether AMD's interrupt enable registers hold requests back (PPR 57238 rev.
// 0.49, 2.1.13.2.1.8).
static bool ier_on(const struct vlapic *vl) {
    return ext_space && (regs_get(vl, APIC_EXT_CONTROL) & EXT_IER) != 0;
}

// Whether they hold any vector back, of those that can be requested.
static bool ier_holds_back(const struct vlapic *vl) {
    if (!ier_on(vl)) {
        return false;
    }
    for (size_t i = 0; i < 8; i++) {
        uint32_t enabled = regs_get(vl, ier_reg(i));
        if (i == 0) {
            enabled |= 0xffff;
        }
        if (enabled != 0xffffffff) {
            return true;
        }
    }
    return false;
}

// The highest request that takes part in prioritisation: the others stay
// requested.
static int highest_request(const struct vlapic *vl) {
    if (!ier_on(vl)) {
        return highest(vl, APIC_IRR);
    }
    for (size_t i = 8; i-- > 0; ) {
        uint32_t word = regs_get(vl, APIC_IRR + (uint32_t)i * 0x10) & regs_get(vl, ier_reg(i));
        if (word != 0) {
            return i * 32 + 31 - __builtin_clz(word);
        }
    }
    return -1;
}

// Whether the local APIC matches xAPIC mode's physical destinations by 4-bit
// IDs, broadcast being xxxx1111b, where the guest left AMD's 8-bit ones off
// (APM 24593 rev. 3.46, 16.3.6).
static bool short_ids(const struct vlapic *vl) {
    return (ext_feature & EXT_APIC_ID) != 0 && !x2apic(vl)
        && (regs_get(vl, APIC_EXT_CONTROL) & EXT_APIC_ID) == 0;
}

static uint32_t tpr_get(const struct vlapic *vl) {
    return regs_get(vl, APIC_TPR) & 0xff;
}

static void tpr_set(struct vlapic *vl, uint32_t tpr) {
    regs_set(vl, APIC_TPR, tpr & 0xff);
}

static uint32_t ppr(const struct vlapic *vl) {
    uint32_t tpr = tpr_get(vl);
    int isrv = highest(vl, APIC_ISR);
    uint32_t isr_class = isrv < 0 ? 0 : (uint32_t)isrv & 0xf0;
    return (tpr & 0xf0) >= isr_class ? tpr : isr_class;
}

// What other processors requested, into the registers.
static void merge(struct vlapic *vl) {
    for (size_t i = 0; i < 8; i++) {
        uint32_t bits = atomic_exchange_explicit(&vl->pending[i], 0, memory_order_acquire);
        if (bits == 0) {
            continue;
        }
        size_t level = atomic_load_explicit(&vl->pending_level[i], memory_order_relaxed) & bits;
        uint32_t tmr = APIC_TMR + i * 0x10;
        regs_and(vl, tmr, ~(bits & ~level));
        regs_or(vl, tmr, level);
        regs_or(vl, APIC_IRR + i * 0x10, bits);
    }
    if (vl->posted == NULL
     || (atomic_load_explicit(&vl->posted->notification, memory_order_acquire) & POSTED_ON) == 0) {
        return;
    }
    atomic_fetch_and_explicit(&vl->posted->notification, ~POSTED_ON, memory_order_acq_rel);
    for (size_t i = 0; i < 8; i++) {
        uint32_t bits = atomic_exchange_explicit(&vl->posted->pir[i], 0, memory_order_acquire);
        if (bits != 0) {
            regs_and(vl, APIC_TMR + i * 0x10, ~bits);
            regs_or(vl, APIC_IRR + i * 0x10, bits);
        }
    }
}

// x2APIC mode has the whole ID in the register.
static void id_reg(struct vlapic *vl) {
    regs_set(vl, APIC_ID, x2apic(vl) ? vl->id : vl->id << 24);
}

// Mirrors the guest's logical destination where the processor's local APIC is
// in the same mode, for the interrupts the guest addresses that way.
static void mirror_logical(const struct vlapic *vl) {
    if (!x2apic(vl) && !lapic_x2apic()) {
        lapic_write(APIC_LDR, regs_get(vl, APIC_LDR));
        lapic_write(APIC_DFR, regs_get(vl, APIC_DFR));
    }
}

static uint32_t timer_mode(const struct vlapic *vl) {
    return regs_get(vl, APIC_LVT_TIMER) & LVT_TIMER_MODE_MASK;
}

// The divide configuration register's bits 3, 1, and 0 (SDM 325462-093 Vol.
// 3A, Figure 13-10): 2 to the power of their value plus 1, and 1 for 111b.
static uint32_t timer_divisor(const struct vlapic *vl) {
    uint32_t config = regs_get(vl, APIC_TIMER_DIVIDE);
    uint32_t shift = ((config & 3) | ((config & 8) >> 1)) + 1;
    return (uint32_t)1 << (shift & 7);
}

static uint64_t timer_span(const struct vlapic *vl) {
    return lapic_timer_to_tsc((uint64_t)regs_get(vl, APIC_TIMER_INITIAL) * timer_divisor(vl));
}

static void timer_stop(struct vlapic *vl) {
    vl->timer_deadline = 0;
    vl->timer_guest_deadline = 0;
}

static void timer_expire(struct vcpu *v, uint64_t now) {
    struct vlapic *vl = v->vlapic;
    uint32_t lvt = regs_get(vl, APIC_LVT_TIMER);
    if ((lvt & APIC_LVT_MASKED) == 0) {
        vlapic_accept(v, (uint8_t)lvt, false);
    }
    uint64_t span = timer_span(vl);
    if ((lvt & LVT_TIMER_MODE_MASK) != LVT_TIMER_PERIODIC || span == 0) {
        timer_stop(vl);
        return;
    }
    // Periods the hypervisor missed are gone, as interrupts in service take no
    // more of the same.
    vl->timer_deadline += span;
    if (vl->timer_deadline <= now) {
        vl->timer_deadline = now + span - (now - vl->timer_deadline) % span;
    }
}

static uint32_t timer_current(const struct vlapic *vl) {
    if (timer_mode(vl) == LVT_TIMER_TSC_DEADLINE || vl->timer_deadline == 0) {
        return 0;
    }
    uint64_t now = rdtsc();
    if (vl->timer_deadline <= now) {
        return 0;
    }
    uint64_t left = lapic_timer_from_tsc(vl->timer_deadline - now) / timer_divisor(vl);
    uint32_t initial = regs_get(vl, APIC_TIMER_INITIAL);
    return left < initial ? (uint32_t)left : initial;
}

// Leaving or entering TSC-deadline mode disarms the timer (SDM 325462-093 Vol.
// 3A, 13.5.4.1), and, as in Linux 7.3's KVM, clears the initial count.
static void timer_mode_changed(struct vlapic *vl, uint32_t old_mode) {
    if ((old_mode == LVT_TIMER_TSC_DEADLINE) != (timer_mode(vl) == LVT_TIMER_TSC_DEADLINE)) {
        regs_set(vl, APIC_TIMER_INITIAL, 0);
        timer_stop(vl);
    }
}

// The processor's entry for an interrupt of its own that the guest's entry at
// the same offset asks for: the hypervisor's vector while that asks for an
// interrupt or an NMI, and its SMIs, which go to the firmware, as the guest's
// entry has them.
static void host_lvt(const struct vlapic *vl, uint32_t reg, uint8_t vector) {
    uint32_t lvt = regs_get(vl, reg);
    uint32_t mode = lvt & LVT_DELIVERY_MASK;
    uint32_t host = APIC_LVT_MASKED;
    if (mode == LVT_DELIVERY_SMI) {
        host = lvt;
    } else if ((lvt & APIC_LVT_MASKED) == 0
            && (mode == LVT_DELIVERY_FIXED || mode == LVT_DELIVERY_NMI)) {
        host = vector;
    }
    lapic_write(reg, host);
}

// The processor's thermal interrupt.
static void thermal_lvt(const struct vlapic *vl) {
    if (lvt_present(LVT_THERMAL)) {
        host_lvt(vl, APIC_LVT_THERMAL, VLAPIC_THERMAL_VECTOR);
    }
}

// The processor's performance monitoring interrupt.
static void perf_lvt(const struct vlapic *vl) {
    if (lvt_present(LVT_PERF)) {
        host_lvt(vl, APIC_LVT_PERF, VLAPIC_PERF_VECTOR);
    }
}

// The processor's interrupts for its extended LVT entries, which the firmware
// assigns to sources such as MCA thresholding (PPR 57238 rev. 0.49,
// APICx5[0...3]0).
static void eilvt_lvt(const struct vlapic *vl, size_t i) {
    host_lvt(vl, eilvt_reg(i), (uint8_t)(VLAPIC_EILVT_VECTOR + i));
}

static void eilvt_lvts(const struct vlapic *vl) {
    for (size_t i = 0; i < eilvt_count; i++) {
        eilvt_lvt(vl, i);
    }
}

// As a local APIC in either mode reads it (SDM 325462-093 Vol. 3C, 32.1.6.2).
static uint32_t ipiv_destination(uint32_t id, bool x2apic) {
    return x2apic ? id : (id << 8) & 0xff00;
}

static void ipiv_recheck_soon(void) {
    if (ipi_virtualization) {
        atomic_store_explicit(&ipiv_recheck, true, memory_order_release);
    }
}

static void post_recheck_soon(struct vlapic *vl) {
    if (posted_interrupts || avic_supported) {
        vl->post_recheck = true;
    }
}

bool vlapic_post_changed(struct vcpu *v) {
    bool changed = v->vlapic->post_recheck;
    v->vlapic->post_recheck = false;
    return changed;
}

// The destinations change mode once every vCPU but those waiting for a
// start-up IPI is in the other: none virtualises IPIs then, as a vCPU only
// changes its mode out of the guest.
static void ipiv_reformat(void) {
    spinlock_acquire(&ipiv_lock);
    bool to = !atomic_load_explicit(&ipiv_x2apic, memory_order_acquire);
    bool all = true;
    for (size_t i = 0; i < smp_cpu_count() && all; i++) {
        struct cpu *cpu = smp_cpu(i);
        if (cpu->vcpu == NULL
         || atomic_load_explicit(&cpu->vcpu->sipi, memory_order_acquire) == VCPU_SIPI_WAITING) {
            continue;
        }
        uintptr_t base = atomic_load_explicit(
            (_Atomic(uint64_t) *)&cpu->apic_base, memory_order_acquire
        );
        all = base != 0 && ((base & APIC_BASE_EXTD) != 0) == to;
    }
    if (all) {
        // The IOMMUs read the destinations in the format they remap in, which
        // follows where it can, nothing being posted meanwhile, as an
        // interrupt to a descriptor in the other is blocked (D51397-019 rev.
        // 5.20, 5.2.3, 9.11). A processor still in xAPIC mode takes x2APIC
        // format's interrupts, which x2APIC mode comes after (SDM 325462-093
        // Vol. 3A, 13.12.7).
        bool posting = remap_posting();
        if (posting) {
            atomic_store_explicit(&post_paused, true, memory_order_release);
            pci_virt_repost();
            remap_x2apic(to);
        }
        for (size_t i = 0; i < smp_cpu_count(); i++) {
            struct vcpu *v = smp_cpu(i)->vcpu;
            if (v != NULL && v->vlapic != NULL) {
                atomic_store_explicit(
                    &v->vlapic->posted->destination, ipiv_destination(v->vlapic->id, to),
                    memory_order_release
                );
            }
        }
        atomic_store_explicit(&ipiv_x2apic, to, memory_order_release);
        if (posting) {
            atomic_store_explicit(&post_paused, false, memory_order_release);
            pci_virt_repost();
        }
    }
    spinlock_release(&ipiv_lock);
}

static void ipiv_entry(struct vcpu *v) {
    struct vlapic *vl = v->vlapic;
    if (atomic_exchange_explicit(&ipiv_recheck, false, memory_order_acq_rel)) {
        ipiv_reformat();
    }
    bool on = lapic_x2apic() == atomic_load_explicit(&ipiv_x2apic, memory_order_acquire);
    if (on != vl->ipiv) {
        vl->ipiv = on;
        vmx_write(VMCS_TERTIARY_CONTROLS, on ? PROC3_IPI_VIRT : 0);
        access_virtualisation(vl);
    }
}

static void avic_recheck_soon(void) {
    if (avic_supported) {
        atomic_store_explicit(&avic_recheck, true, memory_order_release);
    }
}

void vlapic_extint_moved(void) {
    avic_recheck_soon();
}

void vlapic_nested_changed(void) {
    avic_recheck_soon();
}

// AVIC finds the targets of logical destinations in a table of one vCPU per
// destination bit, all in the same format (APM 24593 rev. 3.45, 15.29.5.3):
// built from the software-enabled local APICs' LDRs and DFRs, with whether
// they fit it, as Linux 7.3's KVM has it.
static bool avic_logical_rebuild(void) {
    uint32_t table[AVIC_LOGICAL_ENTRIES] = { 0 };
    int mode = -1;
    bool fits = true;
    for (size_t i = 0; i < smp_cpu_count(); i++) {
        struct vcpu *to = smp_cpu(i)->vcpu;
        if (to == NULL || to->vlapic == NULL) {
            continue;
        }
        struct vlapic *vl = to->vlapic;
        uint32_t ldr = regs_get(vl, APIC_LDR);
        if (!enabled(vl) || !sw_enabled(vl) || x2apic(vl) || ldr == 0) {
            continue;
        }
        bool flat = (regs_get(vl, APIC_DFR) & DFR_FLAT) == DFR_FLAT;
        if (mode >= 0 && mode != flat) {
            fits = false;
        }
        mode = flat;
        uint32_t logical_id = ldr >> 24;
        uint32_t bits = flat ? logical_id : logical_id & 0xf;
        uint32_t cluster = flat ? 0 : logical_id >> 4;
        if ((bits & (bits - 1)) != 0 || vl->id > AVIC_MAX_ID) {
            fits = false;
            continue;
        }
        // Cluster 15 is reserved: the hypervisor delivers there.
        if (bits == 0 || cluster == 0xf) {
            continue;
        }
        uint32_t index = cluster * 4 + (uint32_t)__builtin_ctz(bits);
        if (table[index] != 0) {
            fits = false;
            continue;
        }
        table[index] = AVIC_LOGICAL_VALID | vl->id;
    }
    for (size_t i = 0; i < AVIC_LOGICAL_ENTRIES; i++) {
        if (avic_logical_table[i] != table[i]) {
            atomic_store_explicit(
                (_Atomic(uint32_t) *)&avic_logical_table[i], table[i], memory_order_release
            );
        }
    }
    return fits;
}

// Whether AVIC can run the local APICs, and whether the page behind theirs is
// to be mapped.
static bool avic_decide(bool *map) {
    spinlock_acquire(&avic_lock);
    // The guest's guests reach the page behind the local APICs' through the
    // nested page tables, where AVIC does not run them.
    bool want = avic_logical_rebuild() && !irq_extint_routed() && !nsvm_any_svm();
    spinlock_release(&avic_lock);
    bool any_xapic = false;
    bool any_x2apic = false;
    bool any_short_ids = false;
    uint32_t max_id = 0;
    for (size_t i = 0; i < smp_cpu_count(); i++) {
        struct vcpu *to = smp_cpu(i)->vcpu;
        if (to == NULL || to->vlapic == NULL) {
            continue;
        }
        // It delivers what AMD's interrupt enable registers hold back.
        want = want && !vlapic_extint_open(to) && !ier_holds_back(to->vlapic);
        any_short_ids = any_short_ids || short_ids(to->vlapic);
        if (x2apic(to->vlapic)) {
            any_x2apic = true;
        } else {
            any_xapic = true;
        }
        if (to->vlapic->id > max_id) {
            max_id = to->vlapic->id;
        }
    }
    // The IDs its tables hold in the modes (APM 24593 rev. 3.45, 15.29.4.3),
    // all of which xAPIC mode's broadcasts are to reach, and, before x2AVIC,
    // the 8-bit IDs its doorbells name (APM 24593 rev. 3.45, Figure 15-22).
    if (max_id > (svm_x2avic && !any_xapic ? X2AVIC_MAX_ID : AVIC_MAX_ID)) {
        want = false;
    }
    // Its table matches 8-bit IDs, and 4-bit ones match alike while every
    // vCPU's is below 15, their broadcast (BKDG 42301 rev. 3.14, 2.4.8.1.3):
    // AVIC exits for the destinations no vCPU has.
    if (any_short_ids && max_id >= 0xf) {
        want = false;
    }
    *map = want && !any_x2apic;
    return want;
}

// The other vCPUs are kept out of the guest while the page behind the local
// APICs' changes, and switch before they go back in. The guest's messages are
// posted to the vCPUs while AVIC runs them, and only then.
static void avic_update(void) {
    bool map;
    bool want = avic_decide(&map);
    if (want == atomic_load_explicit(&avic_wanted, memory_order_acquire)
     && map == atomic_load_explicit(&avic_mapped, memory_order_acquire)) {
        return;
    }
    vcpu_pause_others();
    want = avic_decide(&map);
    bool changed = want != atomic_load_explicit(&avic_wanted, memory_order_acquire);
    if (map != atomic_load_explicit(&avic_mapped, memory_order_acquire)) {
        spinlock_acquire(&page_lock);
        for (size_t i = 0; i < page_count; i++) {
            if (map) {
                gpt_map(pages[i], apic_access_page, PAGE_SIZE, GPT_RWX, MT_WB);
            } else {
                gpt_map(pages[i], 0, PAGE_SIZE, 0, 0);
            }
        }
        spinlock_release(&page_lock);
        gpt_flush();
    }
    atomic_store_explicit(&avic_mapped, map, memory_order_release);
    atomic_store_explicit(&avic_wanted, want, memory_order_release);
    vcpu_resume_others();
    if (changed) {
        pci_virt_repost();
    }
}

// Whether the vCPU is on its processor, in the guest or not, as far as AVIC's
// IPIs and the IOMMUs' doorbells go: they exit or are logged for the others,
// for the hypervisor to wake them.
static void avic_running(struct vcpu *v, bool running) {
    _Atomic(uint64_t) *entry = (_Atomic(uint64_t) *)&avic_physical_table[v->vlapic->id];
    uint64_t value = atomic_load_explicit(entry, memory_order_relaxed) & ~AVIC_ENTRY_RUNNING;
    atomic_store_explicit(entry, value | (running ? AVIC_ENTRY_RUNNING : 0), memory_order_seq_cst);
    remap_running(v->cpu->index, running);
}

static int avic_mode_for(struct vcpu *v) {
    // AVIC would redirect a stepped instruction's accesses to the local APIC's
    // page past the page the step maps there.
    if (!atomic_load_explicit(&avic_wanted, memory_order_acquire) || v->stepping) {
        return AVIC_OFF;
    }
    if (!x2apic(v->vlapic)) {
        return AVIC_XAPIC;
    }
    return svm_x2avic ? AVIC_X2APIC : AVIC_HYBRID;
}

static void avic_switch(struct vcpu *v, int mode) {
    struct vlapic *vl = v->vlapic;
    struct vmcb_control *c = &v->vmcb->control;
    c->vintr &= ~(SVM_VINTR_AVIC | SVM_VINTR_X2AVIC);
    if (mode != AVIC_OFF) {
        c->vintr |= SVM_VINTR_AVIC;
    }
    // x2AVIC takes the accesses to x2APIC MSRs the permission map passes, and
    // more IDs (APM 24593 rev. 3.45, 15.29.10).
    uint64_t max_id = AVIC_MAX_ID;
    c->msrpm_base_pa = virt_msr_bitmap();
    if (mode == AVIC_X2APIC) {
        c->vintr |= SVM_VINTR_X2AVIC;
        c->msrpm_base_pa = virt_msr_bitmap_x2apic();
        max_id = X2AVIC_MAX_ID;
    }
    c->avic_physical_table = (uint64_t)(uintptr_t)avic_physical_table | max_id;
    avic_running(v, mode != AVIC_OFF);
    // The TLB may hold translations of the local APIC's page from before,
    // which AVIC redirects now or no longer does, as Linux 7.3's KVM finds.
    svm_flush_guest_tlb(v);
    atomic_store_explicit(&vl->avic, mode, memory_order_seq_cst);
}

// The request is in the registers, which AVIC delivers from in the guest when
// told, and at the next entry anyway (APM 24593 rev. 3.45, 15.29.8.2).
static void avic_notify(struct vcpu *v) {
    if (v->cpu == this_cpu()) {
        return;
    }
    if (atomic_load_explicit(&v->vlapic->avic, memory_order_seq_cst) == AVIC_OFF) {
        vcpu_kick(v->cpu);
    } else if (atomic_load_explicit(&v->in_guest, memory_order_seq_cst)) {
        wrmsr(MSR_AVIC_DOORBELL, v->cpu->apic_id);
    } else if (atomic_load_explicit(&v->cpu->taking_interrupts, memory_order_seq_cst)) {
        vcpu_kick(v->cpu);
    }
}

static void reset_registers(struct vcpu *v) {
    struct vlapic *vl = v->vlapic;
    for (size_t i = 0; i < 8; i++) {
        atomic_store_explicit(&vl->pending[i], 0, memory_order_relaxed);
        if (vl->posted != NULL) {
            atomic_store_explicit(&vl->posted->pir[i], 0, memory_order_relaxed);
        }
        regs_set(vl, APIC_ISR + i * 0x10, 0);
        regs_set(vl, APIC_TMR + i * 0x10, 0);
        regs_set(vl, APIC_IRR + i * 0x10, 0);
    }
    id_reg(vl);
    regs_set(vl, APIC_VERSION, host_version);
    tpr_set(vl, 0);
    regs_set(vl, APIC_LDR, x2apic(vl) ? x2apic_ldr(vl->id) : 0);
    regs_set(vl, APIC_DFR, 0xffffffff);
    regs_set(vl, APIC_SVR, 0xff);
    regs_set(vl, APIC_ESR, 0);
    atomic_store_explicit(&vl->esr_pending, 0, memory_order_relaxed);
    regs_set(vl, APIC_ICR_LOW, 0);
    regs_set(vl, APIC_ICR_HIGH, 0);
    for (size_t i = 0; i < LVT_COUNT; i++) {
        regs_set(vl, lvt_regs[i], lvt_present(i) ? APIC_LVT_MASKED : 0);
    }
    regs_set(vl, APIC_TIMER_INITIAL, 0);
    regs_set(vl, APIC_TIMER_DIVIDE, 0);
    if (ext_space) {
        // 8-bit APIC IDs stay on: the firmware turns them on (BKDG 42301 rev.
        // 3.14, APIC410), Linux 7.3 never, and starting the APs of a processor
        // with more than 16 in xAPIC mode needs them.
        regs_set(vl, APIC_EXT_CONTROL, regs_get(vl, APIC_EXT_CONTROL) & EXT_APIC_ID);
        regs_set(vl, APIC_SEOI, 0);
        for (size_t i = 0; i < 8; i++) {
            regs_set(vl, ier_reg(i), 0xffffffff);
        }
        // Masked, as PPR 57238 rev. 0.49 has them, where APM 24593 rev. 3.46's
        // Table 16-2 has 0, which Linux 7.3 takes for an entry in use (apic.c,
        // eilvt_entry_is_changeable()).
        for (size_t i = 0; i < eilvt_count; i++) {
            regs_set(vl, eilvt_reg(i), APIC_LVT_MASKED);
        }
    }
    timer_stop(vl);
    thermal_lvt(vl);
    perf_lvt(vl);
    eilvt_lvts(vl);
    mirror_logical(vl);
    avic_recheck_soon();
    post_recheck_soon(vl);
}

void vlapic_reset(struct vcpu *v) {
    reset_registers(v);
    ipiv_recheck_soon();
}

void vlapic_create(struct vcpu *v) {
    struct vlapic *vl = kmalloc(sizeof(struct vlapic));
    *vl = (struct vlapic){
        .base = VLAPIC_DEFAULT_BASE | APIC_BASE_EN | (v->cpu->bsp ? APIC_BASE_BSP : 0),
        .id = v->cpu->apic_id
    };
    vl->regs = page_alloc(1);
    v->vlapic = vl;
    if (virt_vmx) {
        vmx_write(VMCS_VIRTUAL_APIC_ADDR, (uint64_t)(uintptr_t)vl->regs);
        vmx_write(VMCS_TPR_THRESHOLD, 0);
        if (apic_access_page != 0) {
            vmx_write(VMCS_APIC_ACCESS_ADDR, apic_access_page);
        }
        if (apicv) {
            vmx_write(
                VMCS_PROC_CONTROLS2,
                vmx_read(VMCS_PROC_CONTROLS2) | PROC2_APIC_REG_VIRT | PROC2_VIRT_INTR_DELIVERY
            );
        }
        if (posted_interrupts) {
            vl->posted = page_alloc(1);
            vmx_write(VMCS_PIN_CONTROLS, vmx_read(VMCS_PIN_CONTROLS) | PIN_POSTED_INTR);
            vmx_write(VMCS_POSTED_INTR_NV, VLAPIC_POSTED_VECTOR);
            vmx_write(VMCS_POSTED_INTR_DESC, (uint64_t)(uintptr_t)vl->posted);
            // The notifications that IPI virtualisation and the IOMMUs send.
            spinlock_acquire(&ipiv_lock);
            atomic_init(
                &vl->posted->notification, (uint32_t)VLAPIC_POSTED_VECTOR << POSTED_VECTOR_SHIFT
            );
            bool x2apic = atomic_load_explicit(&ipiv_x2apic, memory_order_acquire);
            atomic_init(&vl->posted->destination, ipiv_destination(vl->id, x2apic));
            spinlock_release(&ipiv_lock);
        }
        if (ipi_virtualization) {
            atomic_store_explicit(
                (_Atomic(uint64_t) *)&pid_table[vl->id], (uint64_t)(uintptr_t)vl->posted | 1,
                memory_order_release
            );
            vmx_write(VMCS_PROC_CONTROLS, vmx_read(VMCS_PROC_CONTROLS) | PROC_TERTIARY_CONTROLS);
            vmx_write(VMCS_TERTIARY_CONTROLS, 0);
            vmx_write(VMCS_PID_POINTER_TABLE, (uint64_t)(uintptr_t)pid_table);
            vmx_write(VMCS_LAST_PID_POINTER_INDEX, pid_table_last);
        }
    }
    if (avic_supported) {
        struct vmcb_control *c = &v->vmcb->control;
        c->avic_apic_bar = vl->base & APIC_BASE_ADDRESS;
        c->avic_backing_page = (uint64_t)(uintptr_t)vl->regs;
        c->avic_logical_table = (uint64_t)(uintptr_t)avic_logical_table;
        c->avic_physical_table = (uint64_t)(uintptr_t)avic_physical_table | AVIC_MAX_ID;
        // Broadcasts skip the vCPUs without one, rather than exit, as Linux
        // 7.3's KVM finds.
        if (vl->id <= X2AVIC_MAX_ID) {
            atomic_store_explicit(
                (_Atomic(uint64_t) *)&avic_physical_table[vl->id],
                (uint64_t)(uintptr_t)vl->regs | AVIC_ENTRY_VALID | vl->id, memory_order_release
            );
        }
    }
    reset_registers(v);
    access_virtualisation(vl);
    if (ext_space) {
        // Its processor's 8-bit APIC IDs, as the firmware left them.
        regs_set(vl, APIC_EXT_CONTROL, lapic_read(APIC_EXT_CONTROL) & ext_feature & EXT_APIC_ID);
    }
    if (v->cpu->bsp) {
        // Its LINT entries as lapic_legacy_init() set the processor's, and its
        // thermal entry and extended space as that left the firmware's.
        regs_set(vl, APIC_SVR, legacy_svr & SVR_WRITABLE);
        regs_set(vl, APIC_LVT_LINT0, legacy_lint0 & lvt_writable[LVT_LINT0]);
        regs_set(vl, APIC_LVT_LINT1, legacy_lint1 & lvt_writable[LVT_LINT1]);
        if (lvt_present(LVT_THERMAL)) {
            regs_set(vl, APIC_LVT_THERMAL, legacy_thermal & lvt_writable[LVT_THERMAL]);
            thermal_lvt(vl);
        }
        if (ext_space) {
            regs_set(vl, APIC_EXT_CONTROL, legacy_ext_control);
            for (size_t i = 0; i < 8; i++) {
                regs_set(vl, ier_reg(i), legacy_ier[i]);
            }
            for (size_t i = 0; i < eilvt_count; i++) {
                regs_set(vl, eilvt_reg(i), legacy_eilvt[i]);
            }
            eilvt_lvts(vl);
        }
    }
}

bool vlapic_enabled(struct vcpu *v) {
    return enabled(v->vlapic);
}

uint64_t vlapic_page(struct vcpu *v) {
    return v->vlapic->base & APIC_BASE_ADDRESS;
}

bool vlapic_virtual_delivery(struct vcpu *v) {
    // VMCS02 and VMCB02 deliver none of the guest's interrupts themselves.
    if (nvmx_in_l2(v) || nsvm_in_l2(v)) {
        return false;
    }
    return virt_vmx
         ? apicv
         : atomic_load_explicit(&v->vlapic->avic, memory_order_acquire) != AVIC_OFF;
}

void vlapic_accept(struct vcpu *v, uint8_t vector, bool level) {
    struct vlapic *vl = v->vlapic;
    if (!enabled(vl) || !sw_enabled(vl)) {
        return;
    }
    if (vector < 16) {
        atomic_fetch_or_explicit(
            &vl->esr_pending, ESR_RECEIVE_ILLEGAL_VECTOR, memory_order_relaxed
        );
        return;
    }
    if (!level && nvmx_posted_notification(v, vector)) {
        if (v->cpu != this_cpu()) {
            lapic_send_ipi(v->cpu->apic_id, ICR_DELIVERY_FIXED | VLAPIC_NESTED_POSTED_VECTOR);
        }
        return;
    }
    uint32_t bit = (uint32_t)1 << (vector % 32);
    if (!virt_vmx && atomic_load_explicit(&vl->avic, memory_order_seq_cst) != AVIC_OFF) {
        if (level) {
            regs_or(vl, vector_reg(APIC_TMR, vector), bit);
        } else {
            regs_and(vl, vector_reg(APIC_TMR, vector), ~bit);
        }
        regs_or(vl, vector_reg(APIC_IRR, vector), bit);
        avic_notify(v);
        return;
    }
    // A running guest takes it without an exit.
    if (vl->posted != NULL && !level && v->cpu != this_cpu()) {
        atomic_fetch_or_explicit(&vl->posted->pir[vector / 32], bit, memory_order_release);
        uint32_t notification = atomic_fetch_or_explicit(
            &vl->posted->notification, POSTED_ON, memory_order_acq_rel
        );
        if ((notification & POSTED_ON) == 0) {
            lapic_send_ipi(v->cpu->apic_id, ICR_DELIVERY_FIXED | VLAPIC_POSTED_VECTOR);
        }
        return;
    }
    if (level) {
        atomic_fetch_or_explicit(&vl->pending_level[vector / 32], bit, memory_order_relaxed);
    } else {
        atomic_fetch_and_explicit(&vl->pending_level[vector / 32], ~bit, memory_order_relaxed);
    }
    atomic_fetch_or_explicit(&vl->pending[vector / 32], bit, memory_order_release);
    if (v->cpu != this_cpu()) {
        vcpu_kick(v->cpu);
    }
}

void vlapic_timer_fired(struct vcpu *v) {
    v->vlapic->timer_armed = 0;
}

// As an SMI's time goes unnoticed, so does an exit's: a deadline that passes
// during one is due once the guest has had as long as the exit took, and no
// timer fires sooner, so that one shorter than an exit cannot starve the
// guest.
void vlapic_timer_program(struct vcpu *v) {
    struct vlapic *vl = v->vlapic;
    uint64_t now = rdtsc();
    uint64_t start = v->cpu->slice_start_tsc;
    uint64_t earliest = start != 0 ? now + (now - start) : now;
    if (vl->timer_deadline != 0 && vl->timer_deadline <= now) {
        if (vl->timer_deadline > start && vl->timer_held_until < vl->timer_deadline) {
            vl->timer_held_until = earliest;
        }
        if (now >= vl->timer_held_until) {
            timer_expire(v, now);
        }
    }
    uint64_t next = vl->timer_deadline;
    if (v->cpu->bsp) {
        uint64_t own = hv_timer_next();
        if (own != 0 && (next == 0 || own < next)) {
            next = own;
        }
    }
    if (next != 0 && next < earliest) {
        next = earliest;
    }
    if (next != vl->timer_armed) {
        vl->timer_armed = next;
        lapic_timer_arm(next);
    }
}

uint64_t vlapic_tsc_deadline(struct vcpu *v) {
    struct vlapic *vl = v->vlapic;
    return timer_mode(vl) == LVT_TIMER_TSC_DEADLINE && vl->timer_deadline != 0
         ? vl->timer_guest_deadline
         : 0;
}

void vlapic_tsc_deadline_set(struct vcpu *v, uint64_t deadline) {
    struct vlapic *vl = v->vlapic;
    if (timer_mode(vl) != LVT_TIMER_TSC_DEADLINE) {
        return;
    }
    if (deadline == 0) {
        timer_stop(vl);
        return;
    }
    vl->timer_guest_deadline = deadline;
    // 0 is no deadline at all.
    uint64_t own = deadline - v->tsc_offset;
    vl->timer_deadline = own != 0 ? own : 1;
}

void vlapic_tsc_offset_changed(struct vcpu *v) {
    uint64_t deadline = vlapic_tsc_deadline(v);
    if (deadline != 0) {
        vlapic_tsc_deadline_set(v, deadline);
    }
}

// What the vCPU's entry asks of a local interrupt that came.
static void local_interrupt(struct vcpu *v, uint32_t lvt) {
    if ((lvt & APIC_LVT_MASKED) != 0) {
        return;
    }
    if ((lvt & LVT_DELIVERY_MASK) == LVT_DELIVERY_FIXED) {
        vlapic_accept(v, (uint8_t)lvt, false);
    } else if ((lvt & LVT_DELIVERY_MASK) == LVT_DELIVERY_NMI) {
        atomic_store_explicit(&v->cpu->nmi_pending, true, memory_order_release);
    }
}

void vlapic_thermal_fired(struct vcpu *v) {
    local_interrupt(v, regs_get(v->vlapic, APIC_LVT_THERMAL));
}

void vlapic_perf_fired(struct vcpu *v) {
    struct vlapic *vl = v->vlapic;
    uint32_t lvt = regs_get(vl, APIC_LVT_PERF);
    // Intel's local APICs mask the entry as they deliver the interrupt, as the
    // processor's has (SDM 325462-093 Vol. 3A, 13.5.1); AMD's do not.
    if (virt_vmx && (lvt & APIC_LVT_MASKED) == 0) {
        regs_set(vl, APIC_LVT_PERF, lvt | APIC_LVT_MASKED);
    }
    local_interrupt(v, lvt);
}

void vlapic_eilvt_fired(struct vcpu *v, size_t entry) {
    if (entry < eilvt_count) {
        local_interrupt(v, regs_get(v->vlapic, eilvt_reg(entry)));
    }
}

int vlapic_pending(struct vcpu *v) {
    struct vlapic *vl = v->vlapic;
    if (!enabled(vl) || !sw_enabled(vl)) {
        return -1;
    }
    merge(vl);
    int irr = highest_request(vl);
    if (irr < 0 || ((uint32_t)irr & 0xf0) <= (ppr(vl) & 0xf0)) {
        return -1;
    }
    return irr;
}

uint32_t vlapic_cr8(struct vcpu *v) {
    return tpr_get(v->vlapic) >> 4;
}

void vlapic_set_cr8(struct vcpu *v, uint32_t value) {
    tpr_set(v->vlapic, value << 4);
}

int vlapic_tpr_held(struct vcpu *v) {
    struct vlapic *vl = v->vlapic;
    if (!enabled(vl) || !sw_enabled(vl)) {
        return -1;
    }
    merge(vl);
    int irr = highest_request(vl);
    int isrv = highest(vl, APIC_ISR);
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
    set_vector(vl, APIC_IRR, vector, false);
    set_vector(vl, APIC_ISR, vector, true);
}

bool vlapic_extint_open(struct vcpu *v) {
    struct vlapic *vl = v->vlapic;
    // A globally disabled local APIC leaves LINT0 the processor's INTR pin.
    if (!enabled(vl)) {
        return true;
    }
    uint32_t lvt = regs_get(vl, APIC_LVT_LINT0);
    return (lvt & APIC_LVT_MASKED) == 0 && (lvt & LVT_DELIVERY_MASK) == LVT_DELIVERY_EXTINT;
}

// The processor takes the requested and in-service vectors from the registers,
// and EOIs exit for the level-triggered vectors the I/O APICs wait on (SDM
// 325462-093 Vol. 3C, 27.4.2, 32.1.4).
static void virtual_delivery(struct vlapic *vl) {
    int irr = enabled(vl) && sw_enabled(vl) ? highest(vl, APIC_IRR) : -1;
    int isr = highest(vl, APIC_ISR);
    vmx_write(
        VMCS_GUEST_INTR_STATUS, (uint64_t)(isr < 0 ? 0 : isr) << 8 | (uint64_t)(irr < 0 ? 0 : irr)
    );
    bool broadcast = (regs_get(vl, APIC_SVR) & SVR_SUPPRESS_EOI_BROADCAST) == 0;
    for (size_t i = 0; i < 4; i++) {
        size_t level = 0;
        if (broadcast) {
            level = regs_get(vl, APIC_TMR + i * 0x20)
                  | (uint64_t)regs_get(vl, APIC_TMR + i * 0x20 + 0x10) << 32;
        }
        vmx_write(VMCS_EOI_EXIT0 + i * 2, level);
    }
}

void vlapic_prepare_entry(struct vcpu *v) {
    struct vlapic *vl = v->vlapic;
    merge(vl);
    // VMCS01's, which the guest's guest's VMCS is not.
    if (ipi_virtualization && !nvmx_in_l2(v)) {
        ipiv_entry(v);
    }
    if (!avic_supported) {
        return;
    }
    // A step holds the other vCPUs out already.
    if (!v->stepping && atomic_exchange_explicit(&avic_recheck, false, memory_order_acq_rel)) {
        avic_update();
    }
    int mode = avic_mode_for(v);
    if (mode != atomic_load_explicit(&vl->avic, memory_order_relaxed)) {
        avic_switch(v, mode);
    }
}

void vlapic_halt(struct vcpu *v, bool halting) {
    if (atomic_load_explicit(&v->vlapic->avic, memory_order_relaxed) != AVIC_OFF) {
        avic_running(v, !halting);
    }
}

static void posted_woken(uint32_t tag) {
    vcpu_kick(smp_cpu(tag));
}

void vlapic_posted_log(void) {
    remap_poll(posted_woken);
}

void vlapic_before_entry(struct vcpu *v) {
    struct vlapic *vl = v->vlapic;
    merge(vl);
    if (apicv) {
        virtual_delivery(vl);
        return;
    }
    // AVIC delivers from the registers itself, and keeps the PPR and V_TPR in
    // step with the TPR only for the guest's own changes (APM 24593 rev. 3.45,
    // 15.29.3.1).
    if (atomic_load_explicit(&vl->avic, memory_order_relaxed) != AVIC_OFF) {
        regs_set(vl, APIC_PPR, ppr(vl));
        struct vmcb_control *c = &v->vmcb->control;
        c->vintr = (c->vintr & ~(uint64_t)0xf) | (tpr_get(vl) >> 4);
        return;
    }
    if (virt_vmx) {
        // An exit when the guest lowers its TPR below what holds back.
        int held = vlapic_tpr_held(v);
        vmx_write(VMCS_TPR_THRESHOLD, held < 0 ? 0 : (uint64_t)held);
        return;
    }
    // VMCB02's V_TPR is the guest's guest's own.
    if (nsvm_in_l2(v)) {
        return;
    }
    struct vmcb_control *c = &v->vmcb->control;
    c->vintr = (c->vintr & ~(uint64_t)0xf) | (tpr_get(vl) >> 4);
}

void vlapic_after_exit(struct vcpu *v) {
    struct vlapic *vl = v->vlapic;
    if (virt_vmx || atomic_load_explicit(&vl->avic, memory_order_relaxed) != AVIC_OFF
     || nsvm_in_l2(v)) {
        return;
    }
    uint32_t vtpr = (uint32_t)(v->vmcb->control.vintr & 0xf);
    if (vtpr != tpr_get(vl) >> 4) {
        tpr_set(vl, vtpr << 4);
    }
}

static void end_interrupt(struct vlapic *vl, unsigned int vector) {
    set_vector(vl, APIC_ISR, vector, false);
    // Level-triggered ones end at the I/O APICs too, but where the guest ends
    // them there itself.
    if (vector_set(vl, APIC_TMR, vector)
     && (regs_get(vl, APIC_SVR) & SVR_SUPPRESS_EOI_BROADCAST) == 0) {
        irq_mirror_eoi((uint8_t)vector);
    }
}

static void eoi(struct vlapic *vl) {
    int vector = highest(vl, APIC_ISR);
    if (vector >= 0) {
        end_interrupt(vl, (unsigned int)vector);
    }
}

// AMD's specific EOI ends a vector in service whether or not it is the highest
// (APM 24593 rev. 3.46, 16.7.1), and one not in service, which PPR 57238 rev.
// 0.49 leaves undefined, is left alone.
static void specific_eoi(struct vlapic *vl, unsigned int vector) {
    if (vector_set(vl, APIC_ISR, vector)) {
        end_interrupt(vl, vector);
    }
}

void vlapic_dismiss(struct vcpu *v, uint8_t vector) {
    vlapic_take(v, vector);
    eoi(v->vlapic);
}

static bool matches(const struct vlapic *target, uint32_t dest, bool logical, bool x2apic_dest) {
    if (!logical) {
        if (x2apic_dest) {
            return dest == X2APIC_BROADCAST || dest == target->id;
        }
        if (short_ids(target)) {
            return (dest & 0xf) == 0xf || (dest & 0xf) == (target->id & 0xf);
        }
        return dest == XAPIC_BROADCAST || dest == (target->id & 0xff);
    }
    if (x2apic(target)) {
        if (dest == X2APIC_BROADCAST) {
            return true;
        }
        uint32_t ldr = regs_get(target, APIC_LDR);
        return dest >> 16 == ldr >> 16 && (dest & ldr & 0xffff) != 0;
    }
    uint32_t mda = dest & 0xff;
    uint32_t logical_id = regs_get(target, APIC_LDR) >> 24;
    if (mda == XAPIC_BROADCAST) {
        return true;
    }
    if ((regs_get(target, APIC_DFR) & DFR_FLAT) == DFR_FLAT) {
        return (mda & logical_id) != 0;
    }
    return mda >> 4 == logical_id >> 4 && (mda & logical_id & 0xf) != 0;
}

void vlapic_deliver_external(
    uint32_t dest, bool logical, uint8_t vector, bool level, bool lowest
) {
    for (size_t i = 0; i < smp_cpu_count(); i++) {
        struct vcpu *to = smp_cpu(i)->vcpu;
        if (to == NULL || !matches(to->vlapic, dest, logical, false)) {
            continue;
        }
        vlapic_accept(to, vector, level);
        if (lowest) {
            break;
        }
    }
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
            atomic_store_explicit(&to->cpu->nmi_pending, true, memory_order_release);
            vcpu_kick(to->cpu);
            break;
        }
        case ICR_DELIVERY_INIT: {
            // The sender takes its own once done with the write that sent it.
            if (atomic_load_explicit(&to->sipi, memory_order_acquire) == VCPU_SIPI_RUNNING) {
                atomic_store_explicit(&to->cpu->init_pending, true, memory_order_release);
                vcpu_kick(to->cpu);
            }
            break;
        }
        case ICR_DELIVERY_STARTUP: {
            // As on a processor, only a vCPU waiting for one takes it.
            int32_t waiting = VCPU_SIPI_WAITING;
            bool started = atomic_compare_exchange_strong_explicit(
                &to->sipi, &waiting, (int32_t)vector, memory_order_release, memory_order_relaxed
            );
            if (!started) {
                started = nvmx_sipi(to, vector);
            }
            if (started) {
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

struct cpu *vlapic_target(uint32_t dest, bool logical) {
    for (size_t i = 0; i < smp_cpu_count(); i++) {
        struct vcpu *to = smp_cpu(i)->vcpu;
        if (to != NULL && matches(to->vlapic, dest, logical, false)) {
            return to->cpu;
        }
    }
    return NULL;
}

struct cpu *vlapic_msi_target(uint32_t address) {
    return vlapic_target(
        address >> MSI_ADDRESS_DEST_SHIFT & 0xff, (address & MSI_ADDRESS_LOGICAL) != 0
    );
}

// The guest has no SMM, and a message has no 8259 to acknowledge: SMI and
// ExtINT messages go nowhere.
void vlapic_deliver_message(
    uint32_t dest, bool logical, uint32_t mode, uint8_t vector, bool level
) {
    if (mode == ICR_DELIVERY_FIXED || mode == ICR_DELIVERY_LOWEST) {
        vlapic_deliver_external(dest, logical, vector, level, mode == ICR_DELIVERY_LOWEST);
        return;
    }
    if (mode != ICR_DELIVERY_NMI && mode != ICR_DELIVERY_INIT) {
        return;
    }
    for (size_t i = 0; i < smp_cpu_count(); i++) {
        struct vcpu *to = smp_cpu(i)->vcpu;
        if (to != NULL && matches(to->vlapic, dest, logical, false)) {
            deliver(NULL, to, mode);
        }
    }
}

// The destination mode as the address's DM bit says and RH as lowest priority,
// as Linux 7.3's KVM takes them (irq.c, kvm_msi_to_lapic_irq(); lapic.c,
// kvm_lowest_prio_delivery()).
void vlapic_deliver_msi(uint32_t address, uint32_t data) {
    uint32_t mode = data & ICR_DELIVERY_MASK;
    if (mode == ICR_DELIVERY_FIXED && (address & MSI_ADDRESS_REDIRECT) != 0) {
        mode = ICR_DELIVERY_LOWEST;
    }
    vlapic_deliver_message(
        address >> MSI_ADDRESS_DEST_SHIFT & 0xff, (address & MSI_ADDRESS_LOGICAL) != 0, mode,
        data & ICR_VECTOR_MASK, (data & ICR_LEVEL) != 0
    );
}

// Only where posting does what vlapic_deliver_msi() does: fixed or lowest
// priority edge-triggered interrupts, of vectors past the exceptions', to one
// vCPU whose local APIC takes them; with VMX, while the IOMMUs read the
// descriptors' destinations in the format IPI virtualisation has them in, and
// with SVM, while AVIC runs the local APICs from their registers.
bool vlapic_post_target(
    uint32_t address, uint32_t data, size_t count, struct remap_target *target
) {
    if (virt_vmx) {
        bool x2apic_destinations = ipi_virtualization
                                && atomic_load_explicit(&ipiv_x2apic, memory_order_acquire);
        if (!posted_interrupts || atomic_load_explicit(&post_paused, memory_order_acquire)
         || x2apic_destinations != remap_x2apic_on()) {
            return false;
        }
    } else if (!atomic_load_explicit(&avic_wanted, memory_order_acquire)) {
        return false;
    }
    uint32_t mode = data & ICR_DELIVERY_MASK;
    bool lowest = mode == ICR_DELIVERY_LOWEST
               || (mode == ICR_DELIVERY_FIXED && (address & MSI_ADDRESS_REDIRECT) != 0);
    if ((address & MSI_ADDRESS_MASK) != MSI_ADDRESS_BASE
     || (mode != ICR_DELIVERY_FIXED && mode != ICR_DELIVERY_LOWEST) || (data & ICR_LEVEL) != 0
     || (data & ICR_VECTOR_MASK & ~(uint32_t)(count - 1)) < 16) {
        return false;
    }
    uint32_t dest = address >> MSI_ADDRESS_DEST_SHIFT & 0xff;
    bool logical = (address & MSI_ADDRESS_LOGICAL) != 0;
    struct vcpu *found = NULL;
    for (size_t i = 0; i < smp_cpu_count(); i++) {
        struct vcpu *to = smp_cpu(i)->vcpu;
        if (to == NULL || to->vlapic == NULL || !matches(to->vlapic, dest, logical, false)) {
            continue;
        }
        // A fixed interrupt to several is beyond one descriptor.
        if (found != NULL) {
            return false;
        }
        found = to;
        if (lowest) {
            break;
        }
    }
    if (found == NULL || !enabled(found->vlapic) || !sw_enabled(found->vlapic)) {
        return false;
    }
    *target = (struct remap_target){
        .descriptor = virt_vmx ? (uintptr_t)found->vlapic->posted : (uintptr_t)found->vlapic->regs,
        .tag = found->cpu->index,
        .apic_id = found->cpu->apic_id
    };
    return true;
}

static bool ipi_target(struct vcpu *from, struct vcpu *to, uint32_t dest, uint32_t command) {
    switch (command & ICR_SHORTHAND_MASK) {
        case ICR_SHORTHAND_SELF: {
            return to == from;
        }
        case ICR_SHORTHAND_ALL: {
            return true;
        }
        case ICR_SHORTHAND_OTHERS: {
            return to != from;
        }
        default: {
            return matches(to->vlapic, dest, (command & ICR_LOGICAL) != 0, x2apic(from->vlapic));
        }
    }
}

static void send_ipi(struct vcpu *from, uint32_t dest, uint32_t command) {
    uint32_t mode = command & ICR_DELIVERY_MASK;
    // The INIT level de-assert of old processors does nothing.
    if (mode == ICR_DELIVERY_INIT && (command & ICR_ASSERT) == 0) {
        return;
    }
    for (size_t i = 0; i < smp_cpu_count(); i++) {
        struct vcpu *to = smp_cpu(i)->vcpu;
        if (to == NULL || !ipi_target(from, to, dest, command)) {
            continue;
        }
        deliver(from, to, command);
        // A lowest priority interrupt goes to one processor.
        if (mode == ICR_DELIVERY_LOWEST) {
            break;
        }
    }
}

// Whether AMD's extended space has a register, as the processor's does.
static bool ext_present(uint32_t reg) {
    if (!ext_space) {
        return false;
    }
    if (reg == APIC_EXT_FEATURE || reg == APIC_EXT_CONTROL) {
        return true;
    }
    if (reg == APIC_SEOI) {
        return (ext_feature & EXT_SEOI) != 0;
    }
    if (reg >= APIC_IER && reg < ier_reg(8)) {
        return (ext_feature & EXT_IER) != 0;
    }
    return reg >= APIC_EILVT && reg < eilvt_reg(eilvt_count);
}

static void ext_write(struct vlapic *vl, uint32_t reg, uint32_t value) {
    if (reg == APIC_EXT_CONTROL) {
        regs_set(vl, APIC_EXT_CONTROL, value & ext_feature & EXT_CONTROL_BITS);
        avic_recheck_soon();
        post_recheck_soon(vl);
    } else if (reg == APIC_SEOI) {
        regs_set(vl, APIC_SEOI, value & 0xff);
        if ((regs_get(vl, APIC_EXT_CONTROL) & EXT_SEOI) != 0) {
            specific_eoi(vl, value & 0xff);
        }
    } else if (reg >= APIC_IER && reg < ier_reg(8)) {
        // The control register's bit enables writes to them too.
        if (ier_on(vl)) {
            regs_set(vl, reg, value);
            avic_recheck_soon();
        }
    } else if (reg >= APIC_EILVT) {
        value &= EILVT_WRITABLE;
        // A software disabled local APIC keeps them masked.
        if (!sw_enabled(vl)) {
            value |= APIC_LVT_MASKED;
        }
        regs_set(vl, reg, value);
        eilvt_lvt(vl, (reg - APIC_EILVT) / 0x10);
    }
}

// Returns false for registers that are not there.
static bool reg_read(struct vcpu *v, uint32_t reg, uint32_t *value) {
    struct vlapic *vl = v->vlapic;
    bool x2 = x2apic(vl);
    for (size_t i = 0; i < LVT_COUNT; i++) {
        if (reg == lvt_regs[i] && lvt_present(i)) {
            *value = regs_get(vl, reg);
            return true;
        }
    }
    if (reg >= APIC_ISR && reg < APIC_IRR + 0x80) {
        merge(vl);
        *value = regs_get(vl, reg);
        return true;
    }
    switch (reg) {
        case APIC_ID:
        case APIC_VERSION:
        case APIC_LDR:
        case APIC_SVR:
        case APIC_ESR:
        case APIC_TIMER_INITIAL:
        case APIC_TIMER_DIVIDE: {
            *value = regs_get(vl, reg);
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
        case APIC_ICR_LOW: {
            *value = regs_get(vl, APIC_ICR_LOW) & ~ICR_PENDING;
            return true;
        }
        case APIC_TIMER_CURRENT: {
            *value = timer_current(vl);
            return true;
        }
        default: {
            break;
        }
    }
    if (ext_present(reg)) {
        *value = reg == APIC_EXT_FEATURE ? ext_feature : regs_get(vl, reg);
        return true;
    }
    if (x2) {
        return false;
    }
    switch (reg) {
        case APIC_DFR:
        case APIC_ICR_HIGH: {
            *value = regs_get(vl, reg);
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
    for (size_t i = 0; i < LVT_COUNT; i++) {
        if (reg == lvt_regs[i] && lvt_present(i)) {
            value &= lvt_writable[i];
            // A software disabled local APIC keeps them masked.
            if (!sw_enabled(vl)) {
                value |= APIC_LVT_MASKED;
            }
            uint32_t old_mode = timer_mode(vl);
            if (i == LVT_TIMER && !tsc_deadline_mode) {
                value &= ~LVT_TIMER_TSC_DEADLINE;
            }
            regs_set(vl, reg, value);
            if (i == LVT_TIMER) {
                timer_mode_changed(vl, old_mode);
            }
            if (i == LVT_THERMAL) {
                thermal_lvt(vl);
            }
            if (i == LVT_PERF) {
                perf_lvt(vl);
            }
            if (i == LVT_LINT0) {
                avic_recheck_soon();
            }
            return true;
        }
    }
    switch (reg) {
        case APIC_ID: {
            // Read-only here, where a virtualised write may have changed it.
            id_reg(vl);
            return !x2;
        }
        case APIC_TPR: {
            tpr_set(vl, value);
            return true;
        }
        case APIC_EOI: {
            regs_set(vl, APIC_EOI, 0);
            eoi(vl);
            return !x2 || value == 0;
        }
        case APIC_SVR: {
            uint32_t writable = SVR_WRITABLE;
            if ((host_version & VERSION_EOI_SUPPRESSION) != 0) {
                writable |= SVR_SUPPRESS_EOI_BROADCAST;
            }
            regs_set(vl, APIC_SVR, value & writable);
            if (!sw_enabled(vl)) {
                for (size_t i = 0; i < LVT_COUNT; i++) {
                    if (lvt_present(i)) {
                        regs_set(vl, lvt_regs[i], regs_get(vl, lvt_regs[i]) | APIC_LVT_MASKED);
                    }
                }
                for (size_t i = 0; i < eilvt_count; i++) {
                    regs_set(vl, eilvt_reg(i), regs_get(vl, eilvt_reg(i)) | APIC_LVT_MASKED);
                }
                thermal_lvt(vl);
                perf_lvt(vl);
                eilvt_lvts(vl);
            }
            avic_recheck_soon();
            post_recheck_soon(vl);
            return true;
        }
        case APIC_ESR: {
            uint32_t esr = atomic_exchange_explicit(&vl->esr_pending, 0, memory_order_relaxed);
            regs_set(vl, APIC_ESR, esr);
            return !x2 || value == 0;
        }
        case APIC_TIMER_INITIAL: {
            // The IA32_TSC_DEADLINE MSR runs that mode (SDM 325462-093 Vol.
            // 3A, 13.5.4.1).
            if (timer_mode(vl) != LVT_TIMER_TSC_DEADLINE) {
                regs_set(vl, APIC_TIMER_INITIAL, value);
                vl->timer_deadline = value != 0 ? rdtsc() + timer_span(vl) : 0;
            }
            return true;
        }
        case APIC_TIMER_DIVIDE: {
            uint32_t old_divisor = timer_divisor(vl);
            regs_set(vl, APIC_TIMER_DIVIDE, value & TIMER_DIVIDE_WRITABLE);
            uint64_t now = rdtsc();
            if (timer_mode(vl) != LVT_TIMER_TSC_DEADLINE && vl->timer_deadline > now) {
                vl->timer_deadline = now
                                   + (vl->timer_deadline - now) * timer_divisor(vl) / old_divisor;
            }
            return true;
        }
        default: {
            break;
        }
    }
    if (ext_present(reg)) {
        ext_write(vl, reg, value);
        return true;
    }
    if (x2) {
        return false;
    }
    switch (reg) {
        case APIC_LDR: {
            regs_set(vl, APIC_LDR, value & 0xff000000);
            mirror_logical(vl);
            avic_recheck_soon();
            post_recheck_soon(vl);
            break;
        }
        case APIC_DFR: {
            regs_set(vl, APIC_DFR, value | 0x0fffffff);
            mirror_logical(vl);
            avic_recheck_soon();
            post_recheck_soon(vl);
            break;
        }
        case APIC_ICR_HIGH: {
            regs_set(vl, APIC_ICR_HIGH, value & 0xff000000);
            break;
        }
        case APIC_ICR_LOW: {
            regs_set(vl, APIC_ICR_LOW, value & ~ICR_PENDING);
            send_ipi(v, regs_get(vl, APIC_ICR_HIGH) >> 24, value);
            break;
        }
        default: {
            break;
        }
    }
    return true;
}

// x2APIC mode's ICR is one 64-bit register, which VMX keeps whole at its
// offset (SDM 325462-093 Vol. 3C, 32.5) and AVIC as xAPIC mode's two halves
// (APM 24593 rev. 3.45, 15.29.10).
static uint32_t x2apic_icr_high(void) {
    return virt_vmx ? APIC_ICR_LOW + 4 : APIC_ICR_HIGH;
}

static void x2apic_icr_write(struct vcpu *v, uint64_t value) {
    struct vlapic *vl = v->vlapic;
    regs_set(vl, APIC_ICR_LOW, (uint32_t)value & ~ICR_PENDING);
    regs_set(vl, x2apic_icr_high(), (uint32_t)(value >> 32));
    send_ipi(v, (uint32_t)(value >> 32), (uint32_t)value);
}

void vlapic_eoi_exit(struct vcpu *v, uint8_t vector) {
    (void)v;
    irq_mirror_eoi(vector);
}

void vlapic_avic_ipi_exit(struct vcpu *v, uint64_t info1, uint64_t info2) {
    struct vlapic *vl = v->vlapic;
    uint32_t low = (uint32_t)info1;
    uint32_t high = (uint32_t)(info1 >> 32);
    uint32_t dest = x2apic(vl) ? high : high >> 24;
    uint32_t id = (uint32_t)(info2 >> 32);
    // AVIC set the requests of every target.
    if (id == AVIC_IPI_NOT_RUNNING) {
        for (size_t i = 0; i < smp_cpu_count(); i++) {
            struct vcpu *to = smp_cpu(i)->vcpu;
            if (to != NULL && to != v && ipi_target(v, to, dest, low)) {
                avic_notify(to);
            }
        }
        return;
    }
    // Dropped, as Linux 7.3's KVM does: x2AVIC may leave a SELF_IPI write no
    // ICR to send it by (APM 24593 rev. 3.45, 15.29.10).
    if (id == AVIC_IPI_BAD_VECTOR) {
        return;
    }
    if (id == AVIC_IPI_BAD_BACKING_PAGE) {
        printf("AVIC found no backing page for IPI %#x to %#x\n", low, dest);
    }
    // Kinds and targets AVIC does not deliver, with none of their requests
    // set, the write done but maybe not its delivery status, as Linux 7.3's
    // KVM finds.
    regs_set(vl, APIC_ICR_LOW, low & ~ICR_PENDING);
    send_ipi(v, dest, low);
}

static bool avic_trapped(uint32_t offset) {
    if (svm_avic_ext_lvt_reads && offset >= APIC_EILVT && offset < eilvt_reg(APIC_EILVT_MAX)) {
        return true;
    }
    for (size_t i = 0; i < ARRAY_COUNT(avic_trapped_writes); i++) {
        if (avic_trapped_writes[i] == offset) {
            return true;
        }
    }
    return false;
}

void vlapic_avic_access_exit(struct vcpu *v, uint64_t info1) {
    uint32_t offset = (uint32_t)info1 & 0xff0;
    bool write = ((info1 >> 32) & 1) != 0;
    // The write is in the registers already, past its instruction.
    if (write && avic_trapped(offset)) {
        // Remote Read is read-only, and AMD's extended LVT entries past the
        // processor's last read as none.
        if (offset == APIC_RRD || (offset >= APIC_EILVT && !ext_present(offset))) {
            regs_set(v->vlapic, offset, 0);
        } else {
            vlapic_apic_write(v, offset);
        }
        return;
    }
    // Others exit before the access, in x2APIC mode a RDMSR's or WRMSR's,
    // which leaves no next RIP (APM 24593 rev. 3.45, 15.7.1).
    if (x2apic(v->vlapic)) {
        uint32_t msr = (uint32_t)v->gprs.rcx;
        uint64_t value = (uint64_t)(uint32_t)v->gprs.rdx << 32 | (uint32_t)v->gprs.rax;
        bool fault = false;
        if (write) {
            vlapic_wrmsr(v, msr, value, &fault);
        } else if (vlapic_rdmsr(v, msr, &value, &fault) && !fault) {
            v->gprs.rax = (uint32_t)value;
            v->gprs.rdx = value >> 32;
        }
        if (fault) {
            vcpu_inject_exception(v, 13, true, 0);
        } else {
            vcpu_advance(v, insn_length_intercepted(v));
        }
        return;
    }
    mmio_fault(v, vlapic_page(v) + offset);
}

void vlapic_apic_write(struct vcpu *v, uint32_t offset) {
    struct vlapic *vl = v->vlapic;
    // A self-IPI with a vector that is not one (SDM 325462-093 Vol. 3C, 32.5).
    if (offset == APIC_SELF_IPI) {
        vlapic_accept(v, (uint8_t)regs_get(vl, APIC_SELF_IPI), false);
        return;
    }
    if (offset == APIC_ICR_LOW && x2apic(vl)) {
        x2apic_icr_write(
            v, (uint64_t)regs_get(vl, x2apic_icr_high()) << 32 | regs_get(vl, APIC_ICR_LOW)
        );
        return;
    }
    reg_write(v, offset, regs_get(vl, offset));
}

// Where the page is not the vCPU's xAPIC, it is not there.
static bool xapic_at(const struct vlapic *vl, uintptr_t page) {
    return enabled(vl) && !x2apic(vl) && page == (vl->base & APIC_BASE_ADDRESS);
}

// Registers are 32 bits wide, in the first 32 bits of each 16-byte slot.
static uint32_t dword_read(struct vcpu *v, uintptr_t page, uint64_t offset) {
    if (!xapic_at(v->vlapic, page)) {
        return 0xffffffff;
    }
    uint32_t value = 0;
    if ((offset & 0xc) != 0 || !reg_read(v, (uint32_t)offset, &value)) {
        return 0;
    }
    return value;
}

static void dword_write(struct vcpu *v, uintptr_t page, uint64_t offset, uint32_t value) {
    if (xapic_at(v->vlapic, page) && (offset & 0xc) == 0) {
        reg_write(v, (uint32_t)offset, value);
    }
}

static uint64_t mmio_read(struct vcpu *v, void *ctx, uint64_t offset, size_t size) {
    uintptr_t page = (uintptr_t)ctx;
    if (size == 8) {
        return dword_read(v, page, offset) | (uint64_t)dword_read(v, page, offset + 4) << 32;
    }
    uint64_t mask = ((uint64_t)1 << (size * 8)) - 1;
    return (dword_read(v, page, offset & ~(uint64_t)3) >> ((offset & 3) * 8)) & mask;
}

static void mmio_write(struct vcpu *v, void *ctx, uint64_t offset, size_t size, uint64_t value) {
    uintptr_t page = (uintptr_t)ctx;
    if (size == 8) {
        dword_write(v, page, offset, (uint32_t)value);
        dword_write(v, page, offset + 4, (uint32_t)(value >> 32));
        return;
    }
    // Narrower writes change part of a register.
    unsigned int shift = (offset & 3) * 8;
    uint32_t mask = (uint32_t)(size == 4 ? (uint32_t)0xffffffff : ((uint32_t)1 << (size * 8)) - 1)
                 << shift;
    uint32_t old = size == 4 ? 0 : dword_read(v, page, offset & ~(uint64_t)3);
    dword_write(
        v, page, offset & ~(uint64_t)3, (old & ~mask) | (((uint32_t)value << shift) & mask)
    );
}

static const struct mmio_ops mmio_ops = { mmio_read, mmio_write, false };

static bool apic_base_write(struct vcpu *v, uint64_t value) {
    struct vlapic *vl = v->vlapic;
    uint64_t reserved = 0xff | ((uint64_t)1 << 9)
                      | ~(((uint64_t)1 << cpu_physical_address_bits()) - 1);
    if (!lapic_x2apic_supported() || x2apic_withheld) {
        reserved |= APIC_BASE_EXTD;
    }
    if ((value & reserved) != 0) {
        return false;
    }
    bool en = (value & APIC_BASE_EN) != 0;
    bool extd = (value & APIC_BASE_EXTD) != 0;
    bool was_en = enabled(vl);
    bool was_x2 = x2apic(vl);
    // EXTD without EN, x2APIC mode straight from disabled, and xAPIC mode
    // straight from x2APIC mode are not allowed.
    if ((extd && !en) || (extd && !was_en) || (was_x2 && en && !extd)) {
        return false;
    }

    vl->base = (value & ~APIC_BASE_BSP) | (vl->base & APIC_BASE_BSP);
    access_virtualisation(vl);
    if (avic_supported) {
        v->vmcb->control.avic_apic_bar = vl->base & APIC_BASE_ADDRESS;
        svm_flush_guest_tlb(v);
        avic_recheck_soon();
    }
    post_recheck_soon(vl);
    if (!en) {
        if (was_en) {
            reset_registers(v);
        }
        return true;
    }
    if (extd != was_x2 || !was_en) {
        lapic_host_mode(extd);
        // The way there may have taken the timer's count with it.
        vl->timer_armed = 0;
        ipiv_recheck_soon();
        if (!was_en) {
            reset_registers(v);
        }
        if (extd) {
            // x2APIC mode derives the LDR, and keeps no ICR high half.
            regs_set(vl, APIC_LDR, x2apic_ldr(vl->id));
            regs_set(vl, APIC_ICR_HIGH, 0);
            regs_set(vl, x2apic_icr_high(), 0);
        }
        id_reg(vl);
        thermal_lvt(vl);
        perf_lvt(vl);
        eilvt_lvts(vl);
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
        *value |= (uint64_t)regs_get(vl, x2apic_icr_high()) << 32;
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
        x2apic_icr_write(v, value);
    } else if (msr == MSR_X2APIC_SELF_IPI) {
        vlapic_accept(v, (uint8_t)value, false);
    } else {
        uint32_t reg = (msr - MSR_X2APIC_FIRST) << 4;
        uint32_t ignored;
        bool readable = reg == APIC_EOI || reg_read(v, reg, &ignored);
        if (!readable || (value >> 32) != 0 || reg == APIC_ID || reg == APIC_VERSION
         || reg == APIC_PPR || reg == APIC_LDR || reg == APIC_TIMER_CURRENT
         || reg == APIC_EXT_FEATURE || (reg >= APIC_ISR && reg < APIC_IRR + 0x80)
         || !reg_write(v, reg, (uint32_t)value)) {
            *fault = true;
        }
    }
    return true;
}
