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
#include <hw/power.h>
#include <lib/heap.h>
#include <lib/log.h>
#include <sys/cpu.h>
#include <sys/mm.h>
#include <sys/smp.h>
#include <sys/x86.h>
#include <virt/gpt.h>
#include <virt/insn.h>
#include <virt/svm.h>
#include <virt/vcpu.h>
#include <virt/virt.h>
#include <virt/vmx.h>

#define MSR_IA32_CR_PAT 0x277

bool virt_vmx;

static uint8_t *io_bitmap;
static uint8_t *msr_bitmap;

bool vmx_ept_huge_pages(void);
bool vmx_wait_for_sipi_supported(void);
void vmx_get_seg(enum seg_reg reg, struct segment *seg);
void vmx_set_seg(enum seg_reg reg, const struct segment *seg);
uint64_t vmx_cr0_mask(void);
uint64_t vmx_cr4_mask(void);
uint64_t vmx_cr0_real(uint64_t guest_cr0);
uint64_t vmx_cr4_real(uint64_t guest_cr4);

bool virt_init(void) {
    struct cpuid_regs vendor = cpuid(0, 0);
    bool intel = vendor.ebx == 0x756e6547 && vendor.edx == 0x49656e69 && vendor.ecx == 0x6c65746e;

    if (intel && vmx_supported()) {
        virt_vmx = true;
        // I/O bitmaps A and B, then the MSR bitmap.
        io_bitmap = page_alloc(2);
        msr_bitmap = page_alloc(1);
        gpt_init(true, vmx_ept_huge_pages());
    } else if (!intel && svm_supported()) {
        virt_vmx = false;
        io_bitmap = page_alloc(3);
        msr_bitmap = page_alloc(2);
        gpt_init(false, svm_huge_pages());
        // Only nested paging gives the guest its own view of these.
        virt_intercept_msr(MSR_EFER, true, true);
        virt_intercept_msr(MSR_IA32_CR_PAT, true, true);
        virt_intercept_msr(MSR_VM_CR, true, true);
        virt_intercept_msr(MSR_VM_HSAVE_PA, true, true);
    } else {
        return false;
    }

    printf("Using %s\n", virt_vmx ? "VMX" : "SVM");
    return true;
}

bool virt_sipi_in_guest(void) {
    return virt_vmx && vmx_wait_for_sipi_supported();
}

void virt_cpu_init(void) {
    // The host needs none of the protections the firmware may have turned
    // on, and control-flow enforcement would need shadow stacks it lacks.
    uintptr_t cr4 = read_cr4();
    cr4 &= ~(uintptr_t)(CR4_CET | CR4_SMEP | CR4_SMAP | CR4_PKE | CR4_UMIP | CR4_PCIDE);
    // The host executes XSETBV for the guest.
    if (cpuid(1, 0).ecx & (1u << 26)) {
        cr4 |= CR4_OSXSAVE;
    }
    write_cr4(cr4);
    write_cr0(read_cr0() & ~(uintptr_t)(CR0_CD | CR0_NW));
    wrmsr(MSR_IA32_PAT, HOST_PAT);

    if (virt_vmx) {
        vmx_cpu_init();
    } else {
        svm_cpu_init();
    }
}

uint64_t virt_io_bitmap(void) {
    return (uint64_t)(uintptr_t)io_bitmap;
}

uint64_t virt_msr_bitmap(void) {
    return (uint64_t)(uintptr_t)msr_bitmap;
}

void virt_intercept_io(uint16_t port, unsigned count, bool intercept) {
    for (unsigned i = 0; i < count; i++) {
        unsigned p = port + i;
        if (intercept) {
            io_bitmap[p / 8] |= 1 << (p % 8);
        } else {
            io_bitmap[p / 8] &= ~(1 << (p % 8));
        }
    }
}

static void bitmap_set(uint8_t *bitmap, unsigned bit, bool set) {
    if (set) {
        bitmap[bit / 8] |= 1 << (bit % 8);
    } else {
        bitmap[bit / 8] &= ~(1 << (bit % 8));
    }
}

