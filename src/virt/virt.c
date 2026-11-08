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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <virt/virt.h>
#include <hw/power.h>
#include <lib/log.h>
#include <lib/printf.h>
#include <lib/time.h>
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

#define RESET_HOLD_SECONDS 30

bool virt_vmx;
unsigned int virt_breakpoint_masks;

static uint8_t *io_bitmap;
static uint8_t *msr_bitmap;
// The accesses to MSRs a module has chosen for, as their bits in msr_bitmap.
static uint8_t *msr_claimed;
// For vCPUs whose x2APIC the processor virtualises: the same but for the
// accesses to x2APIC registers it passes, and for those whose IPIs it
// virtualises too, the ICR's as well.
static uint8_t *msr_bitmap_x2apic;
static uint8_t *msr_bitmap_x2apic_icr;

static size_t describe_guest(char *buf, size_t size) {
    struct vcpu *v = this_cpu()->vcpu;
    if (v == NULL) {
        return 0;
    }
    int len = snprintf(
        buf, size, "Its guest last exited with %s %#x, at RIP %#llx\n",
        virt_vmx ? "basic reason" : "code", v->last_exit, (unsigned long long)vcpu_rip(v)
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

bool virt_init(void) {
    bool intel = x86_cpu_vendor() == X86_CPU_VENDOR_INTEL;

    if (intel && vmx_supported()) {
        virt_vmx = true;
        // I/O bitmaps A and B, then the MSR bitmap.
        io_bitmap = page_alloc(VMX_IO_BITMAP_PAGES);
        msr_bitmap = page_alloc(VMX_MSR_BITMAP_PAGES);
        msr_claimed = page_alloc(VMX_MSR_BITMAP_PAGES);
        // Only the MSRs the guest gets go to the processor (msr.c).
        memset(msr_bitmap, 0xff, VMX_MSR_BITMAP_PAGES * PAGE_SIZE);
        gpt_init(true, vmx_ept_huge_pages(), vmx_ept_dirty_flags());
    } else if (!intel && svm_supported()) {
        virt_vmx = false;
        io_bitmap = page_alloc(SVM_IOPM_PAGES);
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
        virt_breakpoint_masks = (features & ((uint32_t)1 << 30)) != 0
                              ? 4
                              : (features & ((uint32_t)1 << 26)) != 0 ? 1 : 0;
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

uintptr_t virt_io_bitmap(void) {
    return (uintptr_t)io_bitmap;
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

void virt_intercept_io(uint16_t port, size_t count, bool intercept) {
    for (size_t i = 0; i < count; i++) {
        unsigned int p = port + i;
        if (intercept) {
            io_bitmap[p / 8] |= 1 << (p % 8);
        } else {
            io_bitmap[p / 8] &= ~(1 << (p % 8));
        }
    }
}

bool virt_io_intercepted(uint16_t port) {
    return (io_bitmap[port / 8] & (1 << (port % 8))) != 0;
}

static void bitmap_set(uint8_t *bitmap, unsigned int bit, bool set) {
    if (set) {
        bitmap[bit / 8] |= 1 << (bit % 8);
    } else {
        bitmap[bit / 8] &= ~(1 << (bit % 8));
    }
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

void virt_intercept_msr(uint32_t msr, bool read, bool write) {
    unsigned int bits[2];
    if (!msr_bits(msr, &bits[0], &bits[1])) {
        return;
    }
    bool chosen[2] = { read, write };
    for (size_t i = 0; i < 2; i++) {
        if (!chosen[i]) {
            continue;
        }
        bitmap_set(msr_claimed, bits[i], true);
        bitmap_set(msr_bitmap, bits[i], true);
        if (msr_bitmap_x2apic != NULL && (msr < 0x800 || msr > 0x8ff)) {
            bitmap_set(msr_bitmap_x2apic, bits[i], true);
        }
        if (msr_bitmap_x2apic_icr != NULL && (msr < 0x800 || msr > 0x8ff)) {
            bitmap_set(msr_bitmap_x2apic_icr, bits[i], true);
        }
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
