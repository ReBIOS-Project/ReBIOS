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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <uacpi/acpi.h>
#include <uacpi/namespace.h>
#include <uacpi/platform/types.h>
#include <uacpi/status.h>
#include <uacpi/tables.h>
#include <uacpi/types.h>
#include <uacpi/uacpi.h>
#include <uacpi/utilities.h>
#include <hw/acpi_cpu.h>
#include <hw/acpi.h>
#include <lib/heap.h>
#include <lib/log.h>
#include <lib/util.h>
#include <memory.h>
#include <sys/x86.h>

// What the OS tells the firmware it does with the processors, in Intel's
// processor vendor-specific ACPI interface, as Linux 7.3's
// arch_acpi_set_proc_cap_bits() sets the bits that its
// include/acpi/proc_cap_intel.h names: C-states, through HLT, and through
// MWAIT where the processor has it, T-states, coordinated by software, and
// through IA32_CLOCK_MODULATION where it has the ACPI feature, P-states,
// through IA32_PERF_CTL where it has Enhanced Intel SpeedStep, and
// collaborative performance control where it has HWP. The guest manages them
// all itself.
#define CAP_P_FFH 0x0001
#define CAP_C_C1_HALT 0x0002
#define CAP_T_FFH 0x0004
#define CAP_SMP_C1PT 0x0008
#define CAP_SMP_C2C3 0x0010
#define CAP_SMP_P_SWCOORD 0x0020
#define CAP_SMP_T_SWCOORD 0x0080
#define CAP_C_C1_FFH 0x0100
#define CAP_C_C2C3_FFH 0x0200
#define CAP_SMP_P_HWCOORD 0x0800
#define CAP_COLLAB_PROC_PERF 0x1000
#define CAP_EST (CAP_SMP_C1PT | CAP_C_C1_HALT | CAP_SMP_P_SWCOORD | CAP_SMP_P_HWCOORD | CAP_P_FFH)
#define CAP_C (CAP_SMP_C2C3 | CAP_SMP_C1PT | CAP_C_C1_HALT | CAP_C_C1_FFH | CAP_C_C2C3_FFH)
#define CPUID_1_ECX_MONITOR ((uint32_t)1 << 3)
#define CPUID_1_ECX_EST ((uint32_t)1 << 7)
#define CPUID_1_EDX_ACPI ((uint32_t)1 << 22)
#define CPUID_6_EAX_HWP ((uint32_t)1 << 7)
#define PDC_REVISION 1
#define OSC_REVISION 1
#define OSC_DWORDS 2
// The one error that fails an _OSC request without the query flag, as Linux
// 7.3's acpi_osc_error_check() has it.
#define OSC_INVALID_UUID 0x00000004

// 4077A616-290C-47BE-9EBD-D87058713953, the processors' _OSC, in ToUUID's byte
// order.
static const uint8_t osc_uuid[16] = {
    0x16, 0xa6, 0x77, 0x40, 0x0c, 0x29, 0xbe, 0x47, 0x9e, 0xbd, 0xd8, 0x70, 0x58, 0x71, 0x39, 0x53
};

static const char *const processor_device_ids[] = { "ACPI0007", NULL };

// The processors that the MADT has enabled, as Linux 7.3's
// processor_physically_present() finds them: Processor objects by their
// processor ID, processor devices by their _UID.
struct processors {
    const uint8_t *madt;
    size_t madt_length;
    uacpi_namespace_node **nodes;
    size_t count;
    size_t capacity;
};

static void add(struct processors *p, uacpi_namespace_node *node, uint32_t uid) {
    size_t offset = sizeof(struct acpi_madt);
    const struct acpi_entry_hdr *hdr;
    while ((hdr = acpi_madt_next(p->madt, p->madt_length, &offset)) != NULL) {
        uint32_t apic_id;
        uint32_t entry_uid;
        uint32_t flags;
        if (acpi_madt_processor(hdr, &apic_id, &entry_uid, &flags) && entry_uid == uid
         && (flags & ACPI_PIC_ENABLED) != 0) {
            p->nodes = heap_grow(
                p->nodes, p->count, &p->capacity, sizeof(uacpi_namespace_node *), 8
            );
            p->nodes[p->count++] = node;
            return;
        }
    }
}

