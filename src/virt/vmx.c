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
#include <hw/power.h>
#include <lib/log.h>
#include <sys/cpu.h>
#include <sys/mm.h>
#include <sys/x86.h>
#include <virt/exit.h>
#include <virt/gpt.h>
#include <virt/guest_mem.h>
#include <virt/vapic.h>
#include <virt/vcpu.h>
#include <virt/virt.h>
#include <virt/vmx.h>

#define EPT_CAP_WALK_4 (1ull << 6)
#define EPT_CAP_WB (1ull << 14)
#define EPT_CAP_2M (1ull << 16)
#define EPT_CAP_1G (1ull << 17)
#define EPT_CAP_INVEPT (1ull << 20)
#define EPT_CAP_INVEPT_ALL (1ull << 26)
#define VPID_CAP_INVVPID (1ull << 32)
#define VPID_CAP_INVVPID_ALL (1ull << 42)

#define EPTP_WB 6
#define EPTP_WALK_4 (3 << 3)

static uint64_t vmx_basic;
static uint64_t pin_cap, proc_cap, proc2_cap, exit_cap, entry_cap;
static uint64_t ept_vpid_cap;
static uint64_t vmx_misc;
static uint64_t cr0_fixed0, cr0_fixed1, cr4_fixed0, cr4_fixed1;

int vmx_enter(struct gprs *gprs, int launched);
void vmx_exit_entry(void);

uint64_t vmx_read(uint32_t field) {
    uint64_t value;
    bool ok;
    asm volatile ("vmread %2, %0\n\t"
                  "seta %1"
                  : "=r"(value), "=qm"(ok)
                  : "r"((uint64_t)field)
                  : "cc");
    if (!ok) {
        panic("VMREAD of field %#x failed\n", field);
    }
    return value;
}

void vmx_write(uint32_t field, uint64_t value) {
    bool ok;
    asm volatile ("vmwrite %1, %2\n\t"
                  "seta %0"
                  : "=qm"(ok)
                  : "r"(value), "r"((uint64_t)field)
                  : "cc");
    if (!ok) {
        uint64_t error;
        asm volatile ("vmread %1, %0" : "=r"(error) : "r"((uint64_t)VMCS_VM_INSTR_ERROR) : "cc");
        panic("VMWRITE of %#llx to field %#x failed, error %llu\n",
              (unsigned long long)value, field, (unsigned long long)error);
    }
}

static bool control_allowed(uint64_t cap, uint32_t bits) {
    return ((cap >> 32) & bits) == bits;
}

// Bits the CPU fixes to 1 are set, those it cannot set are dropped, but those
// required are checked first.
static uint32_t adjust_controls(uint64_t cap, uint32_t want, uint32_t require, const char *name) {
    if (!control_allowed(cap, require)) {
        panic("VMX lacks required %s controls %#x\n", name,
              require & ~(uint32_t)(cap >> 32));
    }
    return ((want | require) & (uint32_t)(cap >> 32)) | (uint32_t)cap;
}