void virt_intercept_msr(uint32_t msr, bool read, bool write) {
    if (virt_vmx) {
        // Read and write bitmaps for the low MSRs, then the high ones; others
        // always exit.
        unsigned base;
        if (msr <= 0x1fff) {
            base = 0;
        } else if (msr >= 0xc0000000 && msr <= 0xc0001fff) {
            base = 0x400 * 8;
        } else {
            return;
        }
        unsigned bit = msr & 0x1fff;
        bitmap_set(msr_bitmap, base + bit, read);
        bitmap_set(msr_bitmap, base + 0x800 * 8 + bit, write);
        return;
    }

    // Two bits per MSR, in three ranges; others always exit.
    unsigned offset;
    if (msr <= 0x1fff) {
        offset = 0;
    } else if (msr >= 0xc0000000 && msr <= 0xc0001fff) {
        offset = 0x800;
    } else if (msr >= 0xc0010000 && msr <= 0xc0011fff) {
        offset = 0x1000;
    } else {
        return;
    }
    unsigned bit = offset * 8 + (msr & 0x1fff) * 2;
    bitmap_set(msr_bitmap, bit, read);
    bitmap_set(msr_bitmap, bit + 1, write);
}

struct vcpu *vcpu_create(struct cpu *cpu) {
    struct vcpu *v = kmalloc(sizeof(*v));
    v->cpu = cpu;
    v->sipi = VCPU_SIPI_RUNNING;
    cpu->vcpu = v;
    if (virt_vmx) {
        vmx_vcpu_init(v);
    } else {
        svm_vcpu_init(v);
    }
    return v;
}

void vcpu_run(struct vcpu *v) {
    if (virt_vmx) {
        vmx_run(v);
    }
    svm_run(v);
}

uint64_t vcpu_rip(struct vcpu *v) {
    return virt_vmx ? vmx_read(VMCS_GUEST_RIP) : v->vmcb->save.rip;
}

void vcpu_set_rip(struct vcpu *v, uint64_t rip) {
    if (virt_vmx) {
        vmx_write(VMCS_GUEST_RIP, rip);
    } else {
        v->vmcb->save.rip = rip;
    }
}

uint64_t vcpu_rsp(struct vcpu *v) {
    return virt_vmx ? vmx_read(VMCS_GUEST_RSP) : v->vmcb->save.rsp;
}

void vcpu_set_rsp(struct vcpu *v, uint64_t rsp) {
    if (virt_vmx) {
        vmx_write(VMCS_GUEST_RSP, rsp);
    } else {
        v->vmcb->save.rsp = rsp;
    }
}

uint64_t vcpu_rflags(struct vcpu *v) {
    return virt_vmx ? vmx_read(VMCS_GUEST_RFLAGS) : v->vmcb->save.rflags;
}

void vcpu_set_rflags(struct vcpu *v, uint64_t rflags) {
    if (virt_vmx) {
        vmx_write(VMCS_GUEST_RFLAGS, rflags);
    } else {
        v->vmcb->save.rflags = rflags;
    }
}

uint64_t vcpu_cr0(struct vcpu *v) {
    if (virt_vmx) {
        uint64_t mask = vmx_cr0_mask();
        return (vmx_read(VMCS_GUEST_CR0) & ~mask) | (v->cr0_shadow & mask);
    }
    return v->vmcb->save.cr0;
}

uint64_t vcpu_cr3(struct vcpu *v) {
    return virt_vmx ? vmx_read(VMCS_GUEST_CR3) : v->vmcb->save.cr3;
}

uint64_t vcpu_cr4(struct vcpu *v) {
    if (virt_vmx) {
        uint64_t mask = vmx_cr4_mask();
        return (vmx_read(VMCS_GUEST_CR4) & ~mask) | (v->cr4_shadow & mask);
    }
    return v->vmcb->save.cr4;
}

uint64_t vcpu_efer(struct vcpu *v) {
    if (virt_vmx) {
        return vmx_read(VMCS_GUEST_EFER);
    }
    return v->vmcb->save.efer & ~(uint64_t)EFER_SVME;
}

static struct vmcb_seg *svm_seg(struct vcpu *v, enum seg_reg reg) {
    struct vmcb_save *s = &v->vmcb->save;
    switch (reg) {
        case SEG_ES: {
            return &s->es;
        }
        case SEG_CS: {
            return &s->cs;
        }
        case SEG_SS: {
            return &s->ss;
        }
        case SEG_DS: {
            return &s->ds;
        }
        case SEG_FS: {
            return &s->fs;
        }
        case SEG_GS: {
            return &s->gs;
        }
        case SEG_LDTR: {
            return &s->ldtr;
        }
        case SEG_TR: {
            return &s->tr;
        }
        case SEG_GDTR: {
            return &s->gdtr;
        }
        default: {
            return &s->idtr;
        }
    }
}

void vcpu_get_seg(struct vcpu *v, enum seg_reg reg, struct segment *seg) {
    if (virt_vmx) {
        vmx_get_seg(reg, seg);
        return;
    }
    struct vmcb_seg *s = svm_seg(v, reg);
    seg->selector = s->selector;
    seg->attrib = s->attrib;
    seg->limit = s->limit;
    seg->base = s->base;
}

