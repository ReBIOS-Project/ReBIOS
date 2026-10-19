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

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <memory.h>
#include <core.h>
#include <handoff.h>
#include <lib/heap.h>
#include <lib/log.h>
#include <sys/x86.h>
#include <virt/gpt.h>
#include <virt/mmio.h>
#include <virt/nvmx.h>
#include <virt/vcpu.h>
#include <virt/virt.h>
#include <virt/vmx.h>

// The guest's VMCS regions hold the revision identifier and the VMX-abort
// indicator where the processor's do (SDM 325462-093 Vol. 3C, 27.2), then
// ReBIOS's own: the launch state, and the fields in the order of the table
// below. Software is not to read or write them otherwise.
#define VMCS12_REVISION 0x52424e56u
#define VMCS12_LAUNCHED 8
#define VMCS12_FIELDS 16

// VM-instruction error numbers (SDM 325462-093 Vol. 3C, Table 33-1).
#define ERROR_VMCLEAR_ADDRESS 2
#define ERROR_VMCLEAR_VMXON 3
#define ERROR_VMLAUNCH_NOT_CLEAR 4
#define ERROR_VMRESUME_NOT_LAUNCHED 5
#define ERROR_ENTRY_CONTROLS 7
#define ERROR_VMPTRLD_ADDRESS 9
#define ERROR_VMPTRLD_VMXON 10
#define ERROR_VMPTRLD_REVISION 11
#define ERROR_UNSUPPORTED_FIELD 12
#define ERROR_VMXON_IN_ROOT 15
#define ERROR_ENTRY_MOV_SS 26

#define VMX_RESULT_FLAGS (RFLAGS_CF | RFLAGS_PF | RFLAGS_AF | RFLAGS_ZF | RFLAGS_SF | RFLAGS_OF)

#define NO_VMCS (~0ull)

// Field encodings: access type, index, type, and width (SDM 325462-093 Vol.
// 3C, 27.11.2).
#define FIELD_HIGH 1u
#define FIELD_INDEX(field) ((field) >> 1 & 0x1ff)
#define FIELD_TYPE(field) ((field) >> 10 & 3)
#define FIELD_WIDTH(field) ((field) >> 13 & 3)
#define FIELD_RESERVED 0xffff9000u
#define FIELD_TYPE_EXIT_INFO 1
#define FIELD_WIDTH_16 0
#define FIELD_WIDTH_64 1
#define FIELD_WIDTH_32 2

// The fields the guest's VMCSs have: the controls of what it is offered, and
// all of the guest state, host state, and exit information.
static const uint16_t fields[] = {
    0x0000,
    0x0800, 0x0802, 0x0804, 0x0806, 0x0808, 0x080a, 0x080c, 0x080e,
    0x0c00, 0x0c02, 0x0c04, 0x0c06, 0x0c08, 0x0c0a, 0x0c0c,
    0x2000, 0x2002, 0x2004, 0x2006, 0x2008, 0x200a, 0x2010, 0x201a,
    0x2400,
    0x2800, 0x2802, 0x2804, 0x2806, 0x280a, 0x280c, 0x280e, 0x2810,
    0x2c00, 0x2c02,
    0x4000, 0x4002, 0x4004, 0x4006, 0x4008, 0x400a, 0x400c, 0x400e, 0x4010, 0x4012, 0x4014,
    0x4016, 0x4018, 0x401a, 0x401e,
    0x4400, 0x4402, 0x4404, 0x4406, 0x4408, 0x440a, 0x440c, 0x440e,
    0x4800, 0x4802, 0x4804, 0x4806, 0x4808, 0x480a, 0x480c, 0x480e, 0x4810, 0x4812,
    0x4814, 0x4816, 0x4818, 0x481a, 0x481c, 0x481e, 0x4820, 0x4822,
    0x4824, 0x4826, 0x4828, 0x482a,
    0x4c00,
    0x6000, 0x6002, 0x6004, 0x6006, 0x6008, 0x600a, 0x600c, 0x600e,
    0x6400, 0x6402, 0x6404, 0x6406, 0x6408, 0x640a,
    0x6800, 0x6802, 0x6804, 0x6806, 0x6808, 0x680a, 0x680c, 0x680e, 0x6810, 0x6812, 0x6814,
    0x6816, 0x6818, 0x681a, 0x681c, 0x681e, 0x6820, 0x6822, 0x6824, 0x6826,
    0x6c00, 0x6c02, 0x6c04, 0x6c06, 0x6c08, 0x6c0a, 0x6c0c, 0x6c0e, 0x6c10, 0x6c12, 0x6c14, 0x6c16,
};

