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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <virt/exit.h>
#include <csm/csm_boot.h>
#include <hw/power.h>
#include <lib/log.h>
#include <lib/spinlock.h>
#include <lib/util.h>
#include <sys/fixup.h>
#include <sys/x86.h>
#include <virt/cpuid.h>
#include <virt/emulate.h>
#include <virt/guest_mem.h>
#include <virt/hypercall.h>
#include <virt/insn.h>
#include <virt/io.h>
#include <virt/mmio.h>
#include <virt/msr.h>
#include <virt/mtrr.h>
#include <virt/nsvm.h>
#include <virt/nvmx.h>
#include <virt/profile.h>
#include <virt/svm.h>
#include <virt/usb_disk.h>
#include <virt/vacpi.h>
#include <virt/vcpu.h>
#include <virt/virt.h>
#include <virt/vlapic.h>
#include <virt/vmx.h>

void exit_cpuid(struct vcpu *v) {
    // CPUID faulting (msr.c), which the guest has no SMM for.
    if (v->cpuid_faulting && vcpu_cpl(v) != 0) {
        vcpu_inject_exception(v, 13, true, 0);
        return;
    }
    struct cpuid_regs r = cpuid_guest(v, (uint32_t)v->gprs.rax, (uint32_t)v->gprs.rcx);
    v->gprs.rax = r.eax;
    v->gprs.rbx = r.ebx;
    v->gprs.rcx = r.ecx;
    v->gprs.rdx = r.edx;
    vcpu_skip_instruction(v);
}

// VMCALL and VMMCALL do what they do on a PC: a VMMCALL that nothing
// intercepts raises #UD (APM 24593 rev. 3.46, 15.18), and so does a VMCALL
// outside VMX operation (SDM 325462-093 Vol. 3C, 33.3).
void exit_vmcall(struct vcpu *v) {
    if (!virt_vmx || !nvmx_vmcall(v)) {
        vcpu_inject_exception(v, 6, false, 0);
    }
}

// Whether the instruction at CS:RIP, of length bytes, its first byte and its
// last, is where within says, as the guest's paging maps it.
static bool code_in(struct vcpu *v, size_t length, bool (*within)(uintptr_t gpa)) {
    const size_t ends[] = { 0, length - 1 };
    struct vcpu_segment cs;
    vcpu_get_seg(v, VCPU_SEG_CS, &cs);
    bool long_mode = (vcpu_efer(v) & EFER_LMA) != 0 && (cs.attrib & VCPU_SEG_L) != 0;
    for (size_t i = 0; i < ARRAY_COUNT(ends); i++) {
        uintptr_t linear = cs.base + vcpu_rip(v) + ends[i];
        if (!long_mode) {
            linear &= 0xffffffff;
        }
        uint64_t gpa;
        uint32_t pf_error;
        if (!guest_mem_linear_to_phys(v, linear, false, true, &gpa, &pf_error) || !within(gpa)) {
            return false;
        }
    }
    return true;
}

// Hypercalls are the legacy BIOS's, in whatever mode the processor let its
// write to the port through, as V86 mode, where memory managers and Windows
// run it for DOS, and never a nested guest's. Until the boot, it and the
// option ROMs it runs are all that runs, at CPL 0. From the boot on, they come
// from its image's read-only part. The thunk's returns from its CSM16 calls
// come before the boot, or where the boot fails.
static bool allowed(struct vcpu *v, size_t length, uint32_t function) {
    if (nvmx_in_l2(v) || nsvm_in_l2(v)) {
        return false;
    }
    bool ring0 = vcpu_cpl(v) == 0 && (vcpu_rflags(v) & RFLAGS_VM) == 0;
    if (!csm_boot_begun()) {
        return ring0;
    }
    if (function == HYPERCALL_CSM_RETURN) {
        return ring0 && code_in(v, length, csm_boot_thunk_code);
    }
    return code_in(v, length, csm_boot_bios_code);
}

// The length of the instruction whose I/O exit this is: the VM-exit
// instruction length (SDM 325462-093 Vol. 3C, 28.2.5), or up to the next RIP
// that an IOIO intercept gives (APM 24593 rev. 3.46, 15.10.2).
static size_t io_length(struct vcpu *v) {
    if (virt_vmx) {
        return (size_t)vmx_read(VMCS_EXIT_INSTR_LEN);
    }
    return (size_t)(v->vmcb->control.exitinfo2 - v->vmcb->save.rip);
}

// The legacy BIOS's 32-bit flat mode, for HYPERCALL_CALL32_*, which it gives
// once for every vCPU: what the mode holds stays in its image, which lasts
// across S3.
static struct spinlock call32_lock;
static bool call32_ready;
static struct vcpu_mode call32_flat;