void vcpu_set_seg(struct vcpu *v, enum seg_reg reg, const struct segment *seg) {
    if (virt_vmx) {
        vmx_set_seg(reg, seg);
        return;
    }
    struct vmcb_seg *s = svm_seg(v, reg);
    s->selector = seg->selector;
    s->attrib = (seg->attrib & SEG_UNUSABLE) ? 0 : seg->attrib;
    s->limit = seg->limit;
    s->base = seg->base;
    if (reg == SEG_SS) {
        v->vmcb->save.cpl = (seg->attrib >> 5) & 3;
    }
}

void vcpu_save_mode(struct vcpu *v, struct vcpu_mode *mode) {
    mode->cr0 = vcpu_cr0(v);
    mode->cr3 = vcpu_cr3(v);
    mode->cr4 = vcpu_cr4(v);
    mode->efer = vcpu_efer(v);
    mode->rflags = vcpu_rflags(v);
    for (int reg = SEG_ES; reg <= SEG_IDTR; reg++) {
        vcpu_get_seg(v, reg, &mode->segs[reg]);
    }
}

void vcpu_load_mode(struct vcpu *v, const struct vcpu_mode *mode) {
    for (int reg = SEG_ES; reg <= SEG_IDTR; reg++) {
        vcpu_set_seg(v, reg, &mode->segs[reg]);
    }
    vcpu_set_rflags(v, mode->rflags);

    if (virt_vmx) {
        v->cr0_shadow = mode->cr0;
        v->cr4_shadow = mode->cr4;
        vmx_write(VMCS_CR0_SHADOW, mode->cr0);
        vmx_write(VMCS_CR4_SHADOW, mode->cr4);
        vmx_write(VMCS_GUEST_CR0, vmx_cr0_real(mode->cr0));
        vmx_write(VMCS_GUEST_CR4, vmx_cr4_real(mode->cr4));
        vmx_write(VMCS_GUEST_CR3, mode->cr3);
        vmx_write(VMCS_GUEST_EFER, mode->efer);
        uint64_t entry = vmx_read(VMCS_ENTRY_CONTROLS) & ~(uint64_t)ENTRY_IA32E_MODE_GUEST;
        if (mode->efer & EFER_LMA) {
            entry |= ENTRY_IA32E_MODE_GUEST;
        }
        vmx_write(VMCS_ENTRY_CONTROLS, entry);
        vmx_write(VMCS_GUEST_INTERRUPTIBILITY, 0);
        return;
    }

    struct vmcb_save *s = &v->vmcb->save;
    s->cr0 = mode->cr0;
    s->cr3 = mode->cr3;
    s->cr4 = mode->cr4;
    s->efer = mode->efer | EFER_SVME;
    v->vmcb->control.interrupt_shadow = 0;
}

void vcpu_advance(struct vcpu *v, unsigned length) {
    if (virt_vmx) {
        vmx_write(VMCS_GUEST_RIP, vmx_read(VMCS_GUEST_RIP) + length);
        uint64_t intr = vmx_read(VMCS_GUEST_INTERRUPTIBILITY);
        if (intr & (INTERRUPTIBILITY_STI | INTERRUPTIBILITY_MOV_SS)) {
            vmx_write(VMCS_GUEST_INTERRUPTIBILITY,
                      intr & ~(uint64_t)(INTERRUPTIBILITY_STI | INTERRUPTIBILITY_MOV_SS));
        }
        return;
    }
    v->vmcb->save.rip += length;
    v->vmcb->control.interrupt_shadow = 0;
}

void vcpu_skip_instruction(struct vcpu *v) {
    if (virt_vmx) {
        vcpu_advance(v, (unsigned)vmx_read(VMCS_EXIT_INSTR_LEN));
        return;
    }
    if (svm_nrips) {
        vcpu_advance(v, (unsigned)(v->vmcb->control.nrip - v->vmcb->save.rip));
        return;
    }
    unsigned len = insn_length_intercepted(v);
    if (len == 0) {
        panic("Cannot decode the intercepted instruction at %#llx\n",
              (unsigned long long)v->vmcb->save.rip);
    }
    vcpu_advance(v, len);
}