#define FIELD_COUNT (sizeof(fields) / sizeof(fields[0]))
#define MAX_INDEX 64

_Static_assert(VMCS12_FIELDS + FIELD_COUNT * 8 <= 4096, "VMCS12 layout");

// By width, type, and index: the slot plus one, or 0 where there is no such
// field.
static uint8_t field_slots[4][4][MAX_INDEX];

// VMX as the guest gets it.
struct nvmx {
    bool vmxon;
    uint64_t vmxon_ptr;
    uint64_t current;
    uint8_t *vmcs12;
};

bool nvmx_enabled;

// The guest's VMX capability MSRs: the processor's allowed settings where
// ReBIOS emulates them. Their allowed 0-settings are the processor's, so that
// the guest's controls are the processor's to take.
static uint64_t cap_basic;
static uint64_t cap_pin, cap_proc, cap_exit, cap_entry;
static uint64_t cap_true_pin, cap_true_proc, cap_true_exit, cap_true_entry;
static uint64_t cap_proc2, cap_misc;
static uint64_t cap_cr0_fixed0, cap_cr0_fixed1, cap_cr4_fixed0, cap_cr4_fixed1;
static uint64_t cap_vmcs_enum;

// What the default1 classes of the controls the old capability MSRs report
// as fixed (SDM 325462-093 Vol. 3D, A.2).
#define PIN_DEFAULT1 0x00000016u
#define PROC_DEFAULT1 0x0401e172u
#define EXIT_DEFAULT1 0x00036dffu
#define ENTRY_DEFAULT1 0x000011ffu

#define PIN_EMULATED (PIN_EXT_INTR_EXITING | PIN_NMI_EXITING | PIN_VIRTUAL_NMIS | PIN_DEFAULT1)
#define PROC_EMULATED                                                                         \
    (PROC_INTR_WINDOW_EXITING | PROC_TSC_OFFSETTING | PROC_HLT_EXITING | PROC_INVLPG_EXITING \
     | PROC_MWAIT_EXITING | PROC_RDPMC_EXITING | PROC_RDTSC_EXITING | PROC_CR3_LOAD_EXITING   \
     | PROC_CR3_STORE_EXITING | PROC_CR8_LOAD_EXITING | PROC_CR8_STORE_EXITING                 \
     | PROC_NMI_WINDOW_EXITING | PROC_MOV_DR_EXITING | PROC_UNCOND_IO_EXITING                  \
     | PROC_IO_BITMAPS | PROC_MONITOR_TRAP_FLAG | PROC_MSR_BITMAPS | PROC_MONITOR_EXITING        \
     | PROC_PAUSE_EXITING | PROC_SECONDARY_CONTROLS | PROC_DEFAULT1)
#define PROC2_EMULATED                                                                    \
    (PROC2_DESC_TABLE_EXITING | PROC2_RDTSCP | PROC2_WBINVD_EXITING | PROC2_RDRAND_EXITING \
     | PROC2_INVPCID | PROC2_RDSEED_EXITING)
#define EXIT_EMULATED                                                                        \
    (EXIT_SAVE_DEBUG_CONTROLS | EXIT_HOST_ADDR_SPACE_SIZE | EXIT_ACK_INTR_ON_EXIT | EXIT_SAVE_PAT \
     | EXIT_LOAD_PAT | EXIT_SAVE_EFER | EXIT_LOAD_EFER | EXIT_DEFAULT1)
