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
#include <virt/exit.h>
#include <csm/csm_boot.h>
#include <hw/power.h>
#include <lib/log.h>
#include <sys/fixup.h>
#include <sys/x86.h>
#include <virt/cpuid.h>
#include <virt/emulate.h>
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
    struct cpuid_regs r = cpuid_guest(v, (uint32_t)v->gprs.rax, (uint32_t)v->gprs.rcx);
    v->gprs.rax = r.eax;
    v->gprs.rbx = r.ebx;
    v->gprs.rcx = r.ecx;
    v->gprs.rdx = r.edx;
    vcpu_skip_instruction(v);
}

// A hypercall instruction that makes no hypercall does what it does on a PC: a
// VMMCALL that nothing intercepts raises #UD (APM 24593 rev. 3.46, 15.18), and
// so does a VMCALL outside VMX operation (SDM 325462-093 Vol. 3C, 33.3).
static void not_hypercall(struct vcpu *v) {
    if (!virt_vmx || !nvmx_vmcall(v)) {
        vcpu_inject_exception(v, 6, false, 0);
    }
}

// Returns whether the guest is to go on after the hypercall instruction; the
// others set where it goes on themselves.
static bool hypercall(struct vcpu *v) {
    uint32_t eax = (uint32_t)v->gprs.rax;
    uint32_t ebx = (uint32_t)v->gprs.rbx;

    if ((eax & ~HYPERCALL_FUNCTION_MASK) != HYPERCALL_MAGIC) {
        not_hypercall(v);
        return false;
    }

    profile_hypercall(eax & HYPERCALL_FUNCTION_MASK);
    switch (eax & HYPERCALL_FUNCTION_MASK) {
        case HYPERCALL_DEBUG_PUTC: {
            printf("%c", (char)ebx);
            return true;
        }
        case HYPERCALL_CSM_RETURN: {
            csm_boot_returned(v);
            return false;
        }
        case HYPERCALL_RESET: {
            if (ebx == HYPERCALL_RESET_SHUTDOWN) {
                power_off();
            }
            if (ebx == HYPERCALL_RESET_COLD || ebx == HYPERCALL_RESET_WARM) {
                power_reset(ebx == HYPERCALL_RESET_WARM);
            }
            break;
        }
        case HYPERCALL_CALL32_INIT: {
            vcpu_save_mode(v, &v->call32_flat);
            v->call32_ready = true;
            return true;
        }
        case HYPERCALL_CALL32_ENTER: {
            if (!v->call32_ready) {
                break;
            }
            vcpu_save_mode(v, &v->call32_saved);
            vcpu_load_mode(v, &v->call32_flat);
            vcpu_set_rip(v, ebx);
            return false;
        }
        case HYPERCALL_CALL32_RETURN: {
            if (!v->call32_ready) {
                break;
            }
            vcpu_load_mode(v, &v->call32_saved);
            vcpu_set_rip(v, ebx);
            return false;
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
            break;
        }
    }
    not_hypercall(v);
    return false;
}

void exit_hypercall(struct vcpu *v) {
    if (hypercall(v)) {
        vcpu_skip_instruction(v);
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