bool vmx_supported(void) {
    if (!(cpuid(1, 0).ecx & (1u << 5))) {
        return false;
    }

    uint64_t feature_control = rdmsr(MSR_IA32_FEATURE_CONTROL);
    if ((feature_control & FEATURE_CONTROL_LOCK)
        && !(feature_control & FEATURE_CONTROL_VMX_OUTSIDE_SMX)) {
        printf("VMX is disabled by the firmware\n");
        return false;
    }

    vmx_basic = rdmsr(MSR_IA32_VMX_BASIC);
    bool true_ctls = (vmx_basic >> 55) & 1;
    pin_cap = rdmsr(true_ctls ? MSR_IA32_VMX_TRUE_PINBASED_CTLS : MSR_IA32_VMX_PINBASED_CTLS);
    proc_cap = rdmsr(true_ctls ? MSR_IA32_VMX_TRUE_PROCBASED_CTLS : MSR_IA32_VMX_PROCBASED_CTLS);
    exit_cap = rdmsr(true_ctls ? MSR_IA32_VMX_TRUE_EXIT_CTLS : MSR_IA32_VMX_EXIT_CTLS);
    entry_cap = rdmsr(true_ctls ? MSR_IA32_VMX_TRUE_ENTRY_CTLS : MSR_IA32_VMX_ENTRY_CTLS);
    cr0_fixed0 = rdmsr(MSR_IA32_VMX_CR0_FIXED0);
    cr0_fixed1 = rdmsr(MSR_IA32_VMX_CR0_FIXED1);
    cr4_fixed0 = rdmsr(MSR_IA32_VMX_CR4_FIXED0);
    cr4_fixed1 = rdmsr(MSR_IA32_VMX_CR4_FIXED1);

    if (!control_allowed(proc_cap, PROC_SECONDARY_CONTROLS)) {
        printf("VMX lacks secondary controls\n");
        return false;
    }
    proc2_cap = rdmsr(MSR_IA32_VMX_PROCBASED_CTLS2);
    if (!control_allowed(proc2_cap, PROC2_EPT | PROC2_UNRESTRICTED_GUEST)) {
        printf("VMX lacks EPT or unrestricted guest support\n");
        return false;
    }

    ept_vpid_cap = rdmsr(MSR_IA32_VMX_EPT_VPID_CAP);
    vmx_misc = rdmsr(MSR_IA32_VMX_MISC);
    uint64_t ept_needed = EPT_CAP_WALK_4 | EPT_CAP_WB | EPT_CAP_2M | EPT_CAP_INVEPT
                          | EPT_CAP_INVEPT_ALL;
    if ((ept_vpid_cap & ept_needed) != ept_needed) {
        printf("VMX lacks required EPT capabilities\n");
        return false;
    }

    return true;
}

bool vmx_wait_for_sipi_supported(void) {
    return (vmx_misc & VMX_MISC_WAIT_SIPI) != 0;
}

bool vmx_ept_huge_pages(void) {
    return (ept_vpid_cap & EPT_CAP_1G) != 0;
}

static bool vpid_usable(void) {
    return control_allowed(proc2_cap, PROC2_VPID)
           && (ept_vpid_cap & (VPID_CAP_INVVPID | VPID_CAP_INVVPID_ALL))
              == (VPID_CAP_INVVPID | VPID_CAP_INVVPID_ALL);
}

void vmx_cpu_init(void) {
    uint64_t feature_control = rdmsr(MSR_IA32_FEATURE_CONTROL);
    if (!(feature_control & FEATURE_CONTROL_LOCK)) {
        wrmsr(MSR_IA32_FEATURE_CONTROL,
              feature_control | FEATURE_CONTROL_LOCK | FEATURE_CONTROL_VMX_OUTSIDE_SMX);
    }

    write_cr0((read_cr0() | cr0_fixed0) & cr0_fixed1);
    write_cr4((read_cr4() | cr4_fixed0 | CR4_VMXE) & cr4_fixed1);

    uint32_t *vmxon_region = page_alloc(1);
    vmxon_region[0] = (uint32_t)vmx_basic & 0x7fffffff;
    uint64_t pa = (uint64_t)(uintptr_t)vmxon_region;
    bool ok;
    asm volatile ("vmxon %1\n\t"
                  "seta %0"
                  : "=qm"(ok)
                  : "m"(pa)
                  : "cc", "memory");
    if (!ok) {
        panic("VMXON failed\n");
    }

    struct {
        uint64_t eptp;
        uint64_t reserved;
    } ept_desc = {0, 0};
    asm volatile ("invept %0, %1" :: "m"(ept_desc), "r"(2ull) : "cc", "memory");
    if (vpid_usable()) {
        struct {
            uint64_t vpid;
            uint64_t addr;
        } vpid_desc = {0, 0};
        asm volatile ("invvpid %0, %1" :: "m"(vpid_desc), "r"(2ull) : "cc", "memory");
    }
}