#define ENTRY_EMULATED \
    (ENTRY_LOAD_DEBUG_CONTROLS | ENTRY_IA32E_MODE_GUEST | ENTRY_LOAD_PAT | ENTRY_LOAD_EFER \
     | ENTRY_DEFAULT1)

// IA32_VMX_BASIC (SDM 325462-093 Vol. 3D, A.1): the VMCS size, write-back
// memory, the information of INS and OUTS exits, the TRUE capability MSRs,
// and whether exceptions can be injected without their error codes.
#define BASIC_SIZE_SHIFT 32
#define BASIC_MEMTYPE_WB (6ull << 50)
#define BASIC_INS_OUTS (1ull << 54)
#define BASIC_TRUE_CONTROLS (1ull << 55)
#define BASIC_NO_ERROR_CODE (1ull << 56)

// IA32_VMX_MISC (SDM 325462-093 Vol. 3D, A.6): exits store EFER.LMA in the
// IA-32e mode guest control, the HLT activity state, four CR3-target values,
// and VMWRITE writes the exit information too.
#define MISC_STORE_LMA (1ull << 5)
#define MISC_ACTIVITY_HLT (1ull << 6)
#define MISC_CR3_TARGETS_SHIFT 16
#define MISC_VMWRITE_ANY (1ull << 29)
#define MISC_ZERO_LENGTH_INJECTION (1ull << 30)

#define FEATURE_CONTROL_GUEST (FEATURE_CONTROL_LOCK | FEATURE_CONTROL_VMX_OUTSIDE_SMX)

// A capability MSR: allowed 0-settings low, allowed 1-settings high.
static uint64_t capability(uint64_t hardware, uint32_t emulated, uint32_t required) {
    uint32_t allowed1 = (uint32_t)(hardware >> 32) & emulated;
    uint32_t allowed0 = ((uint32_t)hardware | required) & allowed1;
    return (uint64_t)allowed1 << 32 | allowed0;
}

void nvmx_init(void) {
    if (!virt_vmx || !core_handoff->config.nested) {
        return;
    }
    nvmx_enabled = true;

    uint32_t max_index = 0;
    for (unsigned i = 0; i < FIELD_COUNT; i++) {
        uint32_t field = fields[i];
        field_slots[FIELD_WIDTH(field)][FIELD_TYPE(field)][FIELD_INDEX(field)] = (uint8_t)(i + 1);
        if (FIELD_INDEX(field) > max_index) {
            max_index = FIELD_INDEX(field);
        }
    }
    cap_vmcs_enum = max_index << 1;

    uint64_t basic = vmx_capability(MSR_IA32_VMX_BASIC);
    cap_basic = VMCS12_REVISION | 4096ull << BASIC_SIZE_SHIFT | BASIC_MEMTYPE_WB | BASIC_INS_OUTS
                | BASIC_TRUE_CONTROLS | (basic & BASIC_NO_ERROR_CODE);

    cap_true_pin = capability(vmx_capability(MSR_IA32_VMX_TRUE_PINBASED_CTLS), PIN_EMULATED, 0);
    cap_true_proc = capability(vmx_capability(MSR_IA32_VMX_TRUE_PROCBASED_CTLS), PROC_EMULATED, 0);
    cap_true_exit = capability(vmx_capability(MSR_IA32_VMX_TRUE_EXIT_CTLS), EXIT_EMULATED, 0);
    cap_true_entry = capability(vmx_capability(MSR_IA32_VMX_TRUE_ENTRY_CTLS), ENTRY_EMULATED, 0);
    cap_pin = capability(cap_true_pin, PIN_EMULATED, PIN_DEFAULT1);
    cap_proc = capability(cap_true_proc, PROC_EMULATED, PROC_DEFAULT1);
    cap_exit = capability(cap_true_exit, EXIT_EMULATED, EXIT_DEFAULT1);
    cap_entry = capability(cap_true_entry, ENTRY_EMULATED, ENTRY_DEFAULT1);
    cap_proc2 = capability(vmx_capability(MSR_IA32_VMX_PROCBASED_CTLS2), PROC2_EMULATED, 0);

    uint64_t misc = vmx_capability(MSR_IA32_VMX_MISC);
    cap_misc = MISC_STORE_LMA | MISC_ACTIVITY_HLT | 4ull << MISC_CR3_TARGETS_SHIFT
               | MISC_VMWRITE_ANY | (misc & MISC_ZERO_LENGTH_INJECTION);

    cap_cr0_fixed0 = vmx_capability(MSR_IA32_VMX_CR0_FIXED0);
    cap_cr0_fixed1 = vmx_capability(MSR_IA32_VMX_CR0_FIXED1);
    cap_cr4_fixed0 = vmx_capability(MSR_IA32_VMX_CR4_FIXED0);
    cap_cr4_fixed1 = vmx_capability(MSR_IA32_VMX_CR4_FIXED1);

    virt_intercept_msr(MSR_IA32_FEATURE_CONTROL, true, true);
    for (uint32_t msr = MSR_IA32_VMX_BASIC; msr <= MSR_IA32_VMX_PROCBASED_CTLS3; msr++) {
        virt_intercept_msr(msr, true, true);
    }
    printf("Nested VMX for the guest\n");
}

