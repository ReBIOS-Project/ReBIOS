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
#include <stddef.h>
#include <stdbool.h>
#include <memory.h>
#include <core.h>
#include <handoff.h>
#include <lib/log.h>
#include <lib/time.h>
#include <sys/cpu.h>
#include <sys/mm.h>
#include <sys/x86.h>
#include <virt/profile.h>
#include <virt/vcpu.h>

#define PERIOD_NS 10000000000ull

// VMX's basic exit reasons are below 0x100, as are SVM's exit codes but its
// nested page faults (0x400 up), which go to slots from 0x100.
#define SLOTS 0x200
#define PAGES 64
#define TOP 8
#define HYPERCALLS 8

// VMX's and SVM's HLT, whose time is the guest's idling, not the hypervisor's.
#define VMX_HLT 12
#define SVM_HLT 0x78

struct counters {
    uint64_t exits[SLOTS];
    uint64_t cycles[SLOTS];
};

struct page_count {
    uint64_t page;
    uint64_t count;
};

bool profile_on;

static struct counters now;
static struct counters before;
static uint32_t *ports;
static uint32_t *ports_before;
static struct page_count pages[PAGES];
static uint64_t hypercalls[HYPERCALLS];
static uint64_t hypercalls_before[HYPERCALLS];
static const char *hypercall_names[HYPERCALLS] = {
    "debug output", "CSM return", "reset", "call32 init", "call32 enter", "call32 return",
};
static uint64_t last_report;

static const char *vmx_names[] = {
    [0] = "exception or NMI", [1] = "interrupt", [2] = "triple fault", [3] = "INIT",
    [4] = "SIPI", [7] = "interrupt window", [8] = "NMI window", [9] = "task switch",
    [10] = "CPUID", [12] = "HLT", [18] = "VMCALL", [28] = "CR access", [30] = "I/O",
    [31] = "RDMSR", [32] = "WRMSR", [37] = "monitor trap", [43] = "TPR below threshold",
    [48] = "EPT violation", [49] = "EPT misconfiguration", [52] = "preemption timer",
    [55] = "XSETBV",
};

static const char *svm_name(unsigned slot) {
    switch (slot) {
        case 0x60: {
            return "interrupt";
        }
        case 0x61: {
            return "NMI";
        }
        case 0x64: {
            return "virtual interrupt";
        }
        case 0x72: {
            return "CPUID";
        }
        case 0x78: {
            return "HLT";
        }
        case 0x7b: {
            return "I/O";
        }
        case 0x7c: {
            return "MSR";
        }
        case 0x81: {
            return "VMMCALL";
        }
        case 0x100: {
            return "nested page fault";
        }
        default: {
            if (slot >= 0x40 && slot < 0x60) {
                return "exception";
            }
            return NULL;
        }
    }
}

static unsigned slot_of(uint32_t reason) {
    if (virt_vmx) {
        return reason & 0xff;
    }
    return reason >= 0x400 ? 0x100 + (reason & 0xff) : reason & 0xff;
}

void profile_init(void) {
    if (!core_handoff->config.profile) {
        return;
    }
    size_t port_pages = (0x10000 * sizeof(uint32_t) + PAGE_SIZE - 1) / PAGE_SIZE;
    ports = page_alloc(port_pages);
    ports_before = page_alloc(port_pages);
    memset(ports, 0, 0x10000 * sizeof(uint32_t));
    memset(ports_before, 0, 0x10000 * sizeof(uint32_t));
    last_report = time_ns();
    profile_on = true;
    printf("Profiling the guest's exits, every %llu s\n",
           (unsigned long long)(PERIOD_NS / 1000000000));
}

void profile_exit(struct vcpu *v, uint32_t reason) {
    if (!profile_on) {
        return;
    }
    v->profile_slot = slot_of(reason);
    v->profile_tsc = rdtsc();
    __atomic_fetch_add(&now.exits[v->profile_slot], 1, __ATOMIC_RELAXED);
}

void profile_hypercall(uint32_t function) {
    if (profile_on && function < HYPERCALLS) {
        __atomic_fetch_add(&hypercalls[function], 1, __ATOMIC_RELAXED);
    }
}

void profile_io(uint16_t port) {
    if (profile_on) {
        __atomic_fetch_add(&ports[port], 1, __ATOMIC_RELAXED);
    }
}