static uint32_t seg_to_ar(uint32_t attrib) {
    if (attrib & SEG_UNUSABLE) {
        return 0x10000;
    }
    return (attrib & 0xff) | ((attrib & 0xf00) << 4);
}

static uint32_t ar_to_seg(uint32_t ar) {
    if (ar & 0x10000) {
        return SEG_UNUSABLE;
    }
    return (ar & 0xff) | ((ar >> 4) & 0xf00);
}

static const uint32_t seg_sel_fields[] = {
    VMCS_GUEST_ES_SEL, VMCS_GUEST_CS_SEL, VMCS_GUEST_SS_SEL, VMCS_GUEST_DS_SEL,
    VMCS_GUEST_FS_SEL, VMCS_GUEST_GS_SEL, VMCS_GUEST_LDTR_SEL, VMCS_GUEST_TR_SEL,
};

static const uint32_t seg_base_fields[] = {
    VMCS_GUEST_ES_BASE, VMCS_GUEST_CS_BASE, VMCS_GUEST_SS_BASE, VMCS_GUEST_DS_BASE,
    VMCS_GUEST_FS_BASE, VMCS_GUEST_GS_BASE, VMCS_GUEST_LDTR_BASE, VMCS_GUEST_TR_BASE,
    VMCS_GUEST_GDTR_BASE, VMCS_GUEST_IDTR_BASE,
};

static const uint32_t seg_limit_fields[] = {
    VMCS_GUEST_ES_LIMIT, VMCS_GUEST_CS_LIMIT, VMCS_GUEST_SS_LIMIT, VMCS_GUEST_DS_LIMIT,
    VMCS_GUEST_FS_LIMIT, VMCS_GUEST_GS_LIMIT, VMCS_GUEST_LDTR_LIMIT, VMCS_GUEST_TR_LIMIT,
    VMCS_GUEST_GDTR_LIMIT, VMCS_GUEST_IDTR_LIMIT,
};

static const uint32_t seg_ar_fields[] = {
    VMCS_GUEST_ES_AR, VMCS_GUEST_CS_AR, VMCS_GUEST_SS_AR, VMCS_GUEST_DS_AR,
    VMCS_GUEST_FS_AR, VMCS_GUEST_GS_AR, VMCS_GUEST_LDTR_AR, VMCS_GUEST_TR_AR,
};

void vmx_get_seg(enum seg_reg reg, struct segment *seg) {
    seg->base = vmx_read(seg_base_fields[reg]);
    seg->limit = vmx_read(seg_limit_fields[reg]);
    if (reg >= SEG_GDTR) {
        seg->selector = 0;
        seg->attrib = 0;
        return;
    }
    seg->selector = vmx_read(seg_sel_fields[reg]);
    seg->attrib = ar_to_seg(vmx_read(seg_ar_fields[reg]));
}

void vmx_set_seg(enum seg_reg reg, const struct segment *seg) {
    vmx_write(seg_base_fields[reg], seg->base);
    vmx_write(seg_limit_fields[reg], seg->limit);
    if (reg >= SEG_GDTR) {
        return;
    }
    vmx_write(seg_sel_fields[reg], seg->selector);
    vmx_write(seg_ar_fields[reg], seg_to_ar(seg->attrib));
}

uint64_t vmx_cr0_mask(void) {
    // PE and PG are the guest's with unrestricted guest; the other fixed bits,
    // NE above all, are the host's to keep.
    return (cr0_fixed0 | ~cr0_fixed1) & ~(uint64_t)(CR0_PE | CR0_PG);
}

uint64_t vmx_cr4_mask(void) {
    return cr4_fixed0 | ~cr4_fixed1;
}

uint64_t vmx_cr0_real(uint64_t guest_cr0) {
    return (guest_cr0 | (cr0_fixed0 & ~(uint64_t)(CR0_PE | CR0_PG))) & (cr0_fixed1 | CR0_PE | CR0_PG);
}

uint64_t vmx_cr4_real(uint64_t guest_cr4) {
    return (guest_cr4 | cr4_fixed0) & cr4_fixed1;
}

