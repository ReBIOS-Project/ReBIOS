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
#include <hw/lapic.h>
#include <lib/log.h>
#include <sys/cpu.h>
#include <sys/mm.h>
#include <sys/smp.h>
#include <sys/x86.h>
#include <virt/gpt.h>
#include <virt/insn.h>
#include <virt/vapic.h>
#include <virt/vcpu.h>
#include <virt/virt.h>

#define MSR_X2APIC_ICR 0x830
#define APIC_BASE_ADDRESS 0x000ffffffffff000ull
#define APIC_ICR_LOW 0x300
#define APIC_ICR_HIGH 0x310
#define XAPIC_BROADCAST 0xff
#define X2APIC_BROADCAST 0xffffffffu

static bool trapping;
// The xAPIC page, as the guest and the host both have it.
static uint64_t xapic_page;

void vapic_init(void) {
    if (virt_sipi_in_guest()) {
        return;
    }
    trapping = true;
    xapic_page = rdmsr(MSR_IA32_APIC_BASE) & APIC_BASE_ADDRESS;
    gpt_map(xapic_page, xapic_page, PAGE_SIZE, GPT_R, MT_UC);
    gpt_flush();
    virt_intercept_msr(MSR_X2APIC_ICR, false, true);
}

static void deliver(struct cpu *cpu, uint32_t command) {
    if ((command & ICR_DELIVERY_MASK) == ICR_DELIVERY_STARTUP) {
        // As on a processor, only a vCPU waiting for one takes it.
        int32_t waiting = VCPU_SIPI_WAITING;
        __atomic_compare_exchange_n(&cpu->vcpu->sipi, &waiting,
                                    (int32_t)(command & ICR_VECTOR_MASK), false,
                                    __ATOMIC_RELEASE, __ATOMIC_RELAXED);
        return;
    }
    if (cpu == this_cpu()) {
        vcpu_init_signal(cpu->vcpu);
        return;
    }
    if (__atomic_load_n(&cpu->vcpu->sipi, __ATOMIC_ACQUIRE) == VCPU_SIPI_RUNNING) {
        __atomic_store_n(&cpu->init_pending, true, __ATOMIC_RELEASE);
        vcpu_kick(cpu);
    }
}

// INITs and start-up IPIs go to the vCPUs they name, the sender last, as an
// INIT changes its state.
static void icr_write(struct vcpu *v, uint32_t dest, uint32_t command) {
    uint32_t mode = command & ICR_DELIVERY_MASK;
    if (mode != ICR_DELIVERY_INIT && mode != ICR_DELIVERY_STARTUP) {
        lapic_send_ipi(dest, command);
        return;
    }
    // The INIT level de-assert of old processors does nothing.
    if (mode == ICR_DELIVERY_INIT && !(command & ICR_ASSERT)) {
        return;
    }
    uint32_t shorthand = command & ICR_SHORTHAND_MASK;
    if (shorthand == ICR_SHORTHAND_NONE && (command & ICR_LOGICAL)) {
        printf("An INIT or start-up IPI to logical destination %#x, ignored\n", dest);
        return;
    }

    bool broadcast = dest == (lapic_x2apic() ? X2APIC_BROADCAST : XAPIC_BROADCAST);
    bool to_self = false;
    for (uint32_t i = 0; i < smp_cpu_count(); i++) {
        struct cpu *cpu = smp_cpu(i);
        bool self = cpu == v->cpu;
        bool hit;
        switch (shorthand) {
            case ICR_SHORTHAND_SELF: {
                hit = self;
                break;
            }
            case ICR_SHORTHAND_ALL: {
                hit = true;
                break;
            }
            case ICR_SHORTHAND_OTHERS: {
                hit = !self;
                break;
            }
            default: {
                hit = broadcast || cpu->apic_id == dest;
                break;
            }
        }
        if (!hit || cpu->vcpu == NULL) {
            continue;
        }
        if (self) {
            to_self = true;
        } else {
            deliver(cpu, command);
        }
    }
    if (to_self) {
        deliver(v->cpu, command);
    }
}

bool vapic_mmio_write(struct vcpu *v, uint64_t gpa) {
    if (!trapping || (gpa & ~(uint64_t)(PAGE_SIZE - 1)) != xapic_page) {
        return false;
    }
    struct insn insn;
    if (!insn_decode(v, &insn)) {
        return false;
    }
    struct insn_store store;
    if (!insn_store(v, &insn, &store) || store.size != 4 || (gpa & 3)) {
        printf("A write to local APIC register %#llx that is not emulated, skipped\n",
               (unsigned long long)(gpa - xapic_page));
        vcpu_advance(v, insn.length);
        return true;
    }

    volatile uint32_t *reg = (volatile uint32_t *)(uintptr_t)gpa;
    uint32_t old = store.op == INSN_STORE_MOV ? 0 : *reg;
    uint32_t value = (uint32_t)insn_store_apply(v, &store, old);
    // Before the ICR is written, which may change the vCPU's state.
    vcpu_advance(v, insn.length);

    if ((gpa & (PAGE_SIZE - 1)) == APIC_ICR_LOW) {
        uint32_t dest = *(volatile uint32_t *)(uintptr_t)(xapic_page + APIC_ICR_HIGH) >> 24;
        icr_write(v, dest, value);
    } else {
        *reg = value;
    }
    return true;
}

bool vapic_msr_write(struct vcpu *v, uint32_t msr, uint64_t value) {
    if (!trapping || msr != MSR_X2APIC_ICR) {
        return false;
    }
    vcpu_skip_instruction(v);
    icr_write(v, (uint32_t)(value >> 32), (uint32_t)value);
    return true;
}
