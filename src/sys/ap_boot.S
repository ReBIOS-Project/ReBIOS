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

// Where the other processors start, copied to a page below 1 MiB. It runs from
// wherever that page is, with the page's base in EBP, enters long mode on the
// hypervisor's page tables, and calls the entry point on the stack of the
// processor's area, which it finds by APIC ID. The far pointers and the GDTR's
// base are filled in for the page.

// Offsets into ap_boot_params, struct ap_boot_params in smp.c.
.set PARAM_CR3, 0
.set PARAM_CR4, 8
.set PARAM_EFER, 16
.set PARAM_TABLE, 24
.set PARAM_COUNT, 32
.set PARAM_STACK_OFFSET, 40
.set PARAM_ENTRY, 48
.set PARAM_SIZE, 56

// struct ap_boot_entry in smp.c.
.set ENTRY_APIC_ID, 0
.set ENTRY_CPU, 8
.set ENTRY_SIZE, 16

.set PARAMS, ap_boot_params - ap_boot_start

.section .rodata
.balign 16
.globl ap_boot_start
.type ap_boot_start, %object
.globl ap_boot_gdt
.type ap_boot_gdt, %object
.globl ap_boot_gdtr
.type ap_boot_gdtr, %object
.globl ap_boot_pm32
.type ap_boot_pm32, %object
.globl ap_boot_pm32_ptr
.type ap_boot_pm32_ptr, %object
.globl ap_boot_lm64
.type ap_boot_lm64, %object
.globl ap_boot_lm64_ptr
.type ap_boot_lm64_ptr, %object
.globl ap_boot_params
.type ap_boot_params, %object
.globl ap_boot_end
.type ap_boot_end, %object

.code16
ap_boot_start:
    cli
    cld
    xorl %ebp, %ebp
    movw %cs, %bp
    movw %bp, %ds
    shll $4, %ebp
    lgdtl ap_boot_gdtr - ap_boot_start
    movl %cr0, %eax
    orl $1, %eax
    movl %eax, %cr0
    ljmpl *(ap_boot_pm32_ptr - ap_boot_start)

.code32
ap_boot_pm32:
    movl $0x10, %eax
    movl %eax, %ds
    movl %eax, %es
    movl %eax, %ss
    // Intel's firmware may limit CPUID's leaves, which the APIC ID lookup
    // reads, or disable the XD bit, which EFER.NXE needs: IA32_MISC_ENABLE
    // bits 22 and 34, which the hypervisor keeps clear (virt/msr.c).
    xorl %eax, %eax
    cpuid
    cmpl $0x756e6547, %ebx
    jne .Lap_boot_pm32_2
    cmpl $0x49656e69, %edx
    jne .Lap_boot_pm32_2
    cmpl $0x6c65746e, %ecx
    jne .Lap_boot_pm32_2
    movl $0x1a0, %ecx
    rdmsr
    testl $(1 << 22), %eax
    jnz .Lap_boot_pm32_1
    testl $(1 << 2), %edx
    jz .Lap_boot_pm32_2
.Lap_boot_pm32_1:
    andl $~(1 << 22), %eax
    andl $~(1 << 2), %edx
    wrmsr
.Lap_boot_pm32_2:
    movl PARAMS + PARAM_CR4(%ebp), %eax
    movl %eax, %cr4
    movl PARAMS + PARAM_CR3(%ebp), %eax
    movl %eax, %cr3
    movl $0xc0000080, %ecx
    movl PARAMS + PARAM_EFER(%ebp), %eax
    xorl %edx, %edx
    wrmsr
    movl %cr0, %eax
    orl $0x80000001, %eax
    movl %eax, %cr0
    ljmpl *(ap_boot_lm64_ptr - ap_boot_start)(%ebp)

.size ap_boot_pm32, . - ap_boot_pm32
.code64
ap_boot_lm64:
    // The upper half is not defined after the switch.
    movl %ebp, %ebp

    // The APIC ID: leaf 0Bh's x2APIC ID where there is one, else leaf 1's.
    xorl %eax, %eax
    cpuid
    cmpl $0xb, %eax
    jb .Lap_boot_lm64_1
    movl $0xb, %eax
    xorl %ecx, %ecx
    cpuid
    testl %ebx, %ebx
    jz .Lap_boot_lm64_1
    movl %edx, %esi
    jmp .Lap_boot_lm64_2
.Lap_boot_lm64_1:
    movl $1, %eax
    cpuid
    shrl $24, %ebx
    movl %ebx, %esi
.Lap_boot_lm64_2:
    movq PARAMS + PARAM_TABLE(%rbp), %rdi
    movl PARAMS + PARAM_COUNT(%rbp), %ecx
.Lap_boot_lm64_3:
    testl %ecx, %ecx
    jz .Lap_boot_lm64_5
    cmpl %esi, ENTRY_APIC_ID(%rdi)
    je .Lap_boot_lm64_4
    addq $ENTRY_SIZE, %rdi
    decl %ecx
    jmp .Lap_boot_lm64_3
.Lap_boot_lm64_4:
    movq ENTRY_CPU(%rdi), %rdi
    movq %rdi, %rsp
    addq PARAMS + PARAM_STACK_OFFSET(%rbp), %rsp
    // As if called, for the ABI's stack alignment.
    pushq $0
    jmpq *PARAMS + PARAM_ENTRY(%rbp)

// A processor the hypervisor has no area for.
.Lap_boot_lm64_5:
    cli
    hlt
    jmp .Lap_boot_lm64_5
.size ap_boot_lm64, . - ap_boot_lm64

.balign 8
ap_boot_gdt:
.quad 0
.quad 0x00cf9a000000ffff
.quad 0x00cf92000000ffff
.quad 0x00af9a000000ffff
.size ap_boot_gdt, . - ap_boot_gdt
ap_boot_gdtr:
.word 4 * 8 - 1
.long 0
.size ap_boot_gdtr, . - ap_boot_gdtr
ap_boot_pm32_ptr:
.long 0
.word 0x08
.size ap_boot_pm32_ptr, . - ap_boot_pm32_ptr
ap_boot_lm64_ptr:
.long 0
.word 0x18
.size ap_boot_lm64_ptr, . - ap_boot_lm64_ptr
.balign 8
ap_boot_params:
.fill PARAM_SIZE, 1, 0
.size ap_boot_params, . - ap_boot_params
.size ap_boot_start, . - ap_boot_start
ap_boot_end:
.size ap_boot_end, . - ap_boot_end
