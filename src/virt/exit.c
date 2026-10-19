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

// EFER bits a guest may set: SCE, LME, LMA (ignored), NXE, LMSLE, FFXSR, and
// TCE. SVME is kept for the host.
#define EFER_GUEST_MASK 0xed01ull

#define CPUID_7_EBX_INVPCID (1u << 10)
#define CPUID_7_EBX_PT (1u << 25)
#define CPUID_7_ECX_WAITPKG (1u << 5)
#define CPUID_7_ECX_RDPID (1u << 22)
#define CPUID_7_EDX_PCONFIG (1u << 18)
#define CPUID_7_1_EAX_MSRLIST (1u << 27)
#define CPUID_7_1_EBX_PBNDKB (1u << 1)
#define CPUID_D_1_EAX_XSAVES (1u << 3)
#define CPUID_D_1_ECX_PT_STATE (1u << 8)
#define CPUID_D_PT_STATE 8
#define CPUID_PT 0x14
#define CPUID_80000001_EDX_NX (1u << 20)
#define CPUID_80000001_EDX_RDTSCP (1u << 27)

// The instructions VMX makes #UD unless a VM-execution control lets them run
// (SDM 325462-093 Vol. 3C, 28.3) show only where the guest's VMCS sets it, and
// Intel PT only where its output can go through EPT and its tracing stop at
// exits, with its state in XSAVES's components too (msr.c).
static void vmx_hide_disabled(uint32_t leaf, uint32_t subleaf, struct cpuid_regs *r) {
    bool rdtscp = vmx_secondary_allowed(PROC2_RDTSCP);
    bool pt = vmx_pt_passthrough();
    if (!pt && (leaf == CPUID_PT || (leaf == 0xd && subleaf == CPUID_D_PT_STATE))) {
        *r = (struct cpuid_regs){0};
        return;
    }
    if (leaf == 7 && subleaf == 0) {
        if (!pt) {
            r->ebx &= ~CPUID_7_EBX_PT;
        }
        if (!vmx_secondary_allowed(PROC2_INVPCID)) {
            r->ebx &= ~CPUID_7_EBX_INVPCID;
        }
        if (!vmx_secondary_allowed(PROC2_USER_WAIT_PAUSE)) {
            r->ecx &= ~CPUID_7_ECX_WAITPKG;
        }
        if (!rdtscp) {
            r->ecx &= ~CPUID_7_ECX_RDPID;
        }
        r->edx &= ~CPUID_7_EDX_PCONFIG;
    } else if (leaf == 7 && subleaf == 1) {
        r->eax &= ~CPUID_7_1_EAX_MSRLIST;
        r->ebx &= ~CPUID_7_1_EBX_PBNDKB;
    } else if (leaf == 0xd && subleaf == 1) {
        if (!vmx_secondary_allowed(PROC2_XSAVES)) {
            r->eax &= ~CPUID_D_1_EAX_XSAVES;
        }
        if (!pt) {
            r->ecx &= ~CPUID_D_1_ECX_PT_STATE;
        }
    } else if (leaf == 0x80000001 && !rdtscp) {
        r->edx &= ~CPUID_80000001_EDX_RDTSCP;
    }
}

void exit_cpuid(struct vcpu *v) {
    uint32_t leaf = (uint32_t)v->gprs.rax;
    uint32_t subleaf = (uint32_t)v->gprs.rcx;
    struct cpuid_regs r = cpuid(leaf, subleaf);

    // Under IA32_MISC_ENABLE's limit only the legacy leaves 0-2 remain, and
    // leaves out of range answer as the last basic one (SDM 325462-093 Vol. 1,
    // 21.1.3).
    bool limited = (v->misc_enable & MISC_ENABLE_LIMIT_CPUID) != 0;
    if (limited && (leaf < 0x80000000 ? leaf > 2 : leaf > cpuid(0x80000000, 0).eax)) {
        leaf = 2;
        r = cpuid(leaf, 0);
    }

    switch (leaf) {
        case 0: {
            if (limited) {
                r.eax = 2;
            }
            break;
        }
        case 1: {
            // No hypervisor shows, and VMX only where the guest gets it.
            r.ecx &= ~(1u << 31);
            if (!nvmx_enabled) {
                r.ecx &= ~(1u << 5);
            }
            // These reflect the guest's CR4 and local APIC, not the host's.
            r.ecx &= ~(1u << 27);
            if (vcpu_cr4(v) & CR4_OSXSAVE) {
                r.ecx |= 1u << 27;
            }
            r.edx &= ~(1u << 9);
            if (vlapic_enabled(v)) {
                r.edx |= 1u << 9;
            }
            break;
        }
        case 7: {
            if (subleaf == 0) {
                r.ecx &= ~(1u << 4);
                if (vcpu_cr4(v) & CR4_PKE) {
                    r.ecx |= 1u << 4;
                }
            }
            break;
        }
        case 0x80000001: {
            r.ecx &= ~(1u << 2);
            // AMD's copy of the APIC flag.
            if ((r.edx & (1u << 9)) && !vlapic_enabled(v)) {
                r.edx &= ~(1u << 9);
            }
            if (v->misc_enable & MISC_ENABLE_XD_DISABLE) {
                r.edx &= ~CPUID_80000001_EDX_NX;
            }
            break;
        }
        default: {
            // Hypervisors report themselves in leaves 40000000h-4FFFFFFFh,
            // which the processor itself answers as any leaf past its last.
            if (leaf >= 0x40000000 && leaf <= 0x4fffffff) {
                r = cpuid(cpuid(0, 0).eax + 1, subleaf);
            }
            break;
        }
    }

    if (virt_vmx) {
        vmx_hide_disabled(leaf, subleaf, &r);
    }

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
    return rdmsr_safe(msr, value);
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
        if (value & ~EFER_GUEST_MASK) {
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
        return wrmsr_safe(msr, value);
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

    if (!xsetbv_safe(index, value)) {
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
    struct insn insn;
    if (!insn_decode(v, &insn)) {
        if (nvmx_exit_pending(v)) {
            return;
        }
        panic("Cannot decode the instruction at %#llx that reached trapped port %#x\n",
              (unsigned long long)vcpu_rip(v), port);
    }
    // Another processor may have changed it since: it then runs again.
    if (insn.opcode >= 0x6c && insn.opcode <= 0x6f) {
        emulate(v, &insn, &string_mem);
    }
}