void nvmx_vcpu_init(struct vcpu *v) {
    if (!nvmx_enabled) {
        return;
    }
    struct nvmx *n = kmalloc(sizeof(*n));
    *n = (struct nvmx){.current = NO_VMCS};
    v->nvmx = n;
}

static int field_slot(uint32_t field) {
    if (field & FIELD_RESERVED) {
        return -1;
    }
    if ((field & FIELD_HIGH) && FIELD_WIDTH(field) != FIELD_WIDTH_64) {
        return -1;
    }
    if (FIELD_INDEX(field) >= MAX_INDEX) {
        return -1;
    }
    return field_slots[FIELD_WIDTH(field)][FIELD_TYPE(field)][FIELD_INDEX(field)] - 1;
}

static uint64_t *field_at(const struct nvmx *n, int slot) {
    return (uint64_t *)(n->vmcs12 + VMCS12_FIELDS + (size_t)slot * 8);
}

// The guest's RAM at a guest-physical page, for the regions VMX instructions
// name.
static uint8_t *guest_page(uint64_t gpa) {
    uint64_t hpa;
    uint32_t prot;
    uint8_t memtype;
    if (!gpt_translate(gpa, &hpa, &prot, &memtype) || (prot & (GPT_R | GPT_W)) != (GPT_R | GPT_W)
        || memtype != MT_WB) {
        return NULL;
    }
    return (uint8_t *)(uintptr_t)hpa;
}

static unsigned guest_cpl(struct vcpu *v) {
    struct segment ss;
    vcpu_get_seg(v, SEG_SS, &ss);
    return (ss.attrib >> 5) & 3;
}

static bool long_mode_64(struct vcpu *v) {
    struct segment cs;
    vcpu_get_seg(v, SEG_CS, &cs);
    return (vcpu_efer(v) & EFER_LMA) && (cs.attrib & SEG_L);
}

static uint64_t guest_reg(struct vcpu *v, unsigned reg) {
    return reg == 4 ? vcpu_rsp(v) : ((const uint64_t *)&v->gprs)[reg];
}

static void set_guest_reg(struct vcpu *v, unsigned reg, uint64_t value) {
    if (reg == 4) {
        vcpu_set_rsp(v, value);
    } else {
        ((uint64_t *)&v->gprs)[reg] = value;
    }
}

