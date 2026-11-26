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
#include <virt/virt.h>
#include <hw/power.h>
#include <lib/log.h>
#include <lib/printf.h>
#include <lib/time.h>
#include <lib/util.h>
#include <memory.h>
#include <sys/cache.h>
#include <sys/cpu.h>
#include <sys/errata.h>
#include <sys/mm.h>
#include <sys/x86.h>
#include <virt/event.h>
#include <virt/gpt.h>
#include <virt/msr.h>
#include <virt/svm.h>
#include <virt/vcpu.h>
#include <virt/vmx.h>

// CPUID 80000001H's ECX: LAHF and SAHF in 64-bit mode.
#define CPUID_80000001_ECX_LAHF_SAHF ((uint32_t)1 << 0)

#define RESET_HOLD_SECONDS 30

bool virt_vmx;
unsigned int virt_breakpoint_masks;

static uint8_t *io_bitmap;
// The same but for the port virt_io_lend() gave, which it lets through.
static uint8_t *io_bitmap_lent;
static uint32_t lent_port = UINT32_MAX;
static uint8_t *msr_bitmap;
// The accesses to MSRs a module has chosen for, as their bits in msr_bitmap.
static uint8_t *msr_claimed;
// For vCPUs whose x2APIC the processor virtualises: the same but for the
// accesses to x2APIC registers it passes, and for those whose IPIs it
// virtualises too, the ICR's as well.
static uint8_t *msr_bitmap_x2apic;
static uint8_t *msr_bitmap_x2apic_icr;
// The three alike but letting IA32_TSC_DEADLINE through, for vCPUs whose APIC
// timer the processor virtualises.
static uint8_t *msr_bitmap_deadline[3];

static size_t describe_guest(char *buf, size_t size) {
    struct vcpu *v = this_cpu()->vcpu;
    if (v == NULL) {
        return 0;
    }
    int len = snprintf(
        buf, size, "Its guest last exited with %s %#x, at RIP %#llx\n",
        virt_vmx ? "basic reason" : "code",
        atomic_load_explicit(&v->last_exit, memory_order_relaxed),
        (unsigned long long)vcpu_rip(v)
    );
    return len < 0 ? 0 : (size_t)len;
}

void virt_reset(bool shown) {
    if (shown) {
        printf("Resetting in %u seconds\n", RESET_HOLD_SECONDS);
        time_udelay(RESET_HOLD_SECONDS * (uint64_t)1000000);
    }
    power_reset(false);
}

// Runs load the guest's status flags with SAHF (alu.S), which 64-bit mode has
// where CPUID says so (SDM 325462-093 Vol. 2B, SAHF; APM 40332 rev. 4.11, Vol.
// 3, SAHF), as every processor with VMX's unrestricted guest or SVM's nested
// paging does.
#define NO_LAHF_SAHF "the processor lacks LAHF and SAHF in 64-bit mode"

static bool lahf_sahf(void) {
    return (cpuid(0x80000001, 0).ecx & CPUID_80000001_ECX_LAHF_SAHF) != 0;
}

const char *virt_unusable(bool *in_setup) {
    if (!lahf_sahf()) {
        *in_setup = false;
        return NO_LAHF_SAHF;
    }
    if (x86_cpu_vendor() == X86_CPU_VENDOR_INTEL) {
        return vmx_unusable(in_setup);
    }
    return svm_unusable(in_setup);
}

bool virt_init(void) {
    bool intel = x86_cpu_vendor() == X86_CPU_VENDOR_INTEL;
    if (!lahf_sahf()) {
        printf("Not running: %s\n", NO_LAHF_SAHF);
        return false;
    }

    if (intel && vmx_supported()) {
        virt_vmx = true;
        // I/O bitmaps A and B, then the MSR bitmap.
        io_bitmap = page_alloc(VMX_IO_BITMAP_PAGES);
        io_bitmap_lent = page_alloc(VMX_IO_BITMAP_PAGES);
        msr_bitmap = page_alloc(VMX_MSR_BITMAP_PAGES);
        msr_claimed = page_alloc(VMX_MSR_BITMAP_PAGES);
        // Only the MSRs the guest gets go to the processor (msr.c).
        memset(msr_bitmap, 0xff, VMX_MSR_BITMAP_PAGES * PAGE_SIZE);
        gpt_init(true, vmx_ept_huge_pages(), vmx_ept_dirty_flags());
    } else if (!intel && svm_supported()) {
        virt_vmx = false;
        io_bitmap = page_alloc(SVM_IOPM_PAGES);
        io_bitmap_lent = page_alloc(SVM_IOPM_PAGES);
        msr_bitmap = page_alloc(SVM_MSRPM_PAGES);
        msr_claimed = page_alloc(SVM_MSRPM_PAGES);
        memset(msr_bitmap, 0xff, SVM_MSRPM_PAGES * PAGE_SIZE);
        // Nested paging's tables keep dirty flags as page tables do (APM 24593
        // rev. 3.45, 15.25.5).
        gpt_init(false, svm_huge_pages(), true);
        // Only nested paging gives the guest its own view of these.
        virt_intercept_msr(MSR_EFER, true, true);
        virt_intercept_msr(MSR_IA32_PAT, true, true);
        virt_intercept_msr(MSR_VM_CR, true, true);
        virt_intercept_msr(MSR_VM_HSAVE_PA, true, true);
        uint32_t features = cpuid(0x80000001, 0).ecx;
        virt_breakpoint_masks = 0;
        if ((features & ((uint32_t)1 << 30)) != 0) {
            virt_breakpoint_masks = 4;
        } else if ((features & ((uint32_t)1 << 26)) != 0) {
            virt_breakpoint_masks = 1;
        }
    } else {
        return false;
    }

    cpu_interrupt_handler(event_interrupt);
    cpu_guest_describer(describe_guest);
    printf("Using %s\n", virt_vmx ? "VMX" : "SVM");
    return true;
}

