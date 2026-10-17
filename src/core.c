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
#include <stdbool.h>
#include <core.h>
#include <csm/csm_boot.h>
#include <handoff.h>
#include <hw/acpi.h>
#include <hw/ioapic.h>
#include <hw/lapic.h>
#include <hw/lapic_legacy.h>
#include <hw/pci_cfg.h>
#include <hw/pci_relocate.h>
#include <iommu/dma.h>
#include <lib/log.h>
#include <lib/time.h>
#include <sys/cpu.h>
#include <sys/mm.h>
#include <sys/smp.h>
#include <sys/x86.h>
#include <virt/irq.h>
#include <virt/layout.h>
#include <virt/vacpi.h>
#include <virt/vcpu.h>
#include <virt/vga.h>
#include <virt/vioapic.h>
#include <virt/virt.h>
#include <virt/vlapic.h>

struct handoff *core_handoff;

static void bsp_main(void *arg) {
    (void)arg;
    struct cpu *cpu = this_cpu();

    acpi_init();
    pci_cfg_init();
    pci_relocate();
    dma_init();

    if (!virt_init()) {
        panic("Neither VMX nor SVM is usable on this processor\n");
    }
    cpu_tables_load(cpu, virt_vmx);
    virt_cpu_init();

    layout_init();
    vga_init();
    // The guest's boot processor gets its local APIC as a legacy BIOS hands
    // it over, the hypervisor the processor's.
    lapic_legacy_init();
    vlapic_init();
    lapic_host_init(VLAPIC_SPURIOUS_VECTOR, VLAPIC_TIMER_VECTOR);
    smp_start_aps();
    ioapic_init();
    vioapic_init();
    irq_init();
    vacpi_init();

    struct vcpu *v = vcpu_create(cpu);
    csm_boot_start(v);
    dma_enable();
    vcpu_run(v);
}

void core_main(struct handoff *handoff) {
    core_handoff = handoff;

    printf("ReBIOS core running, heap %#llx-%#llx\n",
           (unsigned long long)handoff->heap_base,
           (unsigned long long)(handoff->heap_base + handoff->heap_size));

    mm_init(handoff->heap_base, handoff->heap_size);
    time_init(handoff->tsc_hz);
    printf("TSC at %llu MHz\n", (unsigned long long)(handoff->tsc_hz / 1000000));

    struct cpu *bsp = cpu_area_alloc(0, 0, true);
    switch_stack(cpu_stack_top(bsp), bsp_main, handoff);
}
