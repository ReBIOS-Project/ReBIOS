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

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <efi.h>
#include <sys/cpu.h>
#include <core.h>
#include <handoff.h>
#include <lib/log.h>
#include <lib/printf.h>
#include <lib/time.h>
#include <memory.h>
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
extern const uint8_t interrupt_stubs[];

#define INTERRUPT_STUB_SIZE 16

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

uintptr_t fixup_find(uintptr_t rip) {
    for (const struct fixup *f = __start_fixup_table; f < __stop_fixup_table;
         f++) {
        if ((uintptr_t)&f->insn + f->insn == rip) {
            return (uintptr_t)&f->resume + f->resume;
        }
    }
    return 0;
}

_Static_assert(
    offsetof(struct cpu, kicked) == CPU_KICKED, "CPU_KICKED is out of date"
);

// The longest an exit keeps the CPU from the guest for what can wait for its
// next exits, so that the guest's interrupts come on time.
#define CPU_SLICE_NS ((uint32_t)1000000)

static uintptr_t nmi_restart_start;
static uintptr_t nmi_restart_end;
static void (*interrupt_handler)(uint8_t vector);
static size_t (*guest_describer)(char *buf, size_t size);

void cpu_interrupt_handler(void (*handler)(uint8_t vector)) {
    interrupt_handler = handler;
}

void cpu_guest_describer(size_t (*describe)(char *buf, size_t size)) {
    guest_describer = describe;
}

// The x2APIC ID, else the initial APIC ID: by CPUID alone, for panics.
static uint32_t apic_id_now(void) {
    if (cpuid(0, 0).eax >= 0xb && (cpuid(0xb, 0).ebx & 0xffff) != 0) {
        return cpuid(0xb, 0).edx;
    }
    return cpuid(1, 0).ebx >> 24;
}

__attribute__((format(printf, 4, 5)))
static void append(char *buf, size_t size, size_t *len, const char *fmt, ...) {
    if (*len + 1 >= size) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf + *len, size - *len, fmt, args);
    va_end(args);
    if (n > 0) {
        *len += (size_t)n < size - *len ? (size_t)n : size - *len - 1;
    }
}

unsigned int cpu_physical_address_bits(void) {
    return cpuid(0x80000008, 0).eax & 0xff;
}

unsigned int cpu_package_shift(void) {
    // Leaf 1Fh's domains, else leaf 0Bh's, each there where its first subleaf
    // counts processors, the last one's shift reaching the package (SDM
    // 325462-093 Vol. 1, Tables 21-31 and 21-70).
    uint32_t max = cpuid(0, 0).eax;
    static const uint32_t leaves[] = { 0x1f, 0xb };
    for (size_t i = 0; i < sizeof(leaves) / sizeof(uint32_t); i++) {
        if (max < leaves[i] || (cpuid(leaves[i], 0).ebx & 0xffff) == 0) {
            continue;
        }
        unsigned int shift = 0;
        for (size_t subleaf = 0; ; subleaf++) {
            struct cpuid_regs r = cpuid(leaves[i], subleaf);
            if ((r.ebx & 0xffff) == 0) {
                return shift;
            }
            shift = r.eax & 0x1f;
        }
    }
    // Leaf 1's count of IDs in the package, a power of two.
    uint32_t ids = (cpuid(1, 0).ebx >> 16) & 0xff;
    unsigned int shift = 0;
    while (((uint32_t)1 << shift) < ids) {
        shift++;
    }
    return shift;
}

void cpu_nmi_restart(uintptr_t start, uintptr_t end) {
    nmi_restart_start = start;
    nmi_restart_end = end;
}

extern const char cpu_halt_restart_end[];