// Carries out the hypercall that the write of AL at CS:RIP makes, if it makes
// one, and returns whether it did; one that sets where the guest goes on
// itself sets v->io_retry, so that the guest goes on there.
static bool hypercall(struct vcpu *v) {
    uint32_t eax = (uint32_t)v->gprs.rax;
    uint32_t ebx = (uint32_t)v->gprs.rbx;
    uint32_t function = eax & HYPERCALL_FUNCTION_MASK;

    if ((eax & ~HYPERCALL_FUNCTION_MASK) != HYPERCALL_MAGIC
     || !allowed(v, io_length(v), function)) {
        return false;
    }

    profile_hypercall(function);
    switch (function) {
        case HYPERCALL_DEBUG_PUTC: {
            printf("%c", (char)ebx);
            return true;
        }
        case HYPERCALL_CSM_RETURN: {
            csm_boot_returned(v);
            v->io_retry = true;
            return true;
        }
        case HYPERCALL_RESET: {
            if (ebx == HYPERCALL_RESET_SHUTDOWN) {
                power_off();
            }
            if (ebx == HYPERCALL_RESET_COLD || ebx == HYPERCALL_RESET_WARM) {
                power_reset(ebx == HYPERCALL_RESET_WARM);
            }
            return false;
        }
        case HYPERCALL_CALL32_INIT: {
            spinlock_acquire(&call32_lock);
            vcpu_save_mode(v, &call32_flat);
            call32_ready = true;
            spinlock_release(&call32_lock);
            return true;
        }
        case HYPERCALL_CALL32_ENTER: {
            spinlock_acquire(&call32_lock);
            bool ready = call32_ready;
            struct vcpu_mode flat = call32_flat;
            spinlock_release(&call32_lock);
            if (!ready) {
                return false;
            }
            vcpu_save_mode(v, &v->call32_saved);
            v->call32_entered = true;
            vcpu_load_mode(v, &flat);
            vcpu_set_rip(v, ebx);
            v->io_retry = true;
            return true;
        }
        case HYPERCALL_CALL32_RETURN: {
            if (!v->call32_entered) {
                return false;
            }
            vcpu_load_mode(v, &v->call32_saved);
            vcpu_set_rip(v, ebx);
            v->io_retry = true;
            return true;
        }
        case HYPERCALL_USB_DISK: {
            usb_disk_call(ebx);
            return true;
        }
        case HYPERCALL_PNP_NODES: {
            vacpi_pnp_nodes(ebx);
            return true;
        }
        default: {
            return false;
        }
    }
}

bool exit_msr_read(struct vcpu *v, uint32_t msr, uint64_t *value) {
    bool fault;
    if (vlapic_rdmsr(v, msr, value, &fault) || mtrr_rdmsr(v, msr, value, &fault)
     || msr_rdmsr(v, msr, value, &fault) || nvmx_rdmsr(v, msr, value, &fault)
     || nsvm_rdmsr(v, msr, value, &fault)) {
        return !fault;
    }
    if (!virt_vmx && msr == MSR_EFER) {
        *value = vcpu_efer(v);
        return true;
    }
    if (!virt_vmx && msr == MSR_IA32_PAT) {
        *value = v->vmcb->save.g_pat;
        return true;
    }
    if (!virt_vmx && msr == MSR_VM_HSAVE_PA) {
        *value = 0;
        return true;
    }
    // SVM, where the guest does not get it, as a BIOS that disabled it leaves
    // it (APM 24593 rev. 3.45, 15.30.1).
    if (!virt_vmx && msr == MSR_VM_CR) {
        *value = VM_CR_LOCK | VM_CR_SVMDIS;
        return true;
    }
    return msr_allowed(msr) && rdmsr_safe(msr, value);
}

void exit_rdmsr(struct vcpu *v) {
    uint64_t value;
    if (!exit_msr_read(v, (uint32_t)v->gprs.rcx, &value)) {
        vcpu_inject_exception(v, 13, true, 0);
        return;
    }
    v->gprs.rax = (uint32_t)value;
    v->gprs.rdx = value >> 32;
    vcpu_skip_instruction(v);
}