void vmx_vcpu_init(struct vcpu *v) {
    struct cpu *cpu = v->cpu;

    v->vmcs = page_alloc(1);
    v->vmcs_pa = (uint64_t)(uintptr_t)v->vmcs;
    *(uint32_t *)v->vmcs = (uint32_t)vmx_basic & 0x7fffffff;

    bool ok;
    asm volatile ("vmclear %1\n\tseta %0" : "=qm"(ok) : "m"(v->vmcs_pa) : "cc", "memory");
    if (!ok) {
        panic("VMCLEAR failed\n");
    }
    asm volatile ("vmptrld %1\n\tseta %0" : "=qm"(ok) : "m"(v->vmcs_pa) : "cc", "memory");
    if (!ok) {
        panic("VMPTRLD failed\n");
    }

    vmx_write(VMCS_PIN_CONTROLS, adjust_controls(pin_cap, 0, 0, "pin-based"));

    vmx_write(VMCS_PROC_CONTROLS,
              adjust_controls(proc_cap, 0,
                              PROC_IO_BITMAPS | PROC_MSR_BITMAPS | PROC_SECONDARY_CONTROLS,
                              "processor-based"));

    // What the guest's instructions need to work as they would without VMX.
    uint32_t proc2_want = PROC2_RDTSCP | PROC2_INVPCID | PROC2_XSAVES | PROC2_USER_WAIT_PAUSE;
    uint32_t proc2_require = PROC2_EPT | PROC2_UNRESTRICTED_GUEST;
    if (vpid_usable()) {
        proc2_require |= PROC2_VPID;
    }
    vmx_write(VMCS_PROC_CONTROLS2,
              adjust_controls(proc2_cap, proc2_want, proc2_require, "secondary"));

    vmx_write(VMCS_EXIT_CONTROLS,
              adjust_controls(exit_cap, EXIT_SAVE_DEBUG_CONTROLS,
                              EXIT_HOST_ADDR_SPACE_SIZE | EXIT_SAVE_EFER | EXIT_LOAD_EFER
                              | EXIT_SAVE_PAT | EXIT_LOAD_PAT,
                              "VM-exit"));
    vmx_write(VMCS_ENTRY_CONTROLS,
              adjust_controls(entry_cap, ENTRY_LOAD_DEBUG_CONTROLS,
                              ENTRY_LOAD_EFER | ENTRY_LOAD_PAT, "VM-entry"));

    vmx_write(VMCS_EXCEPTION_BITMAP, 0);
    vmx_write(VMCS_PF_ERROR_MASK, 0);
    vmx_write(VMCS_PF_ERROR_MATCH, 0);
    vmx_write(VMCS_CR3_TARGET_COUNT, 0);
    vmx_write(VMCS_EXIT_MSR_STORE_COUNT, 0);
    vmx_write(VMCS_EXIT_MSR_LOAD_COUNT, 0);
    vmx_write(VMCS_ENTRY_MSR_LOAD_COUNT, 0);
    vmx_write(VMCS_ENTRY_INTR_INFO, 0);

    vmx_write(VMCS_IO_BITMAP_A, virt_io_bitmap());
    vmx_write(VMCS_IO_BITMAP_B, virt_io_bitmap() + PAGE_SIZE);
    vmx_write(VMCS_MSR_BITMAP, virt_msr_bitmap());
    vmx_write(VMCS_EPT_POINTER, gpt_root() | EPTP_WALK_4 | EPTP_WB);
    if (vpid_usable()) {
        vmx_write(VMCS_VPID, 1);
    }

    vmx_write(VMCS_CR0_MASK, vmx_cr0_mask());
    vmx_write(VMCS_CR4_MASK, vmx_cr4_mask());

    vmx_write(VMCS_HOST_CR0, read_cr0());
    vmx_write(VMCS_HOST_CR3, read_cr3());
    vmx_write(VMCS_HOST_CR4, read_cr4());
    vmx_write(VMCS_HOST_CS_SEL, GDT_CODE);
    vmx_write(VMCS_HOST_SS_SEL, GDT_DATA);
    vmx_write(VMCS_HOST_DS_SEL, GDT_DATA);
    vmx_write(VMCS_HOST_ES_SEL, GDT_DATA);
    vmx_write(VMCS_HOST_FS_SEL, 0);
    vmx_write(VMCS_HOST_GS_SEL, 0);
    vmx_write(VMCS_HOST_TR_SEL, GDT_TSS);
    vmx_write(VMCS_HOST_FS_BASE, 0);
    vmx_write(VMCS_HOST_GS_BASE, 0);
    vmx_write(VMCS_HOST_TR_BASE, (uint64_t)(uintptr_t)&cpu->tss);
    vmx_write(VMCS_HOST_GDTR_BASE, (uint64_t)(uintptr_t)cpu->gdt);
    vmx_write(VMCS_HOST_IDTR_BASE, cpu_idt_base());
    vmx_write(VMCS_HOST_SYSENTER_CS, 0);
    vmx_write(VMCS_HOST_SYSENTER_ESP, 0);
    vmx_write(VMCS_HOST_SYSENTER_EIP, 0);
    vmx_write(VMCS_HOST_EFER, rdmsr(MSR_EFER));
    vmx_write(VMCS_HOST_PAT, HOST_PAT);
    vmx_write(VMCS_HOST_RIP, (uint64_t)(uintptr_t)vmx_exit_entry);

    vmx_write(VMCS_LINK_POINTER, ~0ull);
    vmx_write(VMCS_GUEST_DEBUGCTL, 0);
    vmx_write(VMCS_GUEST_DR7, 0x400);
    // The PAT's power-on value.
    vmx_write(VMCS_GUEST_PAT, 0x0007040600070406ull);
    vmx_write(VMCS_GUEST_SYSENTER_CS, 0);
    vmx_write(VMCS_GUEST_SYSENTER_ESP, 0);
    vmx_write(VMCS_GUEST_SYSENTER_EIP, 0);
    vmx_write(VMCS_GUEST_INTERRUPTIBILITY, 0);
    vmx_write(VMCS_GUEST_ACTIVITY_STATE, ACTIVITY_ACTIVE);
    vmx_write(VMCS_GUEST_PENDING_DBG, 0);
}

