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
#include <memory.h>
#include <csm/csm_boot.h>
#include <hw/power.h>
#include <lib/log.h>
#include <sys/fixup.h>
#include <sys/x86.h>
#include <virt/exit.h>
#include <virt/guest_mem.h>
#include <virt/hypercall.h>
#include <virt/io.h>
#include <virt/svm.h>
#include <virt/vcpu.h>

#define MSR_IA32_CR_PAT 0x277

// EFER bits a guest may set: SCE, LME, LMA (ignored), NXE, LMSLE, FFXSR, and
// TCE. SVME is kept for the host.
#define EFER_GUEST_MASK 0xed01ull

void exit_cpuid(struct vcpu *v) {
    uint32_t leaf = (uint32_t)v->gprs.rax;
    uint32_t subleaf = (uint32_t)v->gprs.rcx;
    struct cpuid_regs r = cpuid(leaf, subleaf);

    switch (leaf) {
        case 1: {
            // Hardware virtualisation is not offered to the guest yet, and no
            // hypervisor shows.
            r.ecx &= ~(1u << 5 | 1u << 31);
            // These reflect the guest's CR4, not the host's.
            r.ecx &= ~(1u << 27);
            if (vcpu_cr4(v) & CR4_OSXSAVE) {
                r.ecx |= 1u << 27;
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

void exit_rdmsr(struct vcpu *v) {
    uint32_t msr = (uint32_t)v->gprs.rcx;
    uint64_t value;

    if (!virt_vmx && msr == MSR_EFER) {
        value = vcpu_efer(v);
    } else if (!virt_vmx && msr == MSR_IA32_CR_PAT) {
        value = v->vmcb->save.g_pat;
    } else if (!virt_vmx && msr == MSR_VM_HSAVE_PA) {
        value = 0;
    } else if (!rdmsr_safe(msr, &value)) {
        vcpu_inject_exception(v, 13, true, 0);
        return;
    }

    v->gprs.rax = (uint32_t)value;
    v->gprs.rdx = value >> 32;
    vcpu_skip_instruction(v);
}

void exit_wrmsr(struct vcpu *v) {
    uint32_t msr = (uint32_t)v->gprs.rcx;
    uint64_t value = ((uint64_t)(uint32_t)v->gprs.rdx << 32) | (uint32_t)v->gprs.rax;

    if (!virt_vmx && msr == MSR_EFER) {
        if (value & ~EFER_GUEST_MASK) {
            vcpu_inject_exception(v, 13, true, 0);
            return;
        }
        uint64_t old = v->vmcb->save.efer;
        v->vmcb->save.efer = (value & ~(uint64_t)EFER_LMA) | (old & EFER_LMA) | EFER_SVME;
    } else if (!virt_vmx && msr == MSR_IA32_CR_PAT) {
        // Each entry must name one of the types 0, 1, 4, 5, 6, or 7.
        for (int i = 0; i < 8; i++) {
            uint8_t type = value >> (i * 8);
            if (type > 7 || type == 2 || type == 3) {
                vcpu_inject_exception(v, 13, true, 0);
                return;
            }
        }
        v->vmcb->save.g_pat = value;
    } else if (!virt_vmx && (msr == MSR_VM_HSAVE_PA || msr == MSR_VM_CR)) {
        // The guest has no SVM of its own to configure yet.
    } else if (!wrmsr_safe(msr, value)) {
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

void exit_io(struct vcpu *v, uint16_t port, unsigned size, bool in, uint32_t *value) {
    if (!io_dispatch(v, port, size, in, value)) {
        printf("Unexpected trapped I/O to port %#x\n", port);
        if (in) {
            *value = 0xffffffff;
        }
    }
}