void exception_handler(struct exception_frame *frame) {
    if (frame->vector == 2) {
        struct cpu *cpu = this_cpu();
        // The guest's, which arrived while the host ran.
        cpu->nmi_pending = true;
        cpu->kicked = true;
        if (frame->rip >= nmi_restart_start && frame->rip < nmi_restart_end) {
            frame->rip = nmi_restart_start;
        }
        uintptr_t halt = (uintptr_t)cpu_halt;
        if (frame->rip >= halt
         && frame->rip < (uintptr_t)cpu_halt_restart_end) {
            frame->rip = halt;
        }
        return;
    }
    if (frame->vector == 30) {
        // An INIT that SVM redirected, meant for the guest.
        this_cpu()->init_pending = true;
        return;
    }
    // Vectors 16 to 31 are interrupts only where the host takes interrupts.
    if (frame->vector >= 32
     || (frame->vector >= 16 && frame->vector != 18
      && this_cpu()->taking_interrupts)) {
        if (interrupt_handler == NULL) {
            panic(
                "Host interrupt %llu with no handler\n",
                (unsigned long long)frame->vector
            );
        }
        interrupt_handler((uint8_t)frame->vector);
        return;
    }

    if (frame->vector == 13 || frame->vector == 6) {
        uintptr_t resume = fixup_find(frame->rip);
        if (resume != 0) {
            frame->rip = resume;
            return;
        }
    }

    // One panic(), as the processor may hold the log's lock. The stack and the
    // CPU area it leads to may be anything, so neither is read unchecked.
    char text[1536];
    size_t len = 0;
    uint32_t apic_id = apic_id_now();
    uint64_t image = (uintptr_t)EfiLoadSlide;
    uintptr_t image_end = image + EfiImageSize;
    uintptr_t map_top = core_handoff != NULL ? core_handoff->map_top : 0;

    append(
        text, sizeof(text), &len,
        "Host exception %s (vector %llu), error code %#llx, APIC ID %#x\n",
        frame->vector < 32 ? exception_names[frame->vector] : "?",
        (unsigned long long)frame->vector,
        (unsigned long long)frame->error_code, apic_id
    );
    if (frame->rip >= image && frame->rip < image_end) {
        append(
            text, sizeof(text), &len, "RIP %#llx (ReBIOS + %#llx)",
            (unsigned long long)frame->rip,
            (unsigned long long)(frame->rip - image)
        );
    } else {
        append(
            text, sizeof(text), &len, "RIP %#llx (outside ReBIOS at %#llx)",
            (unsigned long long)frame->rip, (unsigned long long)image
        );
    }
    append(
        text, sizeof(text), &len,
        " CS %#llx RFLAGS %#llx RSP %#llx SS %#llx\n"
        "CR0 %#llx CR2 %#llx CR3 %#llx CR4 %#llx DR6 %#llx DR7 %#llx\n"
        "RAX %#llx RBX %#llx RCX %#llx RDX %#llx\n"
        "RSI %#llx RDI %#llx RBP %#llx R8 %#llx\n"
        "R9 %#llx R10 %#llx R11 %#llx R12 %#llx\n"
        "R13 %#llx R14 %#llx R15 %#llx\n",
        (unsigned long long)frame->cs, (unsigned long long)frame->rflags,
        (unsigned long long)frame->rsp, (unsigned long long)frame->ss,
        (unsigned long long)read_cr0(), (unsigned long long)read_cr2(),
        (unsigned long long)read_cr3(), (unsigned long long)read_cr4(),
        (unsigned long long)read_dr6(), (unsigned long long)read_dr7(),
        (unsigned long long)frame->rax, (unsigned long long)frame->rbx,
        (unsigned long long)frame->rcx, (unsigned long long)frame->rdx,
        (unsigned long long)frame->rsi, (unsigned long long)frame->rdi,
        (unsigned long long)frame->rbp, (unsigned long long)frame->r8,
        (unsigned long long)frame->r9, (unsigned long long)frame->r10,
        (unsigned long long)frame->r11, (unsigned long long)frame->r12,
        (unsigned long long)frame->r13, (unsigned long long)frame->r14,
        (unsigned long long)frame->r15
    );
    if (frame->rsp % 8 == 0 && frame->rsp >= 4096
     && frame->rsp + 64 <= map_top) {
        const uint64_t *stack = (const uint64_t *)(uintptr_t)frame->rsp;
        append(
            text, sizeof(text), &len,
            "Stack %#llx %#llx %#llx %#llx %#llx %#llx %#llx %#llx\n",
            (unsigned long long)stack[0], (unsigned long long)stack[1],
            (unsigned long long)stack[2], (unsigned long long)stack[3],
            (unsigned long long)stack[4], (unsigned long long)stack[5],
            (unsigned long long)stack[6], (unsigned long long)stack[7]
        );
    }
    struct cpu *cpu = this_cpu();
    if (guest_describer != NULL
     && (uintptr_t)cpu + sizeof(struct cpu) <= map_top
     && cpu->apic_id == apic_id && len + 1 < sizeof(text)) {
        size_t n = guest_describer(text + len, sizeof(text) - len);
        len += n < sizeof(text) - len ? n : sizeof(text) - len - 1;
    }
    panic("%s", text);
}

