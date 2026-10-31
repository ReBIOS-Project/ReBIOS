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
#include <hw/acpi.h>
#include <hw/lapic.h>
#include <hw/power.h>
#include <lib/heap.h>
#include <lib/log.h>
#include <lib/printf.h>
#include <lib/spinlock.h>
#include <lib/time.h>
#include <memory.h>
#include <sys/cpu.h>
#include <sys/mm.h>
#include <sys/smp.h>
#include <sys/x86.h>
#include <virt/event.h>
#include <virt/gpt.h>
#include <virt/hv_timer.h>
#include <virt/insn.h>
#include <virt/msr.h>
#include <virt/mtrr.h>
#include <virt/nvmx.h>
#include <virt/profile.h>
#include <virt/svm.h>
#include <virt/usb.h>
#include <virt/vcpu.h>
#include <virt/vga.h>
#include <virt/vlapic.h>
#include <virt/vmx.h>

#define MSR_IA32_CR_PAT 0x277
#define MSR_DEBUGCTL 0x1d9
#define MSR_DR1_ADDR_MASK 0xc0011019
#define MSR_DR0_ADDR_MASK 0xc0011027

#define DEBUGCTL_LBR ((uint32_t)1 << 0)
#define DEBUGCTL_BTF ((uint32_t)1 << 1)
#define DR6_BS ((uint64_t)1 << 14)
#define DR7_GD ((uint32_t)1 << 13)

#define RESET_HOLD_SECONDS 30

// Exits that keep a CPU from the guest for longer than this get reported.
#define WATCHDOG_NS ((uint32_t)5000000)
#define WATCHDOG_REPORT_NS ((uint64_t)1000000000)

bool virt_vmx;

static uint8_t *io_bitmap;
static uint8_t *msr_bitmap;
// The accesses to MSRs a module has chosen for, as their bits in msr_bitmap.
static uint8_t *msr_claimed;
// For vCPUs whose x2APIC the processor virtualises: the same but for the
// accesses to x2APIC registers it passes, and for those whose IPIs it
// virtualises too, the ICR's as well.
static uint8_t *msr_bitmap_x2apic;
static uint8_t *msr_bitmap_x2apic_icr;
// How many of DR0-DR3 have AMD's address masks for data breakpoints (APM 24593
// rev. 3.45, 13.1.7): DR0 with DataBkptExt, all four with AddrMaskExt.
static unsigned int breakpoint_masks;