void virt_cpu_init(void) {
    // The same control registers on every processor, whatever the firmware
    // left on each: caching on; x87 and SSE usable with x87 errors raising
    // #MF, as VMX needs and as UEFI 2.11, 2.3.4.1, has its runtime services
    // called; machine checks taken, which with SVM's HOST_MCE_OVERRIDE also
    // makes those in a guest whose CR4.MCE is clear exit rather than shut the
    // processor down (APM 24593 rev. 3.46, 15.12.18); XSETBV, which the host
    // executes for the guest; and the paging mode, which cannot change. The
    // host needs none of the protections the firmware may have turned on, and
    // control-flow enforcement would need shadow stacks it lacks.
    uintptr_t cr4 = CR4_PAE | CR4_MCE | CR4_OSFXSR | CR4_OSXMMEXCPT | (read_cr4() & CR4_LA57);
    if ((cpuid(1, 0).ecx & ((uint32_t)1 << 26)) != 0) {
        cr4 |= CR4_OSXSAVE;
    }
    write_cr4(cr4);
    write_cr0(CR0_PE | CR0_MP | CR0_ET | CR0_NE | CR0_WP | CR0_PG);
    cache_cpu_init(GPT_HOST_PAT);
    errata_cpu_init();

    if (virt_vmx) {
        vmx_cpu_init();
    } else {
        svm_cpu_init();
    }
    msr_cpu_init();
}

void virt_cpu_off(void) {
    if (virt_vmx) {
        vmx_cpu_off();
    }
}

uintptr_t virt_io_bitmap(void) {
    return (uintptr_t)io_bitmap;
}

uintptr_t virt_io_bitmap_lent(void) {
    return (uintptr_t)io_bitmap_lent;
}

uintptr_t virt_msr_bitmap(void) {
    return (uintptr_t)msr_bitmap;
}

uintptr_t virt_msr_bitmap_x2apic(void) {
    return (uintptr_t)msr_bitmap_x2apic;
}

uintptr_t virt_msr_bitmap_x2apic_icr(void) {
    return (uintptr_t)msr_bitmap_x2apic_icr;
}

static void bitmap_set(uint8_t *bitmap, unsigned int bit, bool set) {
    if (set) {
        bitmap[bit / 8] |= 1 << (bit % 8);
    } else {
        bitmap[bit / 8] &= ~(1 << (bit % 8));
    }
}

void virt_intercept_io(uint16_t port, size_t count, bool intercept) {
    for (size_t i = 0; i < count; i++) {
        unsigned int p = port + i;
        bitmap_set(io_bitmap, p, intercept);
        bitmap_set(io_bitmap_lent, p, intercept && p != lent_port);
    }
}

bool virt_io_intercepted(uint16_t port) {
    return (io_bitmap[port / 8] & (1 << (port % 8))) != 0;
}

void virt_io_lend(uint16_t port) {
    lent_port = port;
    bitmap_set(io_bitmap_lent, port, false);
}

// Where an MSR's read and write bits are: VMX's read bitmaps for the low MSRs
// and the high ones, then their write bitmaps; SVM's two bits per MSR, in
// three ranges. Others always exit.
static bool msr_bits(uint32_t msr, unsigned int *read, unsigned int *write) {
    if (virt_vmx) {
        size_t base;
        if (msr <= 0x1fff) {
            base = 0;
        } else if (msr >= 0xc0000000 && msr <= 0xc0001fff) {
            base = 0x400 * 8;
        } else {
            return false;
        }
        *read = base + (msr & 0x1fff);
        *write = *read + 0x800 * 8;
        return true;
    }
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
    *read = offset * 8 + (msr & 0x1fff) * 2;
    *write = *read + 1;
    return true;
}

// Follows a change of the other bitmaps in the deadline ones, but for
// IA32_TSC_DEADLINE, and for the x2APIC registers in those of the x2APIC
// bitmaps.
static void deadline_set(unsigned int bit, bool set, bool x2apic_register, uint32_t msr) {
    if (msr == MSR_IA32_TSC_DEADLINE) {
        return;
    }
    for (size_t i = 0; i < ARRAY_COUNT(msr_bitmap_deadline); i++) {
        if (msr_bitmap_deadline[i] != NULL && (i == 0 || !x2apic_register)) {
            bitmap_set(msr_bitmap_deadline[i], bit, set);
        }
    }
}

