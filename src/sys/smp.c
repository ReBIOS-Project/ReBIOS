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
#include <stddef.h>
#include <stdbool.h>
#include <uacpi/acpi.h>
#include <uacpi/tables.h>
#include <memory.h>
#include <core.h>
#include <handoff.h>
#include <hw/acpi.h>
#include <hw/lapic.h>
#include <lib/heap.h>
#include <lib/log.h>
#include <lib/time.h>
#include <sys/cpu.h>
#include <sys/smp.h>
#include <sys/x86.h>
#include <virt/vcpu.h>
#include <virt/virt.h>

// How long the processors get to start, emulated ones included.
#define AP_START_TIMEOUT_NS 2000000000ull
#define INIT_DELAY_US 10000
#define SIPI_DELAY_US 200

extern const uint8_t ap_boot_start[];
extern const uint8_t ap_boot_gdt[];
extern const uint8_t ap_boot_gdtr[];
extern const uint8_t ap_boot_pm32[];
extern const uint8_t ap_boot_pm32_ptr[];
extern const uint8_t ap_boot_lm64[];
extern const uint8_t ap_boot_lm64_ptr[];
extern const uint8_t ap_boot_params[];
extern const uint8_t ap_boot_end[];

// The layouts ap_boot.S expects.
struct ap_boot_params {
    uint64_t cr3;
    uint64_t cr4;
    uint64_t efer;
    uint64_t table;
    uint32_t count;
    uint32_t reserved;
    uint64_t stack_offset;
    uint64_t entry;
};
_Static_assert(sizeof(struct ap_boot_params) == 56, "ap_boot.S has PARAM_SIZE 56");

struct ap_boot_entry {
    uint32_t apic_id;
    uint32_t reserved;
    uint64_t cpu;
};
_Static_assert(sizeof(struct ap_boot_entry) == 16, "ap_boot.S has ENTRY_SIZE 16");

static struct cpu **cpus;
static uint32_t cpu_count;
static uint32_t aps_started;

uint32_t smp_cpu_count(void) {
    return cpu_count;
}

struct cpu *smp_cpu(uint32_t index) {
    return index < cpu_count ? cpus[index] : NULL;
}

struct cpu *smp_cpu_by_apic_id(uint32_t apic_id) {
    for (uint32_t i = 0; i < cpu_count; i++) {
        if (cpus[i]->apic_id == apic_id) {
            return cpus[i];
        }
    }
    return NULL;
}

bool smp_guest_cpu(uint32_t apic_id) {
    // The boot processor runs the legacy BIOS, whatever the configuration.
    if (cpu_count != 0 && apic_id == cpus[0]->apic_id) {
        return true;
    }
    const struct handoff_config *c = &core_handoff->config;
    if (c->cpu_filter == HANDOFF_CPU_FILTER_NONE) {
        return true;
    }
    bool listed = false;
    for (uint32_t i = 0; i < c->cpu_range_count; i++) {
        if (apic_id >= c->cpu_ranges[i].first && apic_id <= c->cpu_ranges[i].last) {
            listed = true;
        }
    }
    return c->cpu_filter == HANDOFF_CPU_FILTER_ALLOW ? listed : !listed;
}

static void ap_main(struct cpu *cpu) {
    cpu_tables_load(cpu, virt_vmx);
    cpu->online = true;
    __atomic_add_fetch(&aps_started, 1, __ATOMIC_RELEASE);

    lapic_xapic_mode();
    virt_cpu_init();
    struct vcpu *v = vcpu_create(cpu);
    vcpu_wait_for_sipi(v);
    vcpu_run(v);
}

static void add_id(uint32_t *ids, uint32_t *count, uint32_t id) {
    for (uint32_t i = 0; i < *count; i++) {
        if (ids[i] == id) {
            return;
        }
    }
    ids[(*count)++] = id;
}

// The APIC IDs of the enabled processors the MADT lists, but the calling one.
static uint32_t madt_aps(uint32_t **ids_out) {
    *ids_out = NULL;
    struct uacpi_table table;
    if (!acpi_tables_ready()
        || uacpi_table_find_by_signature(ACPI_MADT_SIGNATURE, &table) != UACPI_STATUS_OK) {
        printf("No MADT, so no other processors\n");
        return 0;
    }
    const uint8_t *madt = table.ptr;
    uint32_t length = table.hdr->length;
    uint32_t self = lapic_cpu_apic_id();

    // At most one processor per entry.
    uint32_t *ids = kmalloc(length / sizeof(struct acpi_entry_hdr) * sizeof(*ids));
    uint32_t count = 0;
    uint32_t offset = sizeof(struct acpi_madt);
    while (offset + sizeof(struct acpi_entry_hdr) <= length) {
        uint8_t type = madt[offset];
        uint8_t entry_length = madt[offset + 1];
        if (entry_length < sizeof(struct acpi_entry_hdr) || length - offset < entry_length) {
            break;
        }
        if (type == ACPI_MADT_ENTRY_TYPE_LAPIC && entry_length >= sizeof(struct acpi_madt_lapic)) {
            struct acpi_madt_lapic entry;
            memcpy(&entry, madt + offset, sizeof(entry));
            if ((entry.flags & ACPI_PIC_ENABLED) && entry.id != self) {
                add_id(ids, &count, entry.id);
            }
        } else if (type == ACPI_MADT_ENTRY_TYPE_LOCAL_X2APIC
                   && entry_length >= sizeof(struct acpi_madt_x2apic)) {
            struct acpi_madt_x2apic entry;
            memcpy(&entry, madt + offset, sizeof(entry));
            if ((entry.flags & ACPI_PIC_ENABLED) && entry.id != self) {
                add_id(ids, &count, entry.id);
            }
        }
        offset += entry_length;
    }
    uacpi_table_unref(&table);
    *ids_out = ids;
    return count;
}