static void idt_set(int vector, uintptr_t handler, uint8_t ist) {
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
    for (size_t i = 0; i < 32; i++) {
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
        idt_set(i, (uintptr_t)exception_stubs + exception_stubs[i], ist);
    }
    for (size_t i = 32; i < 256; i++) {
        idt_set(
            i, (uintptr_t)interrupt_stubs + (i - 32) * INTERRUPT_STUB_SIZE, 0
        );
    }
    idt_built = true;
}

uintptr_t cpu_idt_base(void) {
    return (uintptr_t)idt;
}

uintptr_t cpu_stack_top(struct cpu *cpu) {
    return (uintptr_t)cpu + CPU_AREA_SIZE;
}

void cpu_slice_start(void) {
    struct cpu *cpu = this_cpu();
    cpu->slice_start = time_ns();
    cpu->slice_start_tsc = rdtsc();
    cpu->slice_end = cpu->slice_start + CPU_SLICE_NS;
}

void cpu_slice_end(void) {
    struct cpu *cpu = this_cpu();
    uint64_t now = rdtsc();
    cpu->root_tsc += now - cpu->slice_start_tsc;
    cpu->slice_start_tsc = now;
}

uint64_t cpu_root_time(void) {
    struct cpu *cpu = this_cpu();
    return cpu->root_tsc + (rdtsc() - cpu->slice_start_tsc);
}

bool cpu_slice_over(void) {
    return time_ns() >= this_cpu()->slice_end;
}

struct cpu *cpu_area_alloc(size_t index, uint32_t apic_id, bool bsp) {
    struct cpu *cpu = page_alloc_aligned(
        CPU_AREA_SIZE / PAGE_SIZE, CPU_AREA_SIZE
    );
    uintptr_t base = (uintptr_t)cpu;

    cpu->self = cpu;
    cpu->index = index;
    cpu->apic_id = apic_id;
    cpu->bsp = bsp;

    cpu->gdt[0] = 0;
    cpu->gdt[1] = 0x00af9b000000ffff;
    cpu->gdt[2] = 0x00cf93000000ffff;

    uintptr_t tss = (uintptr_t)&cpu->tss;
    uint64_t limit = sizeof(struct tss64) - 1;
    // Present, DPL 0, available 64-bit TSS.
    cpu->gdt[3] = (limit & 0xffff) | ((tss & 0xffffff) << 16)
        | ((uint64_t)0x89 << 40) | ((limit & 0xf0000) << 32)
        | ((tss >> 24 & 0xff) << 56);
    cpu->gdt[4] = tss >> 32;

    cpu->tss.ist[0] = base + CPU_IST_NMI_TOP;
    cpu->tss.ist[1] = base + CPU_IST_DF_TOP;
    cpu->tss.ist[2] = base + CPU_IST_MC_TOP;
    cpu->tss.iomap_base = sizeof(struct tss64);

    return cpu;
}

void cpu_tables_load(struct cpu *cpu, bool use_ist) {
    if (!idt_built) {
        idt_build(use_ist);
    }

    struct table_ptr gdtr = {
        sizeof(cpu->gdt) - 1, (uint64_t)(uintptr_t)cpu->gdt
    };
    struct table_ptr idtr = { sizeof(idt) - 1, (uint64_t)(uintptr_t)idt };

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
    cpu->gdt[3] &= ~((uint64_t)0x2 << 40);
    asm volatile ("ltr %w0" :: "r"(GDT_TSS) : "memory");
}