void virt_intercept_msr(uint32_t msr, bool read, bool write) {
    unsigned int bits[2];
    if (!msr_bits(msr, &bits[0], &bits[1])) {
        return;
    }
    bool chosen[] = { read, write };
    bool x2apic_register = msr >= 0x800 && msr <= 0x8ff;
    for (size_t i = 0; i < 2; i++) {
        if (!chosen[i]) {
            continue;
        }
        bitmap_set(msr_claimed, bits[i], true);
        bitmap_set(msr_bitmap, bits[i], true);
        if (msr_bitmap_x2apic != NULL && !x2apic_register) {
            bitmap_set(msr_bitmap_x2apic, bits[i], true);
        }
        if (msr_bitmap_x2apic_icr != NULL && !x2apic_register) {
            bitmap_set(msr_bitmap_x2apic_icr, bits[i], true);
        }
        deadline_set(bits[i], true, x2apic_register, msr);
    }
}

void virt_pass_msr(uint32_t msr) {
    unsigned int bits[2];
    if (!msr_bits(msr, &bits[0], &bits[1])) {
        return;
    }
    // The x2APIC registers' bits in the x2APIC bitmaps are the local APIC's.
    bool x2apic_register = msr >= 0x800 && msr <= 0x8ff;
    for (size_t i = 0; i < 2; i++) {
        if ((msr_claimed[bits[i] / 8] & (1 << (bits[i] % 8))) != 0) {
            continue;
        }
        bitmap_set(msr_bitmap, bits[i], false);
        if (!x2apic_register && msr_bitmap_x2apic != NULL) {
            bitmap_set(msr_bitmap_x2apic, bits[i], false);
        }
        if (!x2apic_register && msr_bitmap_x2apic_icr != NULL) {
            bitmap_set(msr_bitmap_x2apic_icr, bits[i], false);
        }
        deadline_set(bits[i], false, x2apic_register, msr);
    }
}

void virt_x2apic_msr_passed(uint32_t msr, bool read, bool write) {
    size_t pages = virt_vmx ? VMX_MSR_BITMAP_PAGES : SVM_MSRPM_PAGES;
    if (msr_bitmap_x2apic == NULL) {
        msr_bitmap_x2apic = page_alloc(pages);
        memcpy(msr_bitmap_x2apic, msr_bitmap, pages * PAGE_SIZE);
    }
    unsigned int read_bit, write_bit;
    if (!msr_bits(msr, &read_bit, &write_bit)) {
        return;
    }
    if (read) {
        bitmap_set(msr_bitmap_x2apic, read_bit, false);
    }
    if (write) {
        bitmap_set(msr_bitmap_x2apic, write_bit, false);
    }
}

void virt_x2apic_icr_passed(uint32_t msr) {
    size_t pages = virt_vmx ? VMX_MSR_BITMAP_PAGES : SVM_MSRPM_PAGES;
    msr_bitmap_x2apic_icr = page_alloc(pages);
    memcpy(msr_bitmap_x2apic_icr, msr_bitmap_x2apic, pages * PAGE_SIZE);
    unsigned int read_bit, write_bit;
    if (msr_bits(msr, &read_bit, &write_bit)) {
        bitmap_set(msr_bitmap_x2apic_icr, read_bit, false);
        bitmap_set(msr_bitmap_x2apic_icr, write_bit, false);
    }
}

void virt_deadline_msr_passed(void) {
    const uint8_t *from[] = { msr_bitmap, msr_bitmap_x2apic, msr_bitmap_x2apic_icr };
    unsigned int read_bit, write_bit;
    msr_bits(MSR_IA32_TSC_DEADLINE, &read_bit, &write_bit);
    for (size_t i = 0; i < ARRAY_COUNT(msr_bitmap_deadline); i++) {
        if (from[i] == NULL) {
            continue;
        }
        msr_bitmap_deadline[i] = page_alloc(VMX_MSR_BITMAP_PAGES);
        memcpy(msr_bitmap_deadline[i], from[i], VMX_MSR_BITMAP_PAGES * PAGE_SIZE);
        bitmap_set(msr_bitmap_deadline[i], read_bit, false);
        bitmap_set(msr_bitmap_deadline[i], write_bit, false);
    }
}

uintptr_t virt_msr_bitmap_for(bool x2apic, bool icr, bool deadline) {
    size_t i = 0;
    if (x2apic) {
        i = icr ? 2 : 1;
    }
    uint8_t *const plain[] = { msr_bitmap, msr_bitmap_x2apic, msr_bitmap_x2apic_icr };
    if (deadline && msr_bitmap_deadline[i] != NULL) {
        return (uintptr_t)msr_bitmap_deadline[i];
    }
    return (uintptr_t)plain[i];
}