void profile_mmio(uint64_t gpa) {
    if (!profile_on) {
        return;
    }
    uint64_t key = (gpa & ~(uint64_t)(PAGE_SIZE - 1)) + 1;
    for (unsigned i = 0; i < PAGES; i++) {
        struct page_count *p = &pages[(unsigned)((key >> 12) + i) % PAGES];
        uint64_t seen = __atomic_load_n(&p->page, __ATOMIC_RELAXED);
        if (seen == 0) {
            uint64_t expected = 0;
            seen = __atomic_compare_exchange_n(&p->page, &expected, key, false, __ATOMIC_RELAXED,
                                               __ATOMIC_RELAXED) ? key : expected;
        }
        if (seen == key) {
            __atomic_fetch_add(&p->count, 1, __ATOMIC_RELAXED);
            return;
        }
    }
}

static void report(uint64_t elapsed_ns) {
    uint64_t hz = time_tsc_hz();
    uint64_t total = 0;
    uint64_t cycles = 0;
    uint64_t idle = 0;
    unsigned hlt = virt_vmx ? VMX_HLT : SVM_HLT;
    struct {
        unsigned slot;
        uint64_t exits;
        uint64_t cycles;
    } top[TOP] = {0};
    for (unsigned slot = 0; slot < SLOTS; slot++) {
        uint64_t exits = now.exits[slot] - before.exits[slot];
        uint64_t c = now.cycles[slot] - before.cycles[slot];
        total += exits;
        if (slot == hlt) {
            idle += c;
        } else {
            cycles += c;
        }
        for (unsigned i = 0; i < TOP; i++) {
            if (c > top[i].cycles) {
                memmove(&top[i + 1], &top[i], (TOP - 1 - i) * sizeof(top[0]));
                top[i].slot = slot;
                top[i].exits = exits;
                top[i].cycles = c;
                break;
            }
        }
    }
    before = now;
    uint64_t ms = elapsed_ns / 1000000;
    uint64_t period = hz * ms / 1000 + 1;
    printf("Profile: %llu exits/s, %llu.%llu%% of a processor in the hypervisor, "
           "%llu.%llu%% halted\n",
           (unsigned long long)(total * 1000 / (ms ? ms : 1)),
           (unsigned long long)(cycles * 100 / period), (unsigned long long)(cycles * 1000 / period % 10),
           (unsigned long long)(idle * 100 / period), (unsigned long long)(idle * 1000 / period % 10));
    for (unsigned i = 0; i < TOP && top[i].exits != 0; i++) {
        const char *name = virt_vmx ? (top[i].slot < sizeof(vmx_names) / sizeof(vmx_names[0])
                                       ? vmx_names[top[i].slot] : NULL)
                                    : svm_name(top[i].slot);
        printf("Profile:   %-20s %#5x %8llu/s %6llu us each\n", name != NULL ? name : "?",
               top[i].slot, (unsigned long long)(top[i].exits * 1000 / (ms ? ms : 1)),
               (unsigned long long)(top[i].cycles * 1000000 / hz / top[i].exits));
    }
    for (unsigned i = 0; i < HYPERCALLS; i++) {
        uint64_t count = hypercalls[i] - hypercalls_before[i];
        hypercalls_before[i] = hypercalls[i];
        if (count != 0) {
            printf("Profile:   hypercall %-14s %8llu/s\n",
                   hypercall_names[i] != NULL ? hypercall_names[i] : "?",
                   (unsigned long long)count * 1000 / (ms ? ms : 1));
        }
    }
    for (unsigned n = 0; n < TOP; n++) {
        unsigned best = 0;
        uint32_t most = 0;
        for (unsigned port = 0; port < 0x10000; port++) {
            uint32_t count = ports[port] - ports_before[port];
            if (count > most) {
                most = count;
                best = port;
            }
        }
        if (most == 0) {
            break;
        }
        printf("Profile:   port %#06x %8llu/s\n", best, (unsigned long long)most * 1000 / (ms ? ms : 1));
        ports_before[best] = ports[best];
    }
    memcpy(ports_before, ports, 0x10000 * sizeof(uint32_t));
    for (unsigned i = 0; i < PAGES; i++) {
        uint64_t count = __atomic_exchange_n(&pages[i].count, 0, __ATOMIC_RELAXED);
        if (count * 1000 / (ms ? ms : 1) >= 100) {
            printf("Profile:   page %#llx %8llu/s\n", (unsigned long long)(pages[i].page - 1),
                   (unsigned long long)count * 1000 / (ms ? ms : 1));
        }
    }
}

void profile_entry(struct vcpu *v) {
    if (!profile_on) {
        return;
    }
    if (v->profile_tsc != 0) {
        __atomic_fetch_add(&now.cycles[v->profile_slot], rdtsc() - v->profile_tsc, __ATOMIC_RELAXED);
        v->profile_tsc = 0;
    }
    if (!v->cpu->bsp) {
        return;
    }
    uint64_t t = time_ns();
    if (t - last_report >= PERIOD_NS) {
        report(t - last_report);
        last_report = t;
    }
}