static void vmx_dump_guest(void) {
    printf("Guest RIP %#llx RSP %#llx RFLAGS %#llx\n",
           (unsigned long long)vmx_read(VMCS_GUEST_RIP),
           (unsigned long long)vmx_read(VMCS_GUEST_RSP),
           (unsigned long long)vmx_read(VMCS_GUEST_RFLAGS));
    printf("Guest CR0 %#llx CR3 %#llx CR4 %#llx EFER %#llx entry controls %#llx\n",
           (unsigned long long)vmx_read(VMCS_GUEST_CR0),
           (unsigned long long)vmx_read(VMCS_GUEST_CR3),
           (unsigned long long)vmx_read(VMCS_GUEST_CR4),
           (unsigned long long)vmx_read(VMCS_GUEST_EFER),
           (unsigned long long)vmx_read(VMCS_ENTRY_CONTROLS));
    static const char *names[] = {"ES", "CS", "SS", "DS", "FS", "GS", "LDTR", "TR"};
    for (int i = 0; i < 8; i++) {
        printf("Guest %s sel %#llx base %#llx limit %#llx ar %#llx\n", names[i],
               (unsigned long long)vmx_read(seg_sel_fields[i]),
               (unsigned long long)vmx_read(seg_base_fields[i]),
               (unsigned long long)vmx_read(seg_limit_fields[i]),
               (unsigned long long)vmx_read(seg_ar_fields[i]));
    }
}