static void patch32(uint8_t *page, const uint8_t *field, uint64_t value) {
    uint32_t v = (uint32_t)value;
    memcpy(page + (field - ap_boot_start), &v, sizeof(v));
}

void smp_start_aps(void) {
    struct cpu *bsp = this_cpu();
    bsp->apic_id = lapic_cpu_apic_id();
    bsp->online = true;

    uint32_t *ids;
    uint32_t count = madt_aps(&ids);
    cpus = kmalloc((count + 1) * sizeof(*cpus));
    cpus[0] = bsp;
    cpu_count = 1;
    if (count == 0) {
        return;
    }
    uint64_t page = core_handoff->ap_boot;
    if (page == 0) {
        printf("Not starting the %u other processors without a page below 1 MiB\n", count);
        return;
    }
    if (read_cr3() >= (1ull << 32)) {
        panic("The page tables are beyond the other processors' reach\n");
    }

    struct ap_boot_entry *table = kmalloc(count * sizeof(*table));
    for (uint32_t i = 0; i < count; i++) {
        struct cpu *cpu = cpu_area_alloc(i + 1, ids[i], false);
        cpus[i + 1] = cpu;
        table[i] = (struct ap_boot_entry){ids[i], 0, (uint64_t)(uintptr_t)cpu};
    }
    __atomic_store_n(&cpu_count, count + 1, __ATOMIC_RELEASE);

    uint8_t *code = (uint8_t *)(uintptr_t)page;
    memcpy(code, ap_boot_start, ap_boot_end - ap_boot_start);
    patch32(code, ap_boot_gdtr + 2, page + (ap_boot_gdt - ap_boot_start));
    patch32(code, ap_boot_pm32_ptr, page + (ap_boot_pm32 - ap_boot_start));
    patch32(code, ap_boot_lm64_ptr, page + (ap_boot_lm64 - ap_boot_start));
    struct ap_boot_params params = {
        .cr3 = read_cr3(),
        .cr4 = CR4_PAE | (read_cr4() & CR4_LA57),
        .efer = rdmsr(MSR_EFER) & (EFER_LME | EFER_NXE),
        .table = (uint64_t)(uintptr_t)table,
        .count = count,
        .stack_offset = CPU_AREA_SIZE,
        .entry = (uint64_t)(uintptr_t)ap_main,
    };
    memcpy(code + (ap_boot_params - ap_boot_start), &params, sizeof(params));
    __atomic_thread_fence(__ATOMIC_SEQ_CST);

    // All at once: each processor finds its own area.
    uint32_t vector = (uint32_t)(page >> 12);
    lapic_send_ipi(0, ICR_SHORTHAND_OTHERS | ICR_DELIVERY_INIT | ICR_ASSERT);
    time_udelay(INIT_DELAY_US);
    lapic_send_ipi(0, ICR_SHORTHAND_OTHERS | ICR_DELIVERY_STARTUP | vector);
    time_udelay(SIPI_DELAY_US);
    lapic_send_ipi(0, ICR_SHORTHAND_OTHERS | ICR_DELIVERY_STARTUP | vector);

    uint64_t deadline = time_ns() + AP_START_TIMEOUT_NS;
    while (__atomic_load_n(&aps_started, __ATOMIC_ACQUIRE) < count && time_ns() < deadline) {
        cpu_relax();
    }
    printf("Started %u of %u other processors\n", __atomic_load_n(&aps_started, __ATOMIC_ACQUIRE),
           count);
    for (uint32_t i = 1; i <= count; i++) {
        if (!__atomic_load_n(&cpus[i]->online, __ATOMIC_ACQUIRE)) {
            printf("Processor %#x did not start\n", cpus[i]->apic_id);
        } else if (!smp_guest_cpu(cpus[i]->apic_id)) {
            printf("Processor %#x is not the guest's\n", cpus[i]->apic_id);
        }
    }
    kfree(ids);
}