bool exit_msr_write(struct vcpu *v, uint32_t msr, uint64_t value) {
    bool fault;
    if (vlapic_wrmsr(v, msr, value, &fault) || mtrr_wrmsr(v, msr, value, &fault)
     || msr_wrmsr(v, msr, value, &fault) || nvmx_wrmsr(v, msr, value, &fault)
     || nsvm_wrmsr(v, msr, value, &fault)) {
        return !fault;
    }
    if (!virt_vmx && msr == MSR_EFER) {
        if ((value & ~cpuid_efer_allowed(v)) != 0) {
            return false;
        }
        uint64_t old = v->vmcb->save.efer;
        // LME changes only while paging is off (APM 24593 rev. 3.46, 14.6.2),
        // as Linux 7.3's KVM checks (set_efer()).
        if ((vcpu_cr0(v) & CR0_PG) != 0 && ((old ^ value) & EFER_LME) != 0) {
            return false;
        }
        v->vmcb->save.efer = (value & ~(uint64_t)EFER_LMA) | (old & EFER_LMA) | EFER_SVME;
        nsvm_set_svme(v, (value & EFER_SVME) != 0);
        // The processor would have flushed them for the guest's own write.
        svm_flush_guest_tlb(v);
    } else if (!virt_vmx && msr == MSR_IA32_PAT) {
        if (!x86_pat_valid(value)) {
            return false;
        }
        nsvm_set_pat(v, value);
    } else if (!virt_vmx && (msr == MSR_VM_HSAVE_PA || msr == MSR_VM_CR)) {
        // SVM, where the guest does not get it, which it cannot configure.
    } else {
        return msr_allowed(msr) && wrmsr_safe(msr, value);
    }
    return true;
}

void exit_wrmsr(struct vcpu *v) {
    uint64_t value = ((uint64_t)(uint32_t)v->gprs.rdx << 32) | (uint32_t)v->gprs.rax;
    if (!exit_msr_write(v, (uint32_t)v->gprs.rcx, value)) {
        vcpu_inject_exception(v, 13, true, 0);
        return;
    }
    vcpu_skip_instruction(v);
}

void exit_msr_register(struct vcpu *v, uint32_t msr, unsigned int reg, bool write) {
    uint64_t value = 0;
    bool done = write ? exit_msr_write(v, msr, vcpu_gpr(v, reg)) : exit_msr_read(v, msr, &value);
    if (!done) {
        vcpu_inject_exception(v, 13, true, 0);
        return;
    }
    if (!write) {
        vcpu_set_gpr(v, reg, value);
    }
    vcpu_skip_instruction(v);
}

void exit_xsetbv(struct vcpu *v) {
    uint32_t index = (uint32_t)v->gprs.rcx;
    uint64_t value = ((uint64_t)(uint32_t)v->gprs.rdx << 32) | (uint32_t)v->gprs.rax;

    // Only the state components the guest gets.
    if ((index == 0 && (value & ~cpuid_xcr0_allowed()) != 0) || !xsetbv_safe(index, value)) {
        vcpu_inject_exception(v, 13, true, 0);
        return;
    }
    vcpu_skip_instruction(v);
}

static void port_access(struct vcpu *v, uint16_t port, size_t size, bool in, uint32_t *value) {
    vcpu_io_access(v, port, size);
    io_dispatch(v, port, size, in, value);
}

void exit_io(struct vcpu *v, uint16_t port, size_t size, bool in, uint32_t *value) {
    profile_io(port);
    if (port == HYPERCALL_PORT && size == 1 && !in) {
        vcpu_io_access(v, port, size);
        if (!hypercall(v)) {
            io_dispatch(v, port, size, in, value);
        }
        return;
    }
    port_access(v, port, size, in, value);
}

static bool string_ins(
    struct vcpu *v,
    uintptr_t linear,
    uint16_t port,
    size_t size,
    uint32_t *value
) {
    (void)linear;
    port_access(v, port, size, true, value);
    return true;
}

static void string_outs(struct vcpu *v, uint16_t port, size_t size, uint32_t value) {
    port_access(v, port, size, false, &value);
}

static const struct emulate_mem string_mem = {
    mmio_read_linear, mmio_write_linear, string_ins, string_outs, mmio_string
};

void exit_string_io(struct vcpu *v, uint16_t port) {
    profile_io(port);
    // One the hypervisor cannot read, as where another processor just changed
    // its mapping, runs again.
    struct insn insn;
    if (!insn_decode(v, &insn)) {
        if (!nvmx_exit_pending(v) && !nsvm_exit_pending(v)) {
            printf(
                "Cannot decode the instruction at %#llx that reached trapped port %#x\n",
                (unsigned long long)vcpu_rip(v), port
            );
        }
        return;
    }
    // Another processor may have changed it since: it then runs again.
    if (insn.opcode >= 0x6c && insn.opcode <= 0x6f) {
        emulate(v, &insn, &string_mem);
    }
    v->io_retry = false;
}