// What the processor does itself on a MOV to CR0 or CR4 that it carries out:
// paging with EFER.LME on activates long mode, and PAE paging outside long
// mode loads the PDPTEs CR3 points to. Returns false where the move raises
// #GP instead.
static bool vmx_paging_changed(uint64_t cr0, uint64_t cr4) {
    uint64_t efer = vmx_read(VMCS_GUEST_EFER);
    bool paging = (cr0 & CR0_PG) != 0;
    bool pae = (cr4 & CR4_PAE) != 0;
    if (paging && (efer & EFER_LME) && !pae) {
        return false;
    }
    uint64_t entry = vmx_read(VMCS_ENTRY_CONTROLS) & ~(uint64_t)ENTRY_IA32E_MODE_GUEST;
    if (paging && (efer & EFER_LME)) {
        efer |= EFER_LMA;
        entry |= ENTRY_IA32E_MODE_GUEST;
    } else {
        efer &= ~(uint64_t)EFER_LMA;
    }
    vmx_write(VMCS_GUEST_EFER, efer);
    vmx_write(VMCS_ENTRY_CONTROLS, entry);
    if (paging && pae && !(efer & EFER_LMA)) {
        uint64_t pdptes[4] = {0};
        guest_phys_read(vmx_read(VMCS_GUEST_CR3) & 0xffffffe0, pdptes, sizeof(pdptes));
        vmx_write(VMCS_GUEST_PDPTE0, pdptes[0]);
        vmx_write(VMCS_GUEST_PDPTE1, pdptes[1]);
        vmx_write(VMCS_GUEST_PDPTE2, pdptes[2]);
        vmx_write(VMCS_GUEST_PDPTE3, pdptes[3]);
    }
    return true;
}

static void vmx_handle_cr_access(struct vcpu *v, uint64_t qual) {
    unsigned cr = qual & 15;
    unsigned type = (qual >> 4) & 3;
    unsigned reg = (qual >> 8) & 15;

    // Only CR0 and CR4 writes trap, through their guest/host masks; LMSW and
    // CLTS can only touch CR0 bits the guest owns entirely.
    if (type != 0 || (cr != 0 && cr != 4)) {
        panic("Unexpected control register access, qualification %#llx\n",
              (unsigned long long)qual);
    }

    uint64_t value = reg == 4 ? vmx_read(VMCS_GUEST_RSP) : ((uint64_t *)&v->gprs)[reg];
    uint64_t cr0 = cr == 0 ? vmx_cr0_real(value) : vmx_read(VMCS_GUEST_CR0);
    uint64_t cr4 = cr == 4 ? vmx_cr4_real(value) : vmx_read(VMCS_GUEST_CR4);
    // Long mode cannot do without PAE, and 64-bit code without paging.
    if (vmx_read(VMCS_GUEST_EFER) & EFER_LMA) {
        bool code64 = (vmx_read(VMCS_GUEST_CS_AR) & VMX_AR_L) != 0;
        if (!(cr4 & CR4_PAE) || (code64 && !(cr0 & CR0_PG))) {
            vcpu_inject_exception(v, 13, true, 0);
            return;
        }
    }
    if (!vmx_paging_changed(cr0, cr4)) {
        vcpu_inject_exception(v, 13, true, 0);
        return;
    }
    if (cr == 0) {
        v->cr0_shadow = value;
        vmx_write(VMCS_CR0_SHADOW, value);
        vmx_write(VMCS_GUEST_CR0, cr0);
    } else {
        v->cr4_shadow = value;
        vmx_write(VMCS_CR4_SHADOW, value);
        vmx_write(VMCS_GUEST_CR4, cr4);
    }
    vcpu_skip_instruction(v);
}

static void vmx_handle_io(struct vcpu *v, uint64_t qual) {
    unsigned size = (qual & 7) + 1;
    bool in = (qual >> 3) & 1;
    bool string = (qual >> 4) & 1;
    uint16_t port = qual >> 16;

    if (string) {
        panic("String I/O to trapped port %#x is not supported yet\n", port);
    }

    uint32_t value = (uint32_t)v->gprs.rax;
    exit_io(v, port, size, in, &value);
    if (in) {
        uint64_t mask = size == 4 ? 0xffffffffull : (1ull << (size * 8)) - 1;
        if (size == 4) {
            v->gprs.rax = value;
        } else {
            v->gprs.rax = (v->gprs.rax & ~mask) | (value & mask);
        }
    }
    vcpu_skip_instruction(v);
}