// Instruction information (SDM 325462-093 Vol. 3C, Tables 30-14 and 30-15).
#define INFO_SCALE(info) ((info) & 3)
#define INFO_REG1(info) ((info) >> 3 & 15)
#define INFO_ADDRESS_SIZE(info) ((info) >> 7 & 7)
#define INFO_REGISTER (1u << 10)
#define INFO_SEGMENT(info) ((info) >> 15 & 7)
#define INFO_INDEX(info) ((info) >> 18 & 15)
#define INFO_NO_INDEX (1u << 22)
#define INFO_BASE(info) ((info) >> 23 & 15)
#define INFO_NO_BASE (1u << 27)
#define INFO_REG2(info) ((info) >> 28 & 15)

// The linear address of a VMX instruction's memory operand, or false with
// the fault raised.
static bool operand_address(struct vcpu *v, uint32_t info, unsigned size, uint64_t *linear) {
    uint64_t address = vmx_read(VMCS_EXIT_QUALIFICATION);
    if (!(info & INFO_NO_BASE)) {
        address += guest_reg(v, INFO_BASE(info));
    }
    if (!(info & INFO_NO_INDEX)) {
        address += guest_reg(v, INFO_INDEX(info)) << INFO_SCALE(info);
    }
    switch (INFO_ADDRESS_SIZE(info)) {
        case 0: {
            address &= 0xffff;
            break;
        }
        case 1: {
            address &= 0xffffffff;
            break;
        }
        default: {
            break;
        }
    }
    enum seg_reg reg = (enum seg_reg)INFO_SEGMENT(info);
    struct segment seg;
    vcpu_get_seg(v, reg, &seg);
    uint8_t fault = reg == SEG_SS ? 12 : 13;
    if (long_mode_64(v)) {
        if (reg == SEG_FS || reg == SEG_GS) {
            address += seg.base;
        }
        unsigned shift = (vcpu_cr4(v) & CR4_LA57) ? 7 : 16;
        if ((uint64_t)((int64_t)(address << shift) >> shift) != address) {
            vcpu_inject_exception(v, fault, true, 0);
            return false;
        }
    } else {
        if ((seg.attrib & SEG_UNUSABLE) || address + size - 1 > seg.limit) {
            vcpu_inject_exception(v, fault, true, 0);
            return false;
        }
        address = (address + seg.base) & 0xffffffff;
    }
    *linear = address;
    return true;
}

static bool read_operand64(struct vcpu *v, uint32_t info, uint64_t *value) {
    uint64_t linear;
    return operand_address(v, info, 8, &linear) && mmio_read_linear(v, linear, value, 8);
}

// VMsucceed, VMfailInvalid, and VMfailValid (SDM 325462-093 Vol. 3C, 33.2).
static void result(struct vcpu *v, uint64_t flags) {
    vcpu_set_rflags(v, (vcpu_rflags(v) & ~(uint64_t)VMX_RESULT_FLAGS) | flags);
    vcpu_skip_instruction(v);
}

static void succeed(struct vcpu *v) {
    result(v, 0);
}

static void fail(struct vcpu *v, uint32_t error) {
    struct nvmx *n = v->nvmx;
    if (n->vmcs12 == NULL) {
        result(v, RFLAGS_CF);
        return;
    }
    *field_at(n, field_slot(VMCS_VM_INSTR_ERROR)) = error;
    result(v, RFLAGS_ZF);
}

static void fail_invalid(struct vcpu *v) {
    result(v, RFLAGS_CF);
}

// The physical addresses a VMX instruction takes, 4 KiB-aligned within the
// processor's physical-address width.
static bool valid_address(uint64_t addr) {
    unsigned width = cpuid(0x80000008, 0).eax & 0xff;
    return (addr & 0xfff) == 0 && (addr >> width) == 0;
}

