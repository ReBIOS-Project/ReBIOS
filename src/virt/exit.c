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
#include <memory.h>
#include <csm/csm_boot.h>
#include <hw/power.h>
#include <lib/log.h>
#include <sys/fixup.h>
#include <sys/x86.h>
#include <virt/cpuid.h>
#include <virt/emulate.h>
#include <virt/exit.h>
#include <virt/guest_mem.h>
#include <virt/hypercall.h>
#include <virt/insn.h>
#include <virt/io.h>
#include <virt/mmio.h>
#include <virt/msr.h>
#include <virt/mtrr.h>
#include <virt/nvmx.h>
#include <virt/profile.h>
#include <virt/svm.h>
#include <virt/vcpu.h>
#include <virt/vlapic.h>
#include <virt/vmx.h>

#define MSR_IA32_CR_PAT 0x277

void exit_cpuid(struct vcpu *v) {
    struct cpuid_regs r = cpuid_guest(v, (uint32_t)v->gprs.rax, (uint32_t)v->gprs.rcx);
    v->gprs.rax = r.eax;
    v->gprs.rbx = r.ebx;
    v->gprs.rcx = r.ecx;
    v->gprs.rdx = r.edx;
    vcpu_skip_instruction(v);
}

// Returns whether the guest is to go on after the hypercall instruction; the
// others set where it goes on themselves.
static bool hypercall(struct vcpu *v) {
    uint32_t eax = (uint32_t)v->gprs.rax;
    uint32_t ebx = (uint32_t)v->gprs.rbx;

    if ((eax & ~HYPERCALL_FUNCTION_MASK) != HYPERCALL_MAGIC) {
        vcpu_inject_exception(v, 6, false, 0);
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
        default: {
            break;
        }
    }
    vcpu_inject_exception(v, 6, false, 0);
    return false;
}

void exit_hypercall(struct vcpu *v) {
    if (hypercall(v)) {
        vcpu_skip_instruction(v);
    }
}

void exit_invalid_opcode(struct vcpu *v) {
    static const uint8_t vmcall[] = {0x0f, 0x01, 0xc1};
    struct segment cs;
    uint8_t insn[sizeof(vmcall)];

    vcpu_get_seg(v, SEG_CS, &cs);
    uint64_t rip = vcpu_rip(v);
    if (guest_linear_read(v, cs.base + rip, insn, sizeof(insn), true)
        && memcmp(insn, vmcall, sizeof(vmcall)) == 0) {
        if (hypercall(v)) {
            vcpu_set_rip(v, rip + sizeof(vmcall));
        }
        return;
    }
    vcpu_inject_exception(v, 6, false, 0);
}

bool exit_msr_read(struct vcpu *v, uint32_t msr, uint64_t *value) {
    bool fault;
    if (vlapic_rdmsr(v, msr, value, &fault) || mtrr_rdmsr(v, msr, value, &fault)
        || msr_rdmsr(v, msr, value, &fault) || nvmx_rdmsr(v, msr, value, &fault)) {
        return !fault;
    }
    if (!virt_vmx && msr == MSR_EFER) {
        *value = vcpu_efer(v);
        return true;
    }
    if (!virt_vmx && msr == MSR_IA32_CR_PAT) {
        *value = v->vmcb->save.g_pat;
        return true;
    }
    if (!virt_vmx && msr == MSR_VM_HSAVE_PA) {
        *value = 0;
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
        || msr_wrmsr(v, msr, value, &fault) || nvmx_wrmsr(v, msr, value, &fault)) {
        return !fault;
    }
    if (!virt_vmx && msr == MSR_EFER) {
        if (value & ~cpuid_efer_allowed(v)) {
            return false;
        }
        uint64_t old = v->vmcb->save.efer;
        v->vmcb->save.efer = (value & ~(uint64_t)EFER_LMA) | (old & EFER_LMA) | EFER_SVME;
        // The processor would have flushed them for the guest's own write.
        svm_flush_guest_tlb(v);
    } else if (!virt_vmx && msr == MSR_IA32_CR_PAT) {
        // Each entry must name one of the types 0, 1, 4, 5, 6, or 7.
        for (int i = 0; i < 8; i++) {
            uint8_t type = value >> (i * 8);
            if (type > 7 || type == 2 || type == 3) {
                return false;
            }
        }
        v->vmcb->save.g_pat = value;
    } else if (!virt_vmx && (msr == MSR_VM_HSAVE_PA || msr == MSR_VM_CR)) {
        // The guest has no SVM of its own to configure yet.
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
    if ((index == 0 && (value & ~cpuid_xcr0_allowed())) || !xsetbv_safe(index, value)) {
        vcpu_inject_exception(v, 13, true, 0);
        return;
    }
    vcpu_skip_instruction(v);
}

// The guest's guest's accesses all exit, those to ports nobody traps too.
static void port_native(uint16_t port, unsigned size, bool in, uint32_t *value) {
    switch (size) {
        case 1: {
            if (in) {
                *value = inb(port);
            } else {
                outb(port, (uint8_t)*value);
            }
            break;
        }
        case 2: {
            if (in) {
                *value = inw(port);
            } else {
                outw(port, (uint16_t)*value);
            }
            break;
        }
        default: {
            if (in) {
                *value = inl(port);
            } else {
                outl(port, *value);
            }
            break;
        }
    }
}

static void port_access(struct vcpu *v, uint16_t port, unsigned size, bool in, uint32_t *value) {
    if (io_dispatch(v, port, size, in, value)) {
        return;
    }
    if (nvmx_in_l2(v)) {
        port_native(port, size, in, value);
        return;
    }
    printf("Unexpected trapped I/O to port %#x\n", port);
    if (in) {
        *value = 0xffffffff;
    }
}

void exit_io(struct vcpu *v, uint16_t port, unsigned size, bool in, uint32_t *value) {
    profile_io(port);
    port_access(v, port, size, in, value);
}

static bool string_ins(struct vcpu *v, uint64_t linear, uint16_t port, unsigned size,
                       uint32_t *value) {
    (void)linear;
    port_access(v, port, size, true, value);
    return true;
}

static void string_outs(struct vcpu *v, uint16_t port, unsigned size, uint32_t value) {
    port_access(v, port, size, false, &value);
}

static const struct emulate_mem string_mem = {mmio_read_linear, mmio_write_linear, string_ins,
                                              string_outs};

void exit_string_io(struct vcpu *v, uint16_t port) {
    profile_io(port);
    // One the hypervisor cannot read, as where another processor just changed
    // its mapping, runs again.
    struct insn insn;
    if (!insn_decode(v, &insn)) {
        if (!nvmx_exit_pending(v)) {
            printf("Cannot decode the instruction at %#llx that reached trapped port %#x\n",
                   (unsigned long long)vcpu_rip(v), port);
        }
        return;
    }
    // Another processor may have changed it since: it then runs again.
    if (insn.opcode >= 0x6c && insn.opcode <= 0x6f) {
        emulate(v, &insn, &string_mem);
    }
}
