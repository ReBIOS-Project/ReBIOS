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

#ifndef SYS__CPU_H
#define SYS__CPU_H

#define GDT_CODE 0x08
#define GDT_DATA 0x10
#define GDT_TSS 0x18

// Each CPU has a naturally aligned area holding its struct cpu, its IST
// stacks, and its stack, so that this_cpu() can find the struct from the stack
// pointer without the help of a segment base, which SVM does not switch. Other
// stacks are areas of the same size, which begin with the CPU running on them.
#define CPU_AREA_SIZE 0x10000
#define CPU_IST_NMI_TOP 0x2000
#define CPU_IST_DF_TOP 0x3000
#define CPU_IST_MC_TOP 0x4000

// The offset of struct cpu's kicked.
#define CPU_KICKED 17

#ifndef __ASSEMBLER__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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
struct task;

struct cpu {
    // The CPU itself, where another stack's area has the CPU running on it.
    struct cpu *self;
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
    // IA32_APIC_BASE, once lapic_host_mode() set it for good, else 0.
    uint64_t apic_base;
    // The task the CPU runs, while it runs one.
    struct task *task;
    // The time slice of the guest's exit the CPU handles: when the exit came,
    // and when what can wait for the next one gives the CPU back.
    uint64_t slice_start;
    uint64_t slice_start_tsc;
    // By the TSC, how long this CPU has kept the guest waiting at exits.
    uint64_t root_tsc;
    uint64_t slice_end;
    // When the watchdog may report the CPU again.
    uint64_t watchdog_next;
};

_Static_assert(
    sizeof(struct cpu) <= 0x1000, "struct cpu overlaps the IST stacks"
);

static inline struct cpu *this_cpu(void) {
    uintptr_t rsp;
    asm volatile ("mov %%rsp, %0" : "=r"(rsp));
    return ((struct cpu *)(rsp & ~(uintptr_t)(CPU_AREA_SIZE - 1)))->self;
}

// Starts this CPU's time slice, at an exit; whether it is over.
void cpu_slice_start(void);
bool cpu_slice_over(void);
// Ends it, on the way into the guest.
void cpu_slice_end(void);
// How long this CPU has kept the guest waiting so far, by the TSC.
uint64_t cpu_root_time(void);

struct cpu *cpu_area_alloc(size_t index, uint32_t apic_id, bool bsp);
void cpu_tables_load(struct cpu *cpu, bool use_ist);
uintptr_t cpu_idt_base(void);
uintptr_t cpu_stack_top(struct cpu *cpu);

// Runs fn on the given stack, never returning.
__attribute__((noreturn)) void switch_stack(
    uintptr_t stack_top, void (*fn)(void *), void *arg
);

// Code an NMI that reaches the host restarts from start, if it interrupts it.
void cpu_nmi_restart(uintptr_t start, uintptr_t end);

// Where interrupts the host takes go.
void cpu_interrupt_handler(void (*handler)(uint8_t vector));

// What writes the line on the guest that host exception panics end with.
void cpu_guest_describer(size_t (*describe)(char *buf, size_t size));

// Halts until an interrupt or NMI, with interrupts enabled meanwhile, unless
// an NMI arrived since kicked was cleared.
void cpu_halt(void);

// MAXPHYADDR.
unsigned int cpu_physical_address_bits(void);
// How far an APIC ID shifts right to its package's ID.
unsigned int cpu_package_shift(void);

#endif

#endif