static void vmxon(struct vcpu *v, struct nvmx *n, uint32_t info) {
    if (n->vmxon) {
        fail(v, ERROR_VMXON_IN_ROOT);
        return;
    }
    uint64_t cr0 = vcpu_cr0(v);
    uint64_t cr4 = vcpu_cr4(v);
    if ((cr0 & cap_cr0_fixed0) != cap_cr0_fixed0 || (cr0 & ~cap_cr0_fixed1)
        || (cr4 & cap_cr4_fixed0) != cap_cr4_fixed0 || (cr4 & ~cap_cr4_fixed1)) {
        vcpu_inject_exception(v, 13, true, 0);
        return;
    }
    uint64_t addr;
    if (!read_operand64(v, info, &addr)) {
        return;
    }
    uint8_t *region = valid_address(addr) ? guest_page(addr) : NULL;
    if (region == NULL || *(volatile uint32_t *)region != VMCS12_REVISION) {
        fail_invalid(v);
        return;
    }
    n->vmxon = true;
    n->vmxon_ptr = addr;
    n->current = NO_VMCS;
    n->vmcs12 = NULL;
    succeed(v);
}

static void vmxoff(struct vcpu *v, struct nvmx *n) {
    n->vmxon = false;
    n->current = NO_VMCS;
    n->vmcs12 = NULL;
    succeed(v);
}

static void vmclear(struct vcpu *v, struct nvmx *n, uint32_t info) {
    uint64_t addr;
    if (!read_operand64(v, info, &addr)) {
        return;
    }
    uint8_t *region = valid_address(addr) ? guest_page(addr) : NULL;
    if (region == NULL) {
        fail(v, ERROR_VMCLEAR_ADDRESS);
        return;
    }
    if (addr == n->vmxon_ptr) {
        fail(v, ERROR_VMCLEAR_VMXON);
        return;
    }
    *(volatile uint32_t *)(region + VMCS12_LAUNCHED) = 0;
    if (addr == n->current) {
        n->current = NO_VMCS;
        n->vmcs12 = NULL;
    }
    succeed(v);
}

static void vmptrld(struct vcpu *v, struct nvmx *n, uint32_t info) {
    uint64_t addr;
    if (!read_operand64(v, info, &addr)) {
        return;
    }
    uint8_t *region = valid_address(addr) ? guest_page(addr) : NULL;
    if (region == NULL) {
        fail(v, ERROR_VMPTRLD_ADDRESS);
        return;
    }
    if (addr == n->vmxon_ptr) {
        fail(v, ERROR_VMPTRLD_VMXON);
        return;
    }
    if (*(volatile uint32_t *)region != VMCS12_REVISION) {
        fail(v, ERROR_VMPTRLD_REVISION);
        return;
    }
    n->current = addr;
    n->vmcs12 = region;
    succeed(v);
}

static void vmptrst(struct vcpu *v, struct nvmx *n, uint32_t info) {
    uint64_t linear;
    if (!operand_address(v, info, 8, &linear) || !mmio_write_linear(v, linear, &n->current, 8)) {
        return;
    }
    succeed(v);
}

// Outside 64-bit mode, VMREAD and VMWRITE take 32-bit operands (SDM
// 325462-093 Vol. 3C, 33.3).
static void vmread(struct vcpu *v, struct nvmx *n, uint32_t info) {
    if (n->vmcs12 == NULL) {
        fail_invalid(v);
        return;
    }
    bool wide = long_mode_64(v);
    uint32_t field = (uint32_t)guest_reg(v, INFO_REG2(info));
    int slot = wide && (guest_reg(v, INFO_REG2(info)) >> 32) ? -1 : field_slot(field);
    if (slot < 0) {
        fail(v, ERROR_UNSUPPORTED_FIELD);
        return;
    }
    uint64_t value = *field_at(n, slot);
    if (field & FIELD_HIGH) {
        value >>= 32;
    }
    if (!wide) {
        value &= 0xffffffff;
    }
    if (info & INFO_REGISTER) {
        set_guest_reg(v, INFO_REG1(info), value);
    } else {
        uint64_t linear;
        unsigned size = wide ? 8 : 4;
        if (!operand_address(v, info, size, &linear) || !mmio_write_linear(v, linear, &value, size)) {
            return;
        }
    }
    succeed(v);
}

