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

#include <stdbool.h>
#include <core.h>
#include <csm/csm_boot.h>
#include <handoff.h>
#include <hw/acpi.h>
#include <hw/ioapic.h>
#include <hw/lapic.h>
#include <hw/lapic_legacy.h>
#include <hw/pci_assign.h>
#include <hw/pci_cfg.h>
#include <hw/pci_enum.h>
#include <hw/pm_timer.h>
#include <hw/tsc.h>
#include <hw/usb_handoff.h>
#include <iommu/dma.h>
#include <iommu/remap.h>
#include <lib/log.h>
#include <lib/time.h>
#include <sys/cpu.h>
#include <sys/mm.h>
#include <sys/simd.h>
#include <sys/smp.h>
#include <virt/a20.h>
#include <virt/cpuid.h>
#include <virt/ferr.h>
#include <virt/hpet.h>
#include <virt/igfx.h>
#include <virt/io.h>
#include <virt/irq.h>
#include <virt/isa_dma.h>
#include <virt/isa_ports.h>
#include <virt/kbc.h>
#include <virt/layout.h>
#include <virt/log_save.h>
#include <virt/msr.h>
#include <virt/mtrr.h>
#include <virt/nsvm.h>
#include <virt/nvmx.h>
#include <virt/ospm.h>
#include <virt/pci_plan.h>
#include <virt/pci_quirks.h>
#include <virt/pci_virt.h>
#include <virt/pit.h>
#include <virt/profile.h>
#include <virt/rtc.h>
#include <virt/svm.h>
#include <virt/usb.h>
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

    msr_misc_enable_clear();
    acpi_init();
    pm_timer_init();
    tsc_calibrate();
    rtc_sample();
    pci_cfg_init();
    pci_enum_scan();
    pci_enum_shrink_rebars(PCI_PLAN_LOW_BAR_MAX);
    pci_assign_unassigned_bars();
    usb_handoff();
    igfx_scan();
    dma_init();

    if (!virt_init()) {
        panic("Neither VMX nor SVM is usable on this processor\n");
    }
    cpu_tables_load(cpu, virt_vmx);
    io_init();
    virt_cpu_init();
    simd_init();
    simd_cpu_init(cpu);
    msr_init();
    cpuid_init();
    mtrr_init();

    layout_init();
    pci_virt_init();
    igfx_init();
    a20_init();
    vga_init();
    pci_quirks_init();
    usb_init();
    // The guest's boot processor gets its local APIC as a legacy BIOS hands it
    // over, the hypervisor the processor's.
    lapic_legacy_init();
    smp_init();
    vlapic_init();
    nvmx_init();
    nsvm_init();
    msr_allowed_init();
    lapic_host_init(VLAPIC_SPURIOUS_VECTOR, VLAPIC_TIMER_VECTOR);
    smp_start_aps();
    ioapic_init();
    remap_init(!virt_vmx && svm_avic, VLAPIC_KICK_VECTOR);
    vioapic_init();
    irq_init();
    // The legacy devices' emulations ask the namespace which real ones exist.
    if (acpi_init()) {
        acpi_namespace_start();
        pci_cfg_verify();
    }
    kbc_init();
    pit_init();
    rtc_init();
    isa_dma_init();
    ferr_init();
    isa_ports_init();
    hpet_init();
    vacpi_init();
    ospm_init();
    profile_init();

    struct vcpu *v = vcpu_create(cpu);
    csm_boot_start(v);
    igfx_switch_begin();
    dma_enable();
    igfx_switch_end();
    usb_start();
    log_save_start();
    log_hand_over_screen();
    vcpu_run(v);
}

void core_main(struct handoff *handoff) {
    core_handoff = handoff;

    printf(
        "ReBIOS core running, heap %#llx-%#llx\n", (unsigned long long)handoff->heap_base,
        (unsigned long long)(handoff->heap_base + handoff->heap_size)
    );

    mm_init(handoff->heap_base, handoff->heap_size);
    time_init(handoff->tsc_hz);

    struct cpu *bsp = cpu_area_alloc(0, 0, true);
    cpu_switch_stack(cpu_stack_top(bsp), bsp_main, handoff);
}