static size_t describe_guest(char *buf, size_t size) {
    struct vcpu *v = this_cpu()->vcpu;
    if (v == NULL) {
        return 0;
    }
    int len = snprintf(
        buf, size, "Its guest last exited with %s %#x, at RIP %#llx\n",
        virt_vmx ? "basic reason" : "code", v->last_exit,
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

bool virt_init(void) {
    struct cpuid_regs vendor = cpuid(0, 0);
    bool intel = vendor.ebx == 0x756e6547 && vendor.edx == 0x49656e69
        && vendor.ecx == 0x6c65746e;

    if (intel && vmx_supported()) {
        virt_vmx = true;
        // I/O bitmaps A and B, then the MSR bitmap.
        io_bitmap = page_alloc(2);
        msr_bitmap = page_alloc(1);
        msr_claimed = page_alloc(1);
        // Only the MSRs the guest gets go to the processor (msr.c).
        memset(msr_bitmap, 0xff, PAGE_SIZE);
        gpt_init(true, vmx_ept_huge_pages(), vmx_ept_dirty_flags());
    } else if (!intel && svm_supported()) {
        virt_vmx = false;
        io_bitmap = page_alloc(3);
        msr_bitmap = page_alloc(2);
        msr_claimed = page_alloc(2);
        memset(msr_bitmap, 0xff, 2 * PAGE_SIZE);
        // Nested paging's tables keep dirty flags as page tables do (APM 24593
        // rev. 3.45, 15.25.5).
        gpt_init(false, svm_huge_pages(), true);
        // Only nested paging gives the guest its own view of these.
        virt_intercept_msr(MSR_EFER, true, true);
        virt_intercept_msr(MSR_IA32_CR_PAT, true, true);
        virt_intercept_msr(MSR_VM_CR, true, true);
        virt_intercept_msr(MSR_VM_HSAVE_PA, true, true);
        uint32_t features = cpuid(0x80000001, 0).ecx;
        breakpoint_masks = (features & ((uint32_t)1 << 30)) != 0 ? 4
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
    // The host needs none of the protections the firmware may have turned on,
    // and control-flow enforcement would need shadow stacks it lacks.
    uintptr_t cr4 = read_cr4();
    cr4 &= ~(uintptr_t)(CR4_CET | CR4_SMEP | CR4_SMAP | CR4_PKE | CR4_UMIP
        | CR4_PCIDE);
    // The host executes XSETBV for the guest.
    if ((cpuid(1, 0).ecx & ((uint32_t)1 << 26)) != 0) {
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
    size_t pages = virt_vmx ? 1 : 2;
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
    size_t pages = virt_vmx ? 1 : 2;
    msr_bitmap_x2apic_icr = page_alloc(pages);
    memcpy(msr_bitmap_x2apic_icr, msr_bitmap_x2apic, pages * PAGE_SIZE);
    unsigned int read_bit, write_bit;
    if (msr_bits(msr, &read_bit, &write_bit)) {
        bitmap_set(msr_bitmap_x2apic_icr, read_bit, false);
        bitmap_set(msr_bitmap_x2apic_icr, write_bit, false);
    }
}

struct vcpu *vcpu_create(struct cpu *cpu) {
    struct vcpu *v = kmalloc(sizeof(struct vcpu));
    v->cpu = cpu;
    v->sipi = VCPU_SIPI_RUNNING;
    cpu->vcpu = v;
    if (virt_vmx) {
        vmx_vcpu_init(v);
    } else {
        svm_vcpu_init(v);
    }
    vlapic_create(v);
    mtrr_vcpu_init(v);
    msr_vcpu_init(v);
    nvmx_vcpu_init(v);
    return v;
}

static struct spinlock world_lock;
static struct cpu *world_owner;
// Odd while the others are paused.
static uint64_t world_epoch;
static uint32_t world_acks;

static void service_pause(struct vcpu *v) {
    for (;;) {
        uint64_t epoch = __atomic_load_n(&world_epoch, __ATOMIC_ACQUIRE);
        if ((epoch & 1) == 0
         || __atomic_load_n(&world_owner, __ATOMIC_ACQUIRE) == v->cpu) {
            return;
        }
        __atomic_add_fetch(&world_acks, 1, __ATOMIC_ACQ_REL);
        while (__atomic_load_n(&world_epoch, __ATOMIC_ACQUIRE) == epoch) {
            cpu_relax();
        }
    }
}

void vcpu_pause_others(void) {
    struct cpu *self = this_cpu();
    // Another pause may come first, which this one must not hold up.
    while (!spinlock_try(&world_lock)) {
        service_pause(self->vcpu);
        cpu_relax();
    }
    __atomic_store_n(&world_acks, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&world_owner, self, __ATOMIC_RELAXED);
    __atomic_add_fetch(&world_epoch, 1, __ATOMIC_SEQ_CST);
    // Those waiting for a start-up IPI stay out of the guest anyway.
    uint32_t expected = 0;
    for (size_t i = 0; i < smp_cpu_count(); i++) {
        struct cpu *cpu = smp_cpu(i);
        struct vcpu *v = cpu->vcpu;
        if (cpu != self && __atomic_load_n(&cpu->online, __ATOMIC_ACQUIRE)
         && v != NULL
         && __atomic_load_n(&v->sipi, __ATOMIC_ACQUIRE) != VCPU_SIPI_WAITING) {
            expected++;
            vcpu_kick(cpu);
        }
    }
    while (__atomic_load_n(&world_acks, __ATOMIC_ACQUIRE) < expected) {
        cpu_relax();
    }
}

void vcpu_resume_others(void) {
    __atomic_store_n(&world_owner, NULL, __ATOMIC_RELAXED);
    __atomic_add_fetch(&world_epoch, 1, __ATOMIC_SEQ_CST);
    spinlock_release(&world_lock);
}

bool vcpu_can_single_step(void) {
    return !virt_vmx || vmx_monitor_trap_supported();
}

// The guest's IA32_DEBUGCTL: VMX switches it, as SVM does with LBR
// virtualisation; without, the processor's is the guest's.
static uint64_t guest_debugctl(struct vcpu *v) {
    if (virt_vmx) {
        return vmx_read(VMCS_GUEST_DEBUGCTL);
    }
    return svm_lbr_virtualisation ? v->vmcb->save.dbgctl : rdmsr(MSR_DEBUGCTL);
}

static void set_guest_debugctl(struct vcpu *v, uint64_t value) {
    if (virt_vmx) {
        vmx_write(VMCS_GUEST_DEBUGCTL, value);
    } else if (svm_lbr_virtualisation) {
        v->vmcb->save.dbgctl = value;
    } else {
        wrmsr(MSR_DEBUGCTL, value);
    }
}

// The faults a stepped instruction may raise, which SVM intercepts so that the
// step ends there.
#define STEP_EXCEPTIONS \
    ((uint32_t)1 << 0 | (uint32_t)1 << 1 | (uint32_t)1 << 7 \
        | (uint32_t)1 << 12 | (uint32_t)1 << 13 | (uint32_t)1 << 14 \
        | (uint32_t)1 << 16 | (uint32_t)1 << 17 | (uint32_t)1 << 19)

void vcpu_single_step(struct vcpu *v, bool on) {
    if (virt_vmx) {
        uint64_t controls = vmx_read(VMCS_PROC_CONTROLS);
        vmx_write(
            VMCS_PROC_CONTROLS,
            on ? controls | PROC_MONITOR_TRAP_FLAG
                : controls & ~(uint64_t)PROC_MONITOR_TRAP_FLAG
        );
        return;
    }
    // SVM has no monitor trap flag: a single-step #DB instead.
    struct vmcb *vmcb = v->vmcb;
    if (on) {
        v->step_tf = (vmcb->save.rflags & RFLAGS_TF) != 0;
        v->step_dr6 = vmcb->save.dr6;
        v->step_exceptions = vmcb->control.intercept_exceptions;
        // The guest's BTF would let the instruction run on to the next branch
        // (APM 24593 rev. 3.45, 13.1.6.2).
        v->step_debugctl = guest_debugctl(v);
        if ((v->step_debugctl & DEBUGCTL_BTF) != 0) {
            set_guest_debugctl(v, v->step_debugctl & ~(uint64_t)DEBUGCTL_BTF);
        }
        vmcb->save.rflags |= RFLAGS_TF;
        vmcb->control.intercept_exceptions |= STEP_EXCEPTIONS;
        return;
    }
    if (!v->step_tf) {
        vmcb->save.rflags &= ~(uint64_t)RFLAGS_TF;
    }
    if ((v->step_debugctl & DEBUGCTL_BTF) != 0) {
        set_guest_debugctl(v, v->step_debugctl);
    }
    vmcb->save.dr6 = v->step_dr6;
    vmcb->control.intercept_exceptions = v->step_exceptions;
}

// Reports, at most once a second for each CPU, an exit that kept the CPU from
// the guest for longer than the hypervisor means to, as that delays its
// interrupts.
static void watchdog(struct vcpu *v) {
    struct cpu *cpu = v->cpu;
    uint64_t now = time_ns();
    uint64_t held = now - cpu->slice_start;
    if (cpu->slice_start == 0 || held < WATCHDOG_NS
     || now < cpu->watchdog_next) {
        return;
    }
    cpu->watchdog_next = now + WATCHDOG_REPORT_NS;
    printf(
        "Exit %#x kept CPU %u from the guest for %llu.%03llu ms\n",
        v->last_exit, cpu->index, (unsigned long long)(held / 1000000),
        (unsigned long long)(held / 1000 % 1000)
    );
}

void vcpu_before_entry(struct vcpu *v) {
    // The guest would draw over a panic's message.
    if (log_panicked()) {
        halt_forever();
    }
    // Breakpoints only an instruction that completes reports.
    v->db_pending = 0;
    profile_entry(v);
    service_pause(v);
    acpi_slice();
    // Before the flush below, which the display's remapping may need.
    vga_refresh();
    nvmx_wait(v);
    // VMX operation blocks INIT, which its guest's exits take.
    if (!nvmx_holds_init(v)
     && __atomic_exchange_n(&v->cpu->init_pending, false, __ATOMIC_ACQUIRE)) {
        vcpu_init_signal(v);
    }
    uint64_t generation = __atomic_load_n(&gpt_generation, __ATOMIC_ACQUIRE);
    if (v->gpt_generation != generation) {
        v->gpt_generation = generation;
        if (virt_vmx) {
            nvmx_gpt_changed(v);
            vmx_load_gpt_root(v);
            vmx_invept_all();
        } else {
            v->vmcb->control.n_cr3 = gpt_root();
            svm_flush_guest_tlb(v);
        }
    }
    watchdog(v);
    if (v->cpu->bsp) {
        usb_slice(false);
        hv_timer_run();
    }
    vlapic_timer_program(v);
    cpu_slice_end();
    vlapic_prepare_entry(v);
    if (nvmx_before_entry(v)) {
        return;
    }
    // A stepped instruction is to run, not an event's handler.
    if (v->stepping) {
        event_hold(v);
    } else {
        event_entry(v);
    }
    vlapic_before_entry(v);
}

void vcpu_kick(struct cpu *cpu) {
    if (cpu == this_cpu()
     || !__atomic_load_n(&cpu->online, __ATOMIC_ACQUIRE)) {
        return;
    }
    // One waiting for a start-up IPI catches up before it enters the guest.
    struct vcpu *v = cpu->vcpu;
    if (v == NULL
     || __atomic_load_n(&v->sipi, __ATOMIC_ACQUIRE) == VCPU_SIPI_WAITING) {
        return;
    }
    vcpu_wake(cpu);
}

// Interrupts always make the guest exit, and one pending at an entry makes it
// exit at once, as one ends a halt.
void vcpu_wake(struct cpu *cpu) {
    if (cpu == this_cpu()
     || !__atomic_load_n(&cpu->online, __ATOMIC_ACQUIRE)) {
        return;
    }
    if (!__atomic_exchange_n(&cpu->kick, true, __ATOMIC_ACQ_REL)) {
        lapic_send_ipi(cpu->apic_id, ICR_DELIVERY_FIXED | VLAPIC_KICK_VECTOR);
    }
}

void vcpu_exit_soon(struct vcpu *v) {
    struct cpu *cpu = v->cpu;
    if (!__atomic_exchange_n(&cpu->kick, true, __ATOMIC_ACQ_REL)) {
        lapic_send_ipi(cpu->apic_id, ICR_DELIVERY_FIXED | VLAPIC_KICK_VECTOR);
    }
}

void vcpu_wait(struct vcpu *v, bool (*ends)(struct vcpu *v), bool spin) {
    struct cpu *cpu = v->cpu;
    // SVM's host takes interrupts and NMIs only with GIF set.
    if (!virt_vmx) {
        asm volatile ("stgi" :: : "memory");
    }
    cpu->taking_interrupts = true;
    vlapic_halt(v, true);
    for (;;) {
        // Each look at whether the wait ends starts a slice, so that the work
        // done meanwhile holds that up no longer than an exit holds the guest.
        cpu_slice_start();
        __atomic_store_n(&cpu->kicked, false, __ATOMIC_SEQ_CST);
        service_pause(v);
        if (cpu->bsp) {
            usb_slice(true);
            hv_timer_run();
        }
        vlapic_timer_program(v);
        if (ends(v)) {
            break;
        }
        if (log_panicked()) {
            halt_forever();
        }
        acpi_slice();
        vga_refresh();
        // Work under way goes on while the guest waits, the boot processor
        // keeping the AML and USB tasks going.
        if (spin || vga_redrawing()
         || (cpu->bsp && (acpi_working() || usb_working()))) {
            asm volatile ("sti\n\tnop\n\tcli" :: : "memory");
            cpu_relax();
        } else {
            cpu_halt();
        }
    }
    vlapic_halt(v, false);
    cpu->taking_interrupts = false;
    if (!virt_vmx) {
        asm volatile ("clgi" :: : "memory");
    }
}

void vcpu_halt(struct vcpu *v) {
    vcpu_skip_instruction(v);
    vcpu_wait(v, event_halt_ends, false);
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

void vcpu_set_rip(struct vcpu *v, uintptr_t rip) {
    if (virt_vmx) {
        vmx_write(VMCS_GUEST_RIP, rip);
    } else {
        v->vmcb->save.rip = rip;
    }
}

uint64_t vcpu_rsp(struct vcpu *v) {
    return virt_vmx ? vmx_read(VMCS_GUEST_RSP) : v->vmcb->save.rsp;
}

void vcpu_set_rsp(struct vcpu *v, uintptr_t rsp) {
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

// User addresses clear bit 63 (SDM 325462-093 Vol. 3A, 4.4.1). Upper Address
// Ignore takes bits 63:57 of DS and ES accesses for bit 56's.
uint64_t vcpu_untagged(struct vcpu *v, size_t reg, uintptr_t linear) {
    if (!virt_vmx && (reg == SEG_DS || reg == SEG_ES)
     && (vcpu_efer(v) & EFER_UAIE) != 0) {
        return (uint64_t)((int64_t)(linear << 7) >> 7);
    }
    int bit;
    if (linear >> 63 != 0) {
        if ((vcpu_cr4(v) & CR4_LAM_SUP) == 0) {
            return linear;
        }
        bit = (vcpu_cr4(v) & CR4_LA57) != 0 ? 56 : 47;
    } else if ((vcpu_cr3(v) & CR3_LAM_U57) != 0) {
        bit = 56;
    } else if ((vcpu_cr3(v) & CR3_LAM_U48) != 0) {
        bit = 47;
    } else {
        return linear;
    }
    uint64_t metadata = (((uint64_t)1 << 63) - 1)
        & ~(((uint64_t)2 << bit) - 1);
    return (linear >> bit & 1) != 0 ? linear | metadata : linear & ~metadata;
}

uint64_t vcpu_efer(struct vcpu *v) {
    if (virt_vmx) {
        return vmx_read(VMCS_GUEST_EFER);
    }
    return v->vmcb->save.efer & ~(uint64_t)EFER_SVME;
}

static struct vmcb_seg *svm_seg(struct vcpu *v, size_t reg) {
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

void vcpu_get_seg(struct vcpu *v, size_t reg, struct segment *seg) {
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

void vcpu_set_seg(struct vcpu *v, size_t reg, const struct segment *seg) {
    if (virt_vmx) {
        vmx_set_seg(reg, seg);
        return;
    }
    struct vmcb_seg *s = svm_seg(v, reg);
    s->selector = seg->selector;
    s->attrib = (seg->attrib & SEG_UNUSABLE) != 0 ? 0 : seg->attrib;
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
    for (size_t reg = SEG_ES; reg <= SEG_IDTR; reg++) {
        vcpu_get_seg(v, reg, &mode->segs[reg]);
    }
}

void vcpu_load_mode(struct vcpu *v, const struct vcpu_mode *mode) {
    for (size_t reg = SEG_ES; reg <= SEG_IDTR; reg++) {
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
        uint64_t entry = vmx_read(VMCS_ENTRY_CONTROLS)
            & ~(uint64_t)ENTRY_IA32E_MODE_GUEST;
        if ((mode->efer & EFER_LMA) != 0) {
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
    svm_flush_guest_tlb(v);
}

bool vcpu_guest_tf(struct vcpu *v) {
    // SVM's stepping sets TF and clears BTF for itself, keeping the guest's
    // aside.
    if (!virt_vmx && v->stepping) {
        return v->step_tf && (v->step_debugctl & DEBUGCTL_BTF) == 0;
    }
    // With BTF, TF steps only branches (SDM 325462-093 Vol. 3B, 20.4.3; APM
    // 24593 rev. 3.45, 13.1.6.2), and the hypervisor carries out none for the
    // guest.
    return (vcpu_rflags(v) & RFLAGS_TF) != 0
        && (guest_debugctl(v) & DEBUGCTL_BTF) == 0;
}

void vcpu_debug_trap(struct vcpu *v, uint64_t dr6) {
    // A debug exception's exit takes DR6's bits as its qualification, leaving
    // DR6 alone (SDM 325462-093 Vol. 3C, 30.2.1).
    if (nvmx_exception(v, 1, false, 0, dr6)) {
        return;
    }
    // Injection leaves the debug registers and IA32_DEBUGCTL alone (SDM
    // 325462-093 Vol. 3C, 29.6.1.1), where delivering a debug exception sets
    // DR6's bits and clears DR7.GD (Vol. 3B, 20.2.4) and DEBUGCTL's LBR and
    // BTF (20.4.2 and 20.4.3; APM 24593 rev. 3.45, 13.1.6).
    if (virt_vmx) {
        // VMX switches DR7 but not DR6, so the processor's is the guest's.
        uint64_t value;
        asm volatile ("mov %%dr6, %0" : "=r"(value));
        asm volatile ("mov %0, %%dr6" :: "r"(value | dr6));
        vmx_write(
            VMCS_GUEST_DR7, vmx_read(VMCS_GUEST_DR7) & ~(uint64_t)DR7_GD
        );
    } else {
        v->vmcb->save.dr6 |= dr6;
        v->vmcb->save.dr7 &= ~(uint64_t)DR7_GD;
    }
    uint64_t debugctl = guest_debugctl(v);
    if ((debugctl & (DEBUGCTL_LBR | DEBUGCTL_BTF)) != 0) {
        set_guest_debugctl(
            v, debugctl & ~(uint64_t)(DEBUGCTL_LBR | DEBUGCTL_BTF)
        );
    }
    vcpu_inject_exception(v, 1, false, 0);
}

void vcpu_step_trap(struct vcpu *v) {
    vcpu_debug_trap(v, DR6_BS);
}

// Neither VMX nor SVM switches DR0-DR3: the processor's are the guest's.
static uint64_t guest_dr(size_t n) {
    uint64_t value;
    switch (n) {
        case 0: {
            asm volatile ("mov %%dr0, %0" : "=r"(value));
            break;
        }
        case 1: {
            asm volatile ("mov %%dr1, %0" : "=r"(value));
            break;
        }
        case 2: {
            asm volatile ("mov %%dr2, %0" : "=r"(value));
            break;
        }
        default: {
            asm volatile ("mov %%dr3, %0" : "=r"(value));
            break;
        }
    }
    return value;
}

// The enabled breakpoints of the R/W kinds in kinds (as 1 << R/W) that a byte
// of [address, address + size) matches, as DR6's B0-B3 bits: LEN masks the low
// bits of the address (SDM 325462-093 Vol. 3B, 20.2.4 and 20.2.5), and so does
// an AMD address mask the bits it sets.
static uint64_t breakpoints_hit(
    struct vcpu *v, uintptr_t address, size_t size, unsigned int kinds
) {
    uint64_t dr7 = virt_vmx ? vmx_read(VMCS_GUEST_DR7) : v->vmcb->save.dr7;
    if ((dr7 & 0xff) == 0) {
        return 0;
    }
    static const unsigned int lengths[4] = { 1, 2, 8, 4 };
    uint64_t hits = 0;
    for (size_t n = 0; n < 4; n++) {
        unsigned int rw = (dr7 >> (16 + 4 * n)) & 3;
        if (((dr7 >> (2 * n)) & 3) == 0
         || (kinds & ((uint32_t)1 << rw)) == 0) {
            continue;
        }
        uint64_t ignored = lengths[(dr7 >> (18 + 4 * n)) & 3] - 1;
        if (n < breakpoint_masks) {
            ignored |= (uint32_t)rdmsr(
                n == 0 ? MSR_DR0_ADDR_MASK : MSR_DR1_ADDR_MASK + n - 1
            );
        }
        uint64_t dr = guest_dr(n);
        for (size_t i = 0; i < size; i++) {
            if ((((address + i) ^ dr) & ~ignored) == 0) {
                hits |= (uint64_t)1 << n;
                break;
            }
        }
    }
    return hits;
}

void vcpu_data_access(
    struct vcpu *v, uintptr_t linear, size_t size, bool write
) {
    // R/W 01 breaks on writes, 11 on reads and writes.
    v->db_pending |= breakpoints_hit(
        v, linear, size,
        write ? (uint32_t)1 << 1 | (uint32_t)1 << 3 : (uint32_t)1 << 3
    );
}

void vcpu_io_access(struct vcpu *v, uint16_t port, size_t size) {
    // R/W 10 breaks on I/O where CR4.DE says so.
    if ((vcpu_cr4(v) & CR4_DE) != 0) {
        v->db_pending |= breakpoints_hit(v, port, size, (uint32_t)1 << 2);
    }
}

void vcpu_debug_traps(struct vcpu *v, bool single_step) {
    uint64_t dr6 = v->db_pending | (single_step ? DR6_BS : 0);
    v->db_pending = 0;
    if (dr6 != 0) {
        vcpu_debug_trap(v, dr6);
    }
}

void vcpu_step_boundary(struct vcpu *v) {
    uint64_t dr6 = v->db_pending | (vcpu_guest_tf(v) ? DR6_BS : 0);
    if (nvmx_monitor_trap(v, dr6)) {
        v->db_pending = 0;
        return;
    }
    vcpu_debug_traps(v, vcpu_guest_tf(v));
}

bool vcpu_stepped(struct vcpu *v) {
    return vcpu_guest_tf(v) || nvmx_monitor_trap_flag(v);
}

void vcpu_advance(struct vcpu *v, size_t length) {
    uintptr_t rip = vcpu_rip(v);
    uint64_t next = rip + length;
    // Outside 64-bit mode RIP wraps at 4 GiB, as Linux 7.3's KVM has it
    // (vmx.c, skip_emulated_instruction()); a 32-bit guest with a wider one is
    // unpredictable on AMD's Family 19h (erratum 1393, 57095 rev. 1.05).
    if ((next >> 32) != (rip >> 32)) {
        struct segment cs;
        vcpu_get_seg(v, SEG_CS, &cs);
        if ((vcpu_efer(v) & EFER_LMA) == 0 || (cs.attrib & SEG_L) == 0) {
            next = (uint32_t)next;
        }
    }
    // The instruction is done, so RF is clear (SDM 325462-093 Vol. 3B,
    // 20.3.1.1).
    uint64_t rflags = vcpu_rflags(v);
    if ((rflags & RFLAGS_RF) != 0) {
        vcpu_set_rflags(v, rflags & ~(uint64_t)RFLAGS_RF);
    }
    if (virt_vmx) {
        vmx_write(VMCS_GUEST_RIP, next);
        uint64_t intr = vmx_read(VMCS_GUEST_INTERRUPTIBILITY);
        if ((intr & (INTERRUPTIBILITY_STI | INTERRUPTIBILITY_MOV_SS)) != 0) {
            vmx_write(
                VMCS_GUEST_INTERRUPTIBILITY,
                intr
                    & ~(uint64_t)(INTERRUPTIBILITY_STI
                        | INTERRUPTIBILITY_MOV_SS)
            );
        }
    } else {
        v->vmcb->save.rip = next;
        v->vmcb->control.interrupt_shadow = 0;
    }
    vcpu_step_boundary(v);
}

void vcpu_skip_instruction(struct vcpu *v) {
    if (virt_vmx) {
        vcpu_advance(v, (unsigned int)vmx_read(VMCS_EXIT_INSTR_LEN));
        return;
    }
    if (svm_nrips) {
        vcpu_advance(
            v, (unsigned int)(v->vmcb->control.nrip - v->vmcb->save.rip)
        );
        return;
    }
    // One the hypervisor cannot read, as where another processor just changed
    // its mapping, runs again.
    size_t len = insn_length_intercepted(v);
    if (len == 0) {
        printf(
            "Cannot decode the intercepted instruction at %#llx\n",
            (unsigned long long)v->vmcb->save.rip
        );
        return;
    }
    vcpu_advance(v, len);
}

void vcpu_inject_exception(
    struct vcpu *v, uint8_t vector, bool has_error, uint32_t error
) {
    // Breakpoints not yet reported are lost (SDM 325462-093 Vol. 3B,
    // 20.3.1.2).
    v->db_pending = 0;
    // Real mode exceptions push no error code.
    if ((vcpu_cr0(v) & CR0_PE) == 0) {
        has_error = false;
    }
    if (nvmx_exception(v, vector, has_error, error, 0)) {
        return;
    }
    v->injecting = true;

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

void vcpu_inject_page_fault(struct vcpu *v, uintptr_t linear, uint32_t error) {
    // A page fault's exit takes the address as its qualification, leaving CR2
    // alone (SDM 325462-093 Vol. 3C, 30.2.1).
    if (nvmx_exception(v, 14, true, error, linear)) {
        return;
    }
    // VMX leaves CR2 as it is for the guest, and the host takes no page
    // faults.
    if (virt_vmx) {
        write_cr2(linear);
    } else {
        v->vmcb->save.cr2 = linear;
    }
    vcpu_inject_exception(v, 14, true, error);
}

// Halted, as most processors wait out the guest's not starting them: the
// start-up IPI's sender wakes it.
void vcpu_wait_for_sipi(struct vcpu *v) {
    vcpu_set_real_mode(v, 0, 0);
    struct cpu *cpu = v->cpu;
    if (!virt_vmx) {
        asm volatile ("stgi" :: : "memory");
    }
    cpu->taking_interrupts = true;
    __atomic_store_n(&v->sipi, VCPU_SIPI_WAITING, __ATOMIC_RELEASE);
    int32_t vector;
    for (;;) {
        cpu_slice_start();
        __atomic_store_n(&cpu->kicked, false, __ATOMIC_SEQ_CST);
        if ((vector = __atomic_load_n(&v->sipi, __ATOMIC_ACQUIRE)) >= 0) {
            break;
        }
        if (cpu->bsp) {
            usb_slice(true);
            hv_timer_run();
        }
        vlapic_timer_program(v);
        cpu_halt();
    }
    cpu->taking_interrupts = false;
    if (!virt_vmx) {
        asm volatile ("clgi" :: : "memory");
    }
    vcpu_startup(v, (uint8_t)vector);
}

void vcpu_init_signal(struct vcpu *v) {
    if (v->cpu->bsp) {
        // Its INIT takes the boot processor back to the BIOS.
        printf("INIT for the boot processor, resetting\n");
        power_reset(true);
    }
    vlapic_reset(v);
    vcpu_wait_for_sipi(v);
}

void vcpu_startup(struct vcpu *v, uint8_t vector) {
    __atomic_store_n(&v->sipi, VCPU_SIPI_RUNNING, __ATOMIC_RELEASE);
    if (!smp_guest_cpu(v->cpu->apic_id)) {
        vcpu_wait_for_sipi(v);
        return;
    }
    vcpu_set_real_mode(v, (uint16_t)vector << 8, 0);
}

void vcpu_set_real_mode(struct vcpu *v, uint16_t cs, uint16_t ip) {
    struct segment code = { cs, 0x9b, 0xffff, (uint64_t)cs << 4 };
    struct segment data = { 0, 0x93, 0xffff, 0 };
    struct segment ldtr = { 0, 0x82, 0xffff, 0 };
    struct segment tr = { 0, 0x8b, 0xffff, 0 };
    struct segment table = { 0, 0, 0xffff, 0 };

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
    v->gprs = (struct gprs){ 0 };
    v->redeliver = 0;

    v->cr0_shadow = CR0_ET;
    v->cr4_shadow = 0;
    if (virt_vmx) {
        vmx_write(VMCS_CR0_SHADOW, v->cr0_shadow);
        vmx_write(VMCS_CR4_SHADOW, v->cr4_shadow);
        vmx_write(VMCS_GUEST_CR0, vmx_cr0_real(v->cr0_shadow));
        vmx_write(VMCS_GUEST_CR4, vmx_cr4_real(v->cr4_shadow));
        vmx_write(VMCS_GUEST_CR3, 0);
        vmx_write(VMCS_GUEST_EFER, 0);
        vmx_write(
            VMCS_ENTRY_CONTROLS,
            vmx_read(VMCS_ENTRY_CONTROLS) & ~(uint64_t)ENTRY_IA32E_MODE_GUEST
        );
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
    svm_flush_guest_tlb(v);
}