static void vmwrite(struct vcpu *v, struct nvmx *n, uint32_t info) {
    if (n->vmcs12 == NULL) {
        fail_invalid(v);
        return;
    }
    bool wide = long_mode_64(v);
    uint64_t value = 0;
    if (info & INFO_REGISTER) {
        value = guest_reg(v, INFO_REG1(info));
    } else {
        uint64_t linear;
        unsigned size = wide ? 8 : 4;
        if (!operand_address(v, info, size, &linear) || !mmio_read_linear(v, linear, &value, size)) {
            return;
        }
    }
    if (!wide) {
        value &= 0xffffffff;
    }
    uint32_t field = (uint32_t)guest_reg(v, INFO_REG2(info));
    int slot = wide && (guest_reg(v, INFO_REG2(info)) >> 32) ? -1 : field_slot(field);
    if (slot < 0) {
        fail(v, ERROR_UNSUPPORTED_FIELD);
        return;
    }
    uint64_t *at = field_at(n, slot);
    if (field & FIELD_HIGH) {
        *at = (*at & 0xffffffff) | value << 32;
    } else {
        switch (FIELD_WIDTH(field)) {
            case FIELD_WIDTH_16: {
                value &= 0xffff;
                break;
            }
            case FIELD_WIDTH_32: {
                value &= 0xffffffff;
                break;
            }
            default: {
                break;
            }
        }
        *at = value;
    }
    succeed(v);
}

static void vmlaunch(struct vcpu *v, struct nvmx *n, bool resume) {
    if (n->vmcs12 == NULL) {
        fail_invalid(v);
        return;
    }
    if (vmx_read(VMCS_GUEST_INTERRUPTIBILITY) & INTERRUPTIBILITY_MOV_SS) {
        fail(v, ERROR_ENTRY_MOV_SS);
        return;
    }
    uint32_t launched = *(volatile uint32_t *)(n->vmcs12 + VMCS12_LAUNCHED);
    if (!resume && launched) {
        fail(v, ERROR_VMLAUNCH_NOT_CLEAR);
        return;
    }
    if (resume && !launched) {
        fail(v, ERROR_VMRESUME_NOT_LAUNCHED);
        return;
    }
    fail(v, ERROR_ENTRY_CONTROLS);
}

void nvmx_instruction(struct vcpu *v, uint32_t reason) {
    struct nvmx *n = v->nvmx;
    // The processor raises these itself in real mode, virtual-8086 mode and
    // compatibility mode, but not for the guest's CR4.VMXE, which only the
    // shadow has (SDM 325462-093 Vol. 3C, 33.3).
    if (n == NULL || !(vcpu_cr4(v) & CR4_VMXE) || (reason != EXIT_REASON_VMXON && !n->vmxon)
        || reason == EXIT_REASON_INVEPT || reason == EXIT_REASON_INVVPID
        || reason == EXIT_REASON_VMFUNC || reason == EXIT_REASON_GETSEC) {
        vcpu_inject_exception(v, 6, false, 0);
        return;
    }
    if (guest_cpl(v) > 0) {
        vcpu_inject_exception(v, 13, true, 0);
        return;
    }
    uint32_t info = (uint32_t)vmx_read(VMCS_EXIT_INSTR_INFO);
    switch (reason) {
        case EXIT_REASON_VMXON: {
            vmxon(v, n, info);
            break;
        }
        case EXIT_REASON_VMXOFF: {
            vmxoff(v, n);
            break;
        }
        case EXIT_REASON_VMCLEAR: {
            vmclear(v, n, info);
            break;
        }
        case EXIT_REASON_VMPTRLD: {
            vmptrld(v, n, info);
            break;
        }
        case EXIT_REASON_VMPTRST: {
            vmptrst(v, n, info);
            break;
        }
        case EXIT_REASON_VMREAD: {
            vmread(v, n, info);
            break;
        }
        case EXIT_REASON_VMWRITE: {
            vmwrite(v, n, info);
            break;
        }
        case EXIT_REASON_VMLAUNCH:
        case EXIT_REASON_VMRESUME: {
            vmlaunch(v, n, reason == EXIT_REASON_VMRESUME);
            break;
        }
        default: {
            vcpu_inject_exception(v, 6, false, 0);
            break;
        }
    }
}