static void vmx_handle_exit(struct vcpu *v) {
    uint32_t reason = vmx_read(VMCS_EXIT_REASON);
    uint64_t qual = vmx_read(VMCS_EXIT_QUALIFICATION);

    if (reason & EXIT_REASON_ENTRY_FAILURE) {
        printf("VM entry failed, exit reason %#x, qualification %#llx\n", reason,
               (unsigned long long)qual);
        vmx_dump_guest();
        panic("Cannot enter the guest\n");
    }

    switch (reason & 0xffff) {
        case EXIT_REASON_CPUID: {
            exit_cpuid(v);
            break;
        }
        case EXIT_REASON_VMCALL: {
            exit_hypercall(v);
            break;
        }
        case EXIT_REASON_CR_ACCESS: {
            vmx_handle_cr_access(v, qual);
            break;
        }
        case EXIT_REASON_IO: {
            vmx_handle_io(v, qual);
            break;
        }
        case EXIT_REASON_RDMSR: {
            exit_rdmsr(v);
            break;
        }
        case EXIT_REASON_WRMSR: {
            exit_wrmsr(v);
            break;
        }
        case EXIT_REASON_XSETBV: {
            exit_xsetbv(v);
            break;
        }
        case EXIT_REASON_INVD: {
            // Throwing away the host's dirty cache lines is not an option.
            wbinvd();
            vcpu_skip_instruction(v);
            break;
        }
        case EXIT_REASON_INIT: {
            vcpu_init_signal(v);
            break;
        }
        case EXIT_REASON_SIPI: {
            vcpu_startup(v, (uint8_t)qual);
            break;
        }
        case EXIT_REASON_TRIPLE_FAULT: {
            printf("Guest triple fault\n");
            vmx_dump_guest();
            power_reset(false);
        }
        case EXIT_REASON_GETSEC:
        case EXIT_REASON_VMCLEAR:
        case EXIT_REASON_VMLAUNCH:
        case EXIT_REASON_VMPTRLD:
        case EXIT_REASON_VMPTRST:
        case EXIT_REASON_VMREAD:
        case EXIT_REASON_VMRESUME:
        case EXIT_REASON_VMWRITE:
        case EXIT_REASON_VMXOFF:
        case EXIT_REASON_VMXON:
        case EXIT_REASON_INVEPT:
        case EXIT_REASON_INVVPID:
        case EXIT_REASON_VMFUNC: {
            vcpu_inject_exception(v, 6, false, 0);
            break;
        }
        case EXIT_REASON_EPT_VIOLATION: {
            uint64_t gpa = vmx_read(VMCS_GUEST_PHYS_ADDR);
            if ((qual & EPT_QUAL_WRITE)
                && (exit_absent_write(v, gpa) || vapic_mmio_write(v, gpa))) {
                break;
            }
            printf("EPT violation at GPA %#llx, qualification %#llx\n", (unsigned long long)gpa,
                   (unsigned long long)qual);
            vmx_dump_guest();
            panic("Unhandled guest-physical access\n");
        }
        default: {
            printf("Unhandled VM exit, reason %u, qualification %#llx\n", reason,
                   (unsigned long long)qual);
            vmx_dump_guest();
            panic("Unhandled VM exit\n");
        }
    }
}

void vmx_run(struct vcpu *v) {
    asm volatile ("vmptrld %0" :: "m"(v->vmcs_pa) : "cc", "memory");

    for (;;) {
        if (vmx_enter(&v->gprs, v->vmx_launched) != 0) {
            uint64_t error = vmx_read(VMCS_VM_INSTR_ERROR);
            printf("VM%s failed, VM-instruction error %llu\n",
                   v->vmx_launched ? "RESUME" : "LAUNCH", (unsigned long long)error);
            vmx_dump_guest();
            panic("Cannot enter the guest\n");
        }
        v->vmx_launched = true;
        vmx_handle_exit(v);
    }
}