static uacpi_iteration_decision processor_object(
    void *user,
    uacpi_namespace_node *node,
    uacpi_u32 depth
) {
    (void)depth;
    uacpi_object *obj;
    if (uacpi_eval(node, NULL, NULL, &obj) == UACPI_STATUS_OK && obj != NULL) {
        uacpi_processor_info info = { 0 };
        if (uacpi_object_get_processor_info(obj, &info) == UACPI_STATUS_OK) {
            add(user, node, info.id);
        }
        uacpi_object_unref(obj);
    }
    return UACPI_ITERATION_DECISION_CONTINUE;
}

static uacpi_iteration_decision processor_device(
    void *user,
    uacpi_namespace_node *node,
    uacpi_u32 depth
) {
    (void)depth;
    uint64_t uid;
    if (uacpi_eval_simple_integer(node, "_UID", &uid) == UACPI_STATUS_OK) {
        add(user, node, (uint32_t)uid);
    }
    return UACPI_ITERATION_DECISION_CONTINUE;
}

static uint32_t capabilities(void) {
    struct cpuid_regs leaf1 = cpuid(1, 0);
    uint32_t caps = CAP_C | CAP_SMP_T_SWCOORD;
    if ((leaf1.ecx & CPUID_1_ECX_EST) != 0) {
        caps |= CAP_EST;
    }
    if ((leaf1.edx & CPUID_1_EDX_ACPI) != 0) {
        caps |= CAP_T_FFH;
    }
    if (cpuid(0, 0).eax >= 6 && (cpuid(6, 0).eax & CPUID_6_EAX_HWP) != 0) {
        caps |= CAP_COLLAB_PROC_PERF;
    }
    if ((leaf1.ecx & CPUID_1_ECX_MONITOR) == 0) {
        caps &= ~(uint32_t)(CAP_C_C1_FFH | CAP_C_C2C3_FFH);
    }
    return caps;
}

// Whether the processor's _OSC took the request, as Linux 7.3's acpi_run_osc()
// asks: a buffer of the request's size back, without the invalid UUID bit.
static bool run_osc(uacpi_namespace_node *node, uint32_t capability_bits) {
    uint32_t caps[OSC_DWORDS] = { 0, capability_bits };
    uacpi_data_view uuid = { .const_bytes = osc_uuid, .length = sizeof(osc_uuid) };
    uacpi_data_view request = { .const_bytes = (const uint8_t *)caps, .length = sizeof(caps) };
    uacpi_object *args[4] = {
        uacpi_object_create_buffer(uuid), uacpi_object_create_integer(OSC_REVISION),
        uacpi_object_create_integer(OSC_DWORDS), uacpi_object_create_buffer(request)
    };
    uacpi_object_array array = { .objects = args, .count = ARRAY_COUNT(args) };
    uacpi_object *ret = NULL;
    bool made = args[0] != NULL && args[1] != NULL && args[2] != NULL && args[3] != NULL;
    uacpi_data_view view = { .const_bytes = NULL, .length = 0 };
    bool took = made && uacpi_eval(node, "_OSC", &array, &ret) == UACPI_STATUS_OK && ret != NULL
             && uacpi_object_get_type(ret) == UACPI_OBJECT_BUFFER
             && uacpi_object_get_string_or_buffer(ret, &view) == UACPI_STATUS_OK
             && view.length == sizeof(caps);
    if (took) {
        uint32_t status;
        memcpy(&status, view.const_bytes, sizeof(uint32_t));
        took = (status & OSC_INVALID_UUID) == 0;
    }
    uacpi_object_unref(ret);
    for (size_t i = 0; i < ARRAY_COUNT(args); i++) {
        uacpi_object_unref(args[i]);
    }
    return took;
}

static void run_pdc(uacpi_namespace_node *node, uint32_t capability_bits) {
    uint32_t request[3] = { PDC_REVISION, 1, capability_bits };
    uacpi_data_view view = { .const_bytes = (const uint8_t *)request, .length = sizeof(request) };
    uacpi_object *arg = uacpi_object_create_buffer(view);
    if (arg == NULL) {
        return;
    }
    uacpi_object_array array = { .objects = &arg, .count = 1 };
    uacpi_eval(node, "_PDC", &array, NULL);
    uacpi_object_unref(arg);
}

