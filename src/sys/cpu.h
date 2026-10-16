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

#ifndef SYS__CPU_H
#define SYS__CPU_H

#define GDT_CODE 0x08
#define GDT_DATA 0x10
#define GDT_TSS 0x18

// Each CPU has a naturally aligned area holding its struct cpu, its IST stacks,
// and its stack, so that this_cpu() can find the struct from the stack pointer
// without the help of a segment base, which SVM does not switch.
#define CPU_AREA_SIZE 0x10000
#define CPU_IST_NMI_TOP 0x2000
#define CPU_IST_DF_TOP 0x3000
#define CPU_IST_MC_TOP 0x4000

// The offset of struct cpu's kicked.
#define CPU_KICKED 9

#ifndef __ASSEMBLER__

#include <stdint.h>
#include <stdbool.h>

struct tss64 {
    uint32_t reserved0;
    uint64_t rsp[3];
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed));

struct vcpu;

struct cpu {
    uint32_t index;
    uint32_t apic_id;
    bool bsp;
    // Whether an NMI reached the host since the vCPU loop last looked, which
    // it must look again for before it enters the guest.
    volatile bool kicked;
    uint64_t gdt[5];
    struct tss64 tss;
    struct vcpu *vcpu;
    volatile bool online;
    // Whether a kick's interrupt is on its way, which does for further kicks.
    volatile bool kick;
    volatile bool nmi_pending;
    // An INIT that SVM redirected while the host ran.
    volatile bool init_pending;
    // Whether the host is taking interrupts, as SVM has it do.
    bool taking_interrupts;
};

_Static_assert(sizeof(struct cpu) <= 0x1000, "struct cpu overlaps the IST stacks");

static inline struct cpu *this_cpu(void) {
    uintptr_t rsp;
    asm volatile ("mov %%rsp, %0" : "=r"(rsp));
    return (struct cpu *)(rsp & ~(uintptr_t)(CPU_AREA_SIZE - 1));
}

struct cpu *cpu_area_alloc(uint32_t index, uint32_t apic_id, bool bsp);
void cpu_tables_load(struct cpu *cpu, bool use_ist);
uint64_t cpu_idt_base(void);
uint64_t cpu_stack_top(struct cpu *cpu);

// Runs fn on the given stack, never returning.
__attribute__((noreturn)) void switch_stack(uint64_t stack_top, void (*fn)(void *), void *arg);

// Code an NMI that reaches the host restarts from start, if it interrupts it.
void cpu_nmi_restart(uint64_t start, uint64_t end);

// Where interrupts the host takes go.
void cpu_interrupt_handler(void (*handler)(uint8_t vector));

// MAXPHYADDR.
unsigned cpu_physical_address_bits(void);

#endif

#endif
