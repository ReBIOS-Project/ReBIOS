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

#define CPU_GDT_CODE 0x08
#define CPU_GDT_DATA 0x10
#define CPU_GDT_TSS 0x18

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

// The offsets of struct cpu_resume's fields.
#define CPU_RESUME_RBX 0
#define CPU_RESUME_RBP 8
#define CPU_RESUME_R12 16
#define CPU_RESUME_R13 24
#define CPU_RESUME_R14 32
#define CPU_RESUME_R15 40
#define CPU_RESUME_RSP 48
#define CPU_RESUME_RIP 56
#define CPU_RESUME_RFLAGS 64

#ifndef __ASSEMBLER__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdnoreturn.h>

// Where a caller of cpu_resume_save() goes on: the registers its callers keep,
// its stack, and its return address.
struct cpu_resume {
    uint64_t rbx, rbp, r12, r13, r14, r15;
    uint64_t rsp, rip, rflags;
};

_Static_assert(
    offsetof(struct cpu_resume, rflags) == CPU_RESUME_RFLAGS,
    "The CPU_RESUME_* offsets that cpu.S uses are out of date."
);

// Returns 0, and 1 again once an exception the host recovers from resumes
// there, as long as the caller has not returned.
__attribute__((returns_twice)) int cpu_resume_save(struct cpu_resume *resume);

struct cpu_tss {
    uint32_t reserved0;
    uint64_t rsp[3];
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed));

struct task;
struct vcpu;
struct vga_draw_ops;
struct vga_scratch;

struct cpu {
    // The CPU itself, where another stack's area has the CPU running on it.
    struct cpu *self;
    uint32_t index;
    uint32_t apic_id;
    bool bsp;
    // Whether an NMI reached the host since the vCPU loop last looked, which
    // it must look again for before it enters the guest.
    _Atomic(bool) kicked;
    uint64_t gdt[5];
    struct cpu_tss tss;
    struct vcpu *vcpu;
    _Atomic(bool) online;
    // Whether a kick's interrupt is on its way, which does for further kicks.
    _Atomic(bool) kick;
    _Atomic(bool) nmi_pending;
    // An INIT that SVM redirected while the host ran.
    _Atomic(bool) init_pending;
    // The IA32_CLOCK_MODULATION that a thermal zone's passive cooling wants,
    // which the processor writes before it next enters the guest, and the one
    // it last wrote.
    _Atomic(uint64_t) clock_modulation;
    uint64_t clock_modulation_set;
    // Whether the host is taking interrupts, as SVM has it do.
    _Atomic(bool) taking_interrupts;
    // IA32_APIC_BASE, once lapic_host_init() set it for good, else 0.
    uint64_t apic_base;
    // The task the CPU runs, while it runs one.
    struct task *task;
    // The time slice of the guest's exit the CPU handles: when the exit came,
    // and when what can wait for the next one gives the CPU back.
    uint64_t slice_start;
    uint64_t slice_start_tsc;
    uint64_t slice_end;
    // By the TSC, when the CPU last went into the guest, and the shortest it
    // has taken to come back, or 0.
    uint64_t entry_tsc;
    uint64_t shortest_exit;
    // When the watchdog may report the CPU again.
    uint64_t watchdog_next;
    // During a call to the firmware's runtime services, where a page fault in
    // them resumes, else NULL, and the address the last such fault was at.
    struct cpu_resume *firmware_resume;
    uint64_t firmware_fault;
    // Where the guest's SIMD registers wait while the hypervisor uses them
    // (sys/simd.c), which state components XSAVE saved there, none where
    // FXSAVE did, and the guest's XCR0.
    uint8_t *simd_area;
    uint64_t simd_saved;
    uint64_t simd_xcr0;
    // The builds of the emulated VGA's drawing kernels that the processor
    // found fastest, the SIMD level they need, and how long its fastest copy
    // to the framebuffer took (virt/vga_draw.c).
    const struct vga_draw_ops *draw_ops;
    size_t draw_level;
    uint64_t draw_copy_ticks;
    // What the processor draws the emulated VGA with (virt/vga.c).
    struct vga_scratch *vga_scratch;
    // Whether its vCPU waits rather than runs the guest: halted, or not
    // started.
    _Atomic(bool) idle;
    // Whether it got itself ready to run the guest, which the boot processor
    // waits for before it starts the next, so that what each times runs alone.
    _Atomic(bool) ready;
};

_Static_assert(sizeof(struct cpu) <= 0x1000, "The IST stacks overlap struct cpu.");

static inline struct cpu *this_cpu(void) {
    uintptr_t rsp;
    asm volatile ("movq %%rsp, %0" : "=r"(rsp));
    return ((struct cpu *)(rsp & ~(uintptr_t)(CPU_AREA_SIZE - 1)))->self;
}

// Starts this CPU's time slice, at an exit; whether it is over.
void cpu_slice_start(void);
bool cpu_slice_over(void);
// Ends it, on the way into the guest.
void cpu_slice_end(void);
// At an exit: starts the slice, and notes how long the trip took.
void cpu_guest_exited(void);
// Whether exits take this CPU far longer than a processor running the
// hypervisor itself would take, as they do under another hypervisor.
bool cpu_exits_slow(void);

struct cpu *cpu_area_alloc(size_t index, uint32_t apic_id, bool bsp);
void cpu_tables_load(struct cpu *cpu, bool use_ist);
uintptr_t cpu_idt_base(void);
uintptr_t cpu_stack_top(struct cpu *cpu);

// Runs fn on the given stack, never returning.
noreturn void cpu_switch_stack(uintptr_t stack_top, void (*fn)(void *), void *arg);

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

#endif // !__ASSEMBLER__

#endif
