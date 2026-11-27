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
// You should have received a copy of the GNU General Public License along with
// ReBIOS. If not, see <https://www.gnu.org/licenses/>.

#include <stdalign.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdnoreturn.h>
#include <uacpi/namespace.h>
#include <uacpi/sleep.h>
#include <uacpi/status.h>
#include <virt/sleep.h>
#include <core.h>
#include <hw/acpi.h>
#include <hw/ioapic.h>
#include <hw/lapic.h>
#include <hw/lapic_legacy.h>
#include <iommu/dma.h>
#include <lib/log.h>
#include <lib/time.h>
#include <lib/util.h>
#include <sys/cpu.h>
#include <sys/smp.h>
#include <sys/x86.h>
#include <virt/a20.h>
#include <virt/hpet.h>
#include <virt/irq.h>
#include <virt/isa_dma.h>
#include <virt/kbc.h>
#include <virt/pci_virt.h>
#include <virt/pit.h>
#include <virt/rtc.h>
#include <virt/usb.h>
#include <virt/vacpi.h>
#include <virt/vcpu.h>
#include <virt/vga.h>
#include <virt/vioapic.h>
#include <virt/virt.h>
#include <virt/vlapic.h>

// How long the boot processor waits for the others to stop.
#define PARK_TIMEOUT_NS ((uint64_t)1000000000)
// The boot processor's stack while the firmware's wake brings it back, apart
// from its own, whose top holds the frames that it returns through.
#define WAKE_STACK_SIZE ((size_t)0x4000)

static _Atomic(uint8_t) requested;
static _Atomic(uint32_t) parked;
static struct cpu_resume resume;
static alignas(16) uint8_t wake_stack[WAKE_STACK_SIZE];
// What the last sleep took from the TSC, which each processor gets back as it
// wakes; 0 where the machine did not sleep.
static uint64_t tsc_lost;
// How far the TSC is ahead of the count the processors restarted at the last
// waking.
static uint64_t tsc_moved;

bool sleep_offered(uint8_t state) {
    if (state != SLEEP_S3 || !acpi_init()) {
        return false;
    }
    uacpi_namespace_node *node;
    if (uacpi_namespace_node_find(NULL, "\\_S3_", &node) != UACPI_STATUS_OK) {
        return false;
    }
    // The firmware's waking vector is the start trampoline's page.
    if (core_handoff->ap_boot == 0) {
        return false;
    }
    // Nothing lights the screen of an emulated VGA again after S3, so the
    // guest does without S3 there: an exception to hiding nothing from the
    // guest for want of something in ReBIOS.
    return !vga_emulated();
}

void sleep_request(uint8_t state) {
    uint8_t none = 0;
    bool first = atomic_compare_exchange_strong_explicit(
        &requested, &none, state, memory_order_acq_rel, memory_order_acquire
    );
    if (!first) {
        return;
    }
    // Those that wait for a start-up IPI too, which vcpu_kick() leaves.
    for (size_t i = 0; i < smp_cpu_count(); i++) {
        vcpu_wake(smp_cpu(i));
    }
}

bool sleep_requested(void) {
    return atomic_load_explicit(&requested, memory_order_acquire) != 0;
}

// An application processor's last steps before the machine sleeps: it leaves
// VMX operation, as Xen 4.22's enter_state() has every processor do, and stops
// with interrupts off, until waking restarts it.
static noreturn void park(struct vcpu *v) {
    cli();
    virt_cpu_off();
    wbinvd();
    atomic_store_explicit(&v->parked, true, memory_order_relaxed);
    atomic_fetch_add_explicit(&parked, 1, memory_order_release);
    x86_halt_forever();
}

// Where the firmware's waking vector brings the boot processor, through the
// start trampoline, on wake_stack.
static noreturn void woken(void *arg) {
    (void)arg;
    // Interrupts stay off until the hypervisor's IDT is back.
    resume.rflags &= ~(uint64_t)RFLAGS_IF;
    cpu_resume_jump(&resume);
}

// The TSC as it was before the machine slept, plus the time that has gone by
// since waking: by IA32_TSC_ADJUST where the processor has it, which every
// processor gets the same, else by writing the TSC (SDM 325462-093 Vol. 3B,
// 20.17.3). The hypervisor's deadlines are TSC values, which go on holding.
static void tsc_restore(void) {
    if (tsc_lost == 0) {
        return;
    }
    if (cpuid(0, 0).eax >= 7 && (cpuid(7, 0).ebx & CPUID_7_EBX_TSC_ADJUST) != 0) {
        wrmsr(MSR_IA32_TSC_ADJUST, rdmsr(MSR_IA32_TSC_ADJUST) + tsc_lost);
    } else {
        wrmsr(MSR_IA32_TSC, rdtsc() + tsc_lost);
    }
}

// In the order of the processors' start: the local APIC as a legacy BIOS hands
// it over, which the hypervisor's own setup then changes.
void sleep_cpu_wake(struct cpu *cpu) {
    tsc_restore();
    cpu_tables_load(cpu, virt_vmx);
    // The local APIC is in whatever mode waking left it in.
    cpu->apic_base = 0;
    lapic_legacy_init();
    if (smp_long_apic_ids()) {
        lapic_host_long_ids();
    }
    lapic_host_init(VLAPIC_SPURIOUS_VECTOR, VLAPIC_TIMER_VECTOR);
    virt_cpu_init();
}