// Tells the firmware that the guest manages the processors' power, as Linux
// 7.3 does: through every processor's _OSC, else through their _PDC on Intel's
// processors.
static void tell_firmware(const struct processors *p) {
    if (p->count == 0) {
        return;
    }
    uint32_t caps = capabilities();
    bool osc = true;
    for (size_t i = 0; i < p->count && osc; i++) {
        osc = run_osc(p->nodes[i], caps);
    }
    if (osc) {
        printf("The firmware hears through _OSC that the guest manages the processors' power\n");
        return;
    }
    if (x86_cpu_vendor() != X86_CPU_VENDOR_INTEL) {
        return;
    }
    for (size_t i = 0; i < p->count; i++) {
        run_pdc(p->nodes[i], caps);
    }
    printf("The firmware hears through _PDC that the guest manages the processors' power\n");
}

// A P-state's core frequency in MHz, the first of its _PSS package.
static uint64_t frequency(uacpi_object *state) {
    uacpi_object_array fields;
    uint64_t mhz = 0;
    if (uacpi_object_get_package(state, &fields) == UACPI_STATUS_OK && fields.count != 0) {
        uacpi_object_get_integer(fields.objects[0], &mhz);
    }
    return mhz;
}

// Whether the firmware gives the guest P-states, which the log shows.
static bool show_pstates(const struct processors *p) {
    uacpi_object *pss = NULL;
    uacpi_object_array states = { .objects = NULL, .count = 0 };
    uacpi_status status = UACPI_STATUS_NOT_FOUND;
    if (p->count != 0) {
        status = uacpi_eval_typed(p->nodes[0], "_PSS", NULL, UACPI_OBJECT_PACKAGE_BIT, &pss);
    }
    if (status == UACPI_STATUS_OK) {
        uacpi_object_get_package(pss, &states);
    }
    if (states.count == 0) {
        printf("The firmware gives the processors no P-states\n");
    } else {
        const char *path = acpi_path(p->nodes[0]);
        printf(
            "The guest gets the processors' P-states, %zu in %s's _PSS, from %llu to %llu MHz\n",
            states.count, path, (unsigned long long)frequency(states.objects[0]),
            (unsigned long long)frequency(states.objects[states.count - 1])
        );
        acpi_path_free(path);
    }
    uacpi_object_unref(pss);
    return states.count != 0;
}

// A value of the FADT's written to its SMI_CMD, where both are there: false
// where not.
static bool smi_command(uint8_t value) {
    struct acpi_fadt *fadt;
    if (value == 0 || uacpi_table_fadt(&fadt) != UACPI_STATUS_OK || fadt->smi_cmd == 0
     || fadt->smi_cmd > UINT16_MAX) {
        return false;
    }
    outb((uint16_t)fadt->smi_cmd, value);
    return true;
}

// The FADT's PSTATE_CNT tells the firmware that the OS takes over the
// P-states, and its CST_CNT that the OS takes the C-states from _CST, with
// their change notifications (ACPI 6.6, Table 5.9), as Linux 7.3's
// acpi_processor_pstate_control() and acpi_processor_claim_cst_control() write
// them once they have those.
static void take_over(const struct processors *p, bool pstates) {
    struct acpi_fadt *fadt;
    if (uacpi_table_fadt(&fadt) != UACPI_STATUS_OK) {
        return;
    }
    if (pstates && smi_command(fadt->pstate_cnt)) {
        printf("The firmware hears through SMI_CMD that the guest sets the P-states\n");
    }
    uacpi_namespace_node *cst;
    if (p->count != 0 && uacpi_namespace_node_find(p->nodes[0], "_CST", &cst) == UACPI_STATUS_OK
     && smi_command(fadt->cst_cnt)) {
        printf("The firmware hears through SMI_CMD that the guest takes the C-states from _CST\n");
    }
}

void acpi_cpu_init(void) {
    struct uacpi_table table;
    if (uacpi_table_find_by_signature(ACPI_MADT_SIGNATURE, &table) != UACPI_STATUS_OK) {
        return;
    }
    struct processors p = {
        .madt = table.ptr, .madt_length = table.hdr->length, .nodes = NULL, .count = 0,
        .capacity = 0
    };
    uacpi_namespace_for_each_child(
        uacpi_namespace_root(), processor_object, NULL, UACPI_OBJECT_PROCESSOR_BIT,
        UACPI_MAX_DEPTH_ANY, &p
    );
    uacpi_find_devices_at(uacpi_namespace_root(), processor_device_ids, processor_device, &p);
    uacpi_table_unref(&table);
    tell_firmware(&p);
    take_over(&p, show_pstates(&p));
    kfree(p.nodes);
}
