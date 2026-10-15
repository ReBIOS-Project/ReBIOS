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
#include <lib/log.h>
#include <sys/cpu.h>
#include <sys/fixup.h>
#include <sys/mm.h>
#include <sys/x86.h>

struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t ist;
    uint8_t type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
} __attribute__((packed));

struct table_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

struct exception_frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector, error_code;
    uint64_t rip, cs, rflags, rsp, ss;
};

// Offsets of the stubs from the table.
extern const int32_t exception_stubs[32];

static struct idt_entry idt[256];
static bool idt_built;

static const char *exception_names[32] = {
    "#DE", "#DB", "NMI", "#BP", "#OF", "#BR", "#UD", "#NM",
    "#DF", "#MF(9)", "#TS", "#NP", "#SS", "#GP", "#PF", "(15)",
    "#MF", "#AC", "#MC", "#XM", "#VE", "#CP", "(22)", "(23)",
    "(24)", "(25)", "(26)", "(27)", "#HV", "#VC", "#SX", "(31)",
};

void exception_handler(struct exception_frame *frame);

struct fixup {
    int32_t insn;
    int32_t resume;
};

extern const struct fixup __start_fixup_table[];
extern const struct fixup __stop_fixup_table[];

uint64_t fixup_find(uint64_t rip) {
    for (const struct fixup *f = __start_fixup_table; f < __stop_fixup_table; f++) {
        if ((uint64_t)(uintptr_t)&f->insn + f->insn == rip) {
            return (uint64_t)(uintptr_t)&f->resume + f->resume;
        }
    }
    return 0;
}

void exception_handler(struct exception_frame *frame) {
    if (frame->vector == 2) {
        // An NMI meant for the guest, which arrived while the host ran.
        this_cpu()->nmi_pending = true;
        return;
    }

    if (frame->vector == 13 || frame->vector == 6) {
        uint64_t resume = fixup_find(frame->rip);
        if (resume != 0) {
            frame->rip = resume;
            return;
        }
    }

    printf("\nHost exception %s (vector %llu), error code %#llx\n",
           frame->vector < 32 ? exception_names[frame->vector] : "?",
           (unsigned long long)frame->vector, (unsigned long long)frame->error_code);
    printf("RIP %#llx CS %#llx RFLAGS %#llx RSP %#llx SS %#llx CR2 %#llx\n",
           (unsigned long long)frame->rip, (unsigned long long)frame->cs,
           (unsigned long long)frame->rflags, (unsigned long long)frame->rsp,
           (unsigned long long)frame->ss, (unsigned long long)read_cr2());
    printf("RAX %#llx RBX %#llx RCX %#llx RDX %#llx\n",
           (unsigned long long)frame->rax, (unsigned long long)frame->rbx,
           (unsigned long long)frame->rcx, (unsigned long long)frame->rdx);
    printf("RSI %#llx RDI %#llx RBP %#llx R8 %#llx\n",
           (unsigned long long)frame->rsi, (unsigned long long)frame->rdi,
           (unsigned long long)frame->rbp, (unsigned long long)frame->r8);
    panic("Unrecoverable exception in the hypervisor\n");
}

static void idt_set(int vector, uint64_t handler, uint8_t ist) {
    idt[vector].offset_low = handler & 0xffff;
    idt[vector].selector = GDT_CODE;
    idt[vector].ist = ist;
    // Present, DPL 0, 64-bit interrupt gate.
    idt[vector].type_attr = 0x8e;
    idt[vector].offset_mid = (handler >> 16) & 0xffff;
    idt[vector].offset_high = handler >> 32;
    idt[vector].reserved = 0;
}

static void idt_build(bool use_ist) {
    for (int i = 0; i < 32; i++) {
        uint8_t ist = 0;
        if (use_ist) {
            if (i == 2) {
                ist = 1;
            } else if (i == 8) {
                ist = 2;
            } else if (i == 18) {
                ist = 3;
            }
        }
        idt_set(i, (uint64_t)(uintptr_t)exception_stubs + exception_stubs[i], ist);
    }
    idt_built = true;
}

uint64_t cpu_idt_base(void) {
    return (uint64_t)(uintptr_t)idt;
}

uint64_t cpu_stack_top(struct cpu *cpu) {
    return (uint64_t)(uintptr_t)cpu + CPU_AREA_SIZE;
}

struct cpu *cpu_area_alloc(uint32_t index, uint32_t apic_id, bool bsp) {
    struct cpu *cpu = page_alloc_aligned(CPU_AREA_SIZE / PAGE_SIZE, CPU_AREA_SIZE);
    uint64_t base = (uint64_t)(uintptr_t)cpu;

    cpu->index = index;
    cpu->apic_id = apic_id;
    cpu->bsp = bsp;

    cpu->gdt[0] = 0;
    cpu->gdt[1] = 0x00af9b000000ffff;
    cpu->gdt[2] = 0x00cf93000000ffff;

    uint64_t tss = (uint64_t)(uintptr_t)&cpu->tss;
    uint64_t limit = sizeof(cpu->tss) - 1;
    // Present, DPL 0, available 64-bit TSS.
    cpu->gdt[3] = (limit & 0xffff) | ((tss & 0xffffff) << 16) | (0x89ull << 40)
                  | ((limit & 0xf0000) << 32) | ((tss >> 24 & 0xff) << 56);
    cpu->gdt[4] = tss >> 32;

    cpu->tss.ist[0] = base + CPU_IST_NMI_TOP;
    cpu->tss.ist[1] = base + CPU_IST_DF_TOP;
    cpu->tss.ist[2] = base + CPU_IST_MC_TOP;
    cpu->tss.iomap_base = sizeof(cpu->tss);

    return cpu;
}

void cpu_tables_load(struct cpu *cpu, bool use_ist) {
    if (!idt_built) {
        idt_build(use_ist);
    }

    struct table_ptr gdtr = {sizeof(cpu->gdt) - 1, (uint64_t)(uintptr_t)cpu->gdt};
    struct table_ptr idtr = {sizeof(idt) - 1, (uint64_t)(uintptr_t)idt};

    asm volatile (
        "lgdt %0\n\t"
        "lidt %1\n\t"
        "pushq %2\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n"
        "1:\n\t"
        "movl %3, %%eax\n\t"
        "movl %%eax, %%ds\n\t"
        "movl %%eax, %%es\n\t"
        "movl %%eax, %%ss\n\t"
        "xorl %%eax, %%eax\n\t"
        "movl %%eax, %%fs\n\t"
        "movl %%eax, %%gs\n\t"
        :
        : "m"(gdtr), "m"(idtr), "i"(GDT_CODE), "i"(GDT_DATA)
        : "rax", "memory");

    // Busy TSS descriptors cannot be loaded, and this one may be reloaded.
    cpu->gdt[3] &= ~(0x2ull << 40);
    asm volatile ("ltr %w0" :: "r"(GDT_TSS) : "memory");
}