bool nvmx_rdmsr(struct vcpu *v, uint32_t msr, uint64_t *value, bool *fault) {
    (void)v;
    *fault = false;
    if (!nvmx_enabled) {
        return false;
    }
    switch (msr) {
        case MSR_IA32_FEATURE_CONTROL: {
            *value = FEATURE_CONTROL_GUEST;
            return true;
        }
        case MSR_IA32_VMX_BASIC: {
            *value = cap_basic;
            return true;
        }
        case MSR_IA32_VMX_PINBASED_CTLS: {
            *value = cap_pin;
            return true;
        }
        case MSR_IA32_VMX_PROCBASED_CTLS: {
            *value = cap_proc;
            return true;
        }
        case MSR_IA32_VMX_EXIT_CTLS: {
            *value = cap_exit;
            return true;
        }
        case MSR_IA32_VMX_ENTRY_CTLS: {
            *value = cap_entry;
            return true;
        }
        case MSR_IA32_VMX_MISC: {
            *value = cap_misc;
            return true;
        }
        case MSR_IA32_VMX_CR0_FIXED0: {
            *value = cap_cr0_fixed0;
            return true;
        }
        case MSR_IA32_VMX_CR0_FIXED1: {
            *value = cap_cr0_fixed1;
            return true;
        }
        case MSR_IA32_VMX_CR4_FIXED0: {
            *value = cap_cr4_fixed0;
            return true;
        }
        case MSR_IA32_VMX_CR4_FIXED1: {
            *value = cap_cr4_fixed1;
            return true;
        }
        case MSR_IA32_VMX_VMCS_ENUM: {
            *value = cap_vmcs_enum;
            return true;
        }
        case MSR_IA32_VMX_PROCBASED_CTLS2: {
            *value = cap_proc2;
            return true;
        }
        case MSR_IA32_VMX_TRUE_PINBASED_CTLS: {
            *value = cap_true_pin;
            return true;
        }
        case MSR_IA32_VMX_TRUE_PROCBASED_CTLS: {
            *value = cap_true_proc;
            return true;
        }
        case MSR_IA32_VMX_TRUE_EXIT_CTLS: {
            *value = cap_true_exit;
            return true;
        }
        case MSR_IA32_VMX_TRUE_ENTRY_CTLS: {
            *value = cap_true_entry;
            return true;
        }
        case MSR_IA32_VMX_EPT_VPID_CAP:
        case MSR_IA32_VMX_VMFUNC:
        case MSR_IA32_VMX_PROCBASED_CTLS3: {
            *fault = true;
            return true;
        }
        default: {
            return false;
        }
    }
}

bool nvmx_wrmsr(struct vcpu *v, uint32_t msr, uint64_t value, bool *fault) {
    (void)v;
    (void)value;
    *fault = false;
    if (!nvmx_enabled
        || (msr != MSR_IA32_FEATURE_CONTROL
            && (msr < MSR_IA32_VMX_BASIC || msr > MSR_IA32_VMX_PROCBASED_CTLS3))) {
        return false;
    }
    // Locked, and the capability MSRs read-only.
    *fault = true;
    return true;
}

bool nvmx_cr_allowed(struct vcpu *v, unsigned cr, uint64_t value) {
    struct nvmx *n = v->nvmx;
    if (cr == 4 && (value & CR4_VMXE) && !nvmx_enabled) {
        return false;
    }
    if (n == NULL || !n->vmxon) {
        return true;
    }
    // VMX operation keeps CR0 and CR4 to the fixed bits (SDM 325462-093 Vol.
    // 3C, 26.8).
    if (cr == 0) {
        return (value & cap_cr0_fixed0) == cap_cr0_fixed0 && !(value & ~cap_cr0_fixed1);
    }
    return (value & cap_cr4_fixed0) == cap_cr4_fixed0 && !(value & ~cap_cr4_fixed1);
}