uint64_t sleep_tsc_moved(void) {
    return tsc_moved;
}

static bool others_parked(size_t expected) {
    uint64_t deadline = time_ns() + PARK_TIMEOUT_NS;
    while (atomic_load_explicit(&parked, memory_order_acquire) < expected) {
        if (time_ns() > deadline) {
            return false;
        }
        cpu_relax();
    }
    return true;
}

// The guest's machine as it wakes from S3: what its emulated chipset keeps,
// and the boot processor at its reset vector, where its BIOS finds that it is
// resuming and goes on to the OS's waking vector. The others wait for a
// start-up IPI, as smp_restart_aps() leaves them.
static void guest_wake(struct vcpu *v) {
    vacpi_wake();
    rtc_wake();
    vioapic_wake();
    irq_wake();
    pit_wake();
    isa_dma_wake();
    a20_wake();
    vcpu_power_on(v);
}

// Each processor that has not stopped for the machine to sleep, with the exit
// it last took.
static void name_running(const struct vcpu *boot) {
    for (size_t i = 0; i < smp_cpu_count(); i++) {
        struct cpu *cpu = smp_cpu(i);
        const struct vcpu *v = cpu->vcpu;
        if (cpu == boot->cpu || !atomic_load_explicit(&cpu->online, memory_order_acquire)
         || v == NULL || atomic_load_explicit(&v->parked, memory_order_relaxed)) {
            continue;
        }
        printf(
            "CPU %u, APIC ID %#x, has not stopped: its last exit was %s %#x\n",
            cpu->index, cpu->apic_id, virt_vmx ? "basic reason" : "code",
            atomic_load_explicit(&v->last_exit, memory_order_relaxed)
        );
    }
}

static void enter(struct vcpu *v) {
    size_t others = 0;
    for (size_t i = 0; i < smp_cpu_count(); i++) {
        struct cpu *cpu = smp_cpu(i);
        bool online = atomic_load_explicit(&cpu->online, memory_order_acquire);
        others += cpu != v->cpu && online ? 1 : 0;
    }
    if (!others_parked(others)) {
        name_running(v);
        panic("The other processors do not stop for the guest's S3\n");
    }
    uint64_t asked = rdtsc();
    printf("The guest puts the machine to sleep (S3)\n");
    vacpi_sleep_arm();
    usb_suspend();
    kbc_suspend();
    dma_suspend();
    uintptr_t vector = smp_trampoline_wake(woken, wake_stack, WAKE_STACK_SIZE);
    uacpi_status status = uacpi_set_waking_vector(vector, 0);
    if (status == UACPI_STATUS_OK) {
        status = uacpi_prepare_for_sleep_state(UACPI_SLEEP_STATE_S3);
    }
    bool slept = false;
    tsc_lost = 0;
    // By the TSC as tsc_restore() gives it back, the time asleep left out.
    uint64_t sleeping = 0;
    if (status == UACPI_STATUS_OK) {
        cli();
        virt_cpu_off();
        sleeping = rdtsc();
        if (cpu_resume_save(&resume) == 0) {
            wbinvd();
            status = uacpi_enter_sleep_state(UACPI_SLEEP_STATE_S3);
            // The machine did not sleep.
        } else {
            slept = true;
            uint64_t now = rdtsc();
            tsc_lost = now < sleeping ? sleeping - now : 0;
            tsc_moved = tsc_lost;
        }
    }
    sleep_cpu_wake(v->cpu);
    if (!slept) {
        printf("Cannot enter ACPI S3: %s\n", uacpi_status_to_string(status));
    }
    // What the machine's devices lost, the hypervisor's view of them gives
    // back, before anything reaches them.
    dma_resume();
    ioapic_resume();
    pci_virt_resume();
    pit_host_stop();
    hpet_wake();
    usb_resume();
    vacpi_sleep_woken(slept);
    if (slept) {
        uacpi_prepare_for_wake_from_sleep_state(UACPI_SLEEP_STATE_S3);
        uacpi_wake_from_sleep_state(UACPI_SLEEP_STATE_S3);
    }
    kbc_resume(slept);
    // Before the others start again, which would otherwise stop once more.
    atomic_store_explicit(&parked, 0, memory_order_relaxed);
    for (size_t i = 0; i < smp_cpu_count(); i++) {
        struct vcpu *other = smp_cpu(i)->vcpu;
        if (other != NULL) {
            atomic_store_explicit(&other->parked, false, memory_order_relaxed);
        }
    }
    atomic_store_explicit(&requested, 0, memory_order_release);
    smp_restart_aps();
    guest_wake(v);
    if (slept) {
        uint64_t hz = time_tsc_hz();
        printf(
            "The machine is awake: ReBIOS took %llu ms to put it to sleep, %llu ms to have it "
            "back\n",
            (unsigned long long)mul_div(sleeping - asked, 1000, hz),
            (unsigned long long)mul_div(rdtsc() - sleeping, 1000, hz)
        );
    }
}

void sleep_check(struct vcpu *v) {
    if (!sleep_requested()) {
        return;
    }
    if (!v->cpu->bsp) {
        park(v);
    }
    enter(v);
}