void vcpu_inject_exception(struct vcpu *v, uint8_t vector, bool has_error, uint32_t error) {
    // Real mode exceptions push no error code.
    if (!(vcpu_cr0(v) & CR0_PE)) {
        has_error = false;
    }

    if (virt_vmx) {
        uint32_t info = INTR_INFO_VALID | INTR_TYPE_HW_EXCEPTION | vector;
        if (has_error) {
            info |= INTR_INFO_ERROR_CODE;
            vmx_write(VMCS_ENTRY_EXCEPTION_ERROR, error);
        }
        vmx_write(VMCS_ENTRY_INTR_INFO, info);
        return;
    }

    uint64_t event = SVM_EVENT_VALID | SVM_EVENT_TYPE_EXCEPTION | vector;
    if (has_error) {
        event |= SVM_EVENT_ERROR_VALID | (uint64_t)error << 32;
    }
    v->vmcb->control.eventinj = event;
}

void vcpu_wait_for_sipi(struct vcpu *v) {
    vcpu_set_real_mode(v, 0, 0);
    if (virt_sipi_in_guest()) {
        vmx_write(VMCS_GUEST_ACTIVITY_STATE, ACTIVITY_WAIT_SIPI);
        return;
    }
    // Otherwise the processor waits here for the hypervisor to pass the
    // guest's start-up IPI on.
    __atomic_store_n(&v->sipi, VCPU_SIPI_WAITING, __ATOMIC_RELEASE);
    int32_t vector;
    while ((vector = __atomic_load_n(&v->sipi, __ATOMIC_ACQUIRE)) < 0) {
        cpu_relax();
    }
    __atomic_store_n(&v->sipi, VCPU_SIPI_RUNNING, __ATOMIC_RELAXED);
    vcpu_startup(v, (uint8_t)vector);
}

void vcpu_init_signal(struct vcpu *v) {
    if (v->cpu->bsp) {
        // Its INIT takes the boot processor back to the BIOS.
        printf("INIT for the boot processor, resetting\n");
        power_reset(true);
    }
    lapic_init_reset();
    vcpu_wait_for_sipi(v);
}

void vcpu_startup(struct vcpu *v, uint8_t vector) {
    if (!smp_guest_cpu(v->cpu->apic_id)) {
        vcpu_wait_for_sipi(v);
        return;
    }
    vcpu_set_real_mode(v, (uint16_t)vector << 8, 0);
}

void vcpu_set_real_mode(struct vcpu *v, uint16_t cs, uint16_t ip) {
    struct segment code = {cs, 0x9b, 0xffff, (uint64_t)cs << 4};
    struct segment data = {0, 0x93, 0xffff, 0};
    struct segment ldtr = {0, 0x82, 0xffff, 0};
    struct segment tr = {0, 0x8b, 0xffff, 0};
    struct segment table = {0, 0, 0xffff, 0};

    vcpu_set_seg(v, SEG_CS, &code);
    vcpu_set_seg(v, SEG_DS, &data);
    vcpu_set_seg(v, SEG_ES, &data);
    vcpu_set_seg(v, SEG_FS, &data);
    vcpu_set_seg(v, SEG_GS, &data);
    vcpu_set_seg(v, SEG_SS, &data);
    vcpu_set_seg(v, SEG_LDTR, &ldtr);
    vcpu_set_seg(v, SEG_TR, &tr);
    vcpu_set_seg(v, SEG_GDTR, &table);
    vcpu_set_seg(v, SEG_IDTR, &table);

    vcpu_set_rip(v, ip);
    vcpu_set_rsp(v, 0);
    vcpu_set_rflags(v, RFLAGS_FIXED);
    v->gprs = (struct gprs){0};

    v->cr0_shadow = CR0_ET;
    v->cr4_shadow = 0;
    if (virt_vmx) {
        vmx_write(VMCS_CR0_SHADOW, v->cr0_shadow);
        vmx_write(VMCS_CR4_SHADOW, v->cr4_shadow);
        vmx_write(VMCS_GUEST_CR0, vmx_cr0_real(v->cr0_shadow));
        vmx_write(VMCS_GUEST_CR4, vmx_cr4_real(v->cr4_shadow));
        vmx_write(VMCS_GUEST_CR3, 0);
        vmx_write(VMCS_GUEST_EFER, 0);
        vmx_write(VMCS_ENTRY_CONTROLS,
                  vmx_read(VMCS_ENTRY_CONTROLS) & ~(uint64_t)ENTRY_IA32E_MODE_GUEST);
        vmx_write(VMCS_GUEST_INTERRUPTIBILITY, 0);
        vmx_write(VMCS_GUEST_ACTIVITY_STATE, ACTIVITY_ACTIVE);
        return;
    }

    struct vmcb_save *s = &v->vmcb->save;
    s->cr0 = v->cr0_shadow;
    s->cr3 = 0;
    s->cr4 = 0;
    s->efer = EFER_SVME;
    s->cpl = 0;
    v->vmcb->control.interrupt_shadow = 0;
}
