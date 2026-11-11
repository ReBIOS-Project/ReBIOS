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

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <uacpi/acpi.h>
#include <uacpi/kernel_api.h>
#include <uacpi/namespace.h>
#include <uacpi/notify.h>
#include <uacpi/platform/types.h>
#include <uacpi/resources.h>
#include <uacpi/status.h>
#include <uacpi/tables.h>
#include <uacpi/types.h>
#include <uacpi/uacpi.h>
#include <virt/ospm.h>
#include <hw/acpi.h>
#include <hw/power.h>
#include <lib/log.h>
#include <lib/time.h>
#include <lib/util.h>
#include <memory.h>
#include <sys/cpu.h>
#include <sys/smp.h>
#include <sys/x86.h>
#include <virt/hv_timer.h>
#include <virt/vacpi.h>
#include <virt/vcpu.h>

// How often the zones that give no _TZP are polled, and how often the guest is
// looked at while it has an OSPM, in tenths of a second.
#define DEFAULT_POLL_DS 100
#define MAX_ZONES 16

// A thermal zone's notifications (ACPI 6.6, Table 5.230): its temperature
// changed, its trip points did, and its lists of devices did.
#define NOTIFY_ZONE_STATUS 0x80
#define NOTIFY_ZONE_TRIP_POINTS 0x81
#define NOTIFY_ZONE_DEVICES 0x82

// 0 C, in the tenths of a kelvin that ACPI gives temperatures in.
#define ZERO_CELSIUS 2732

// A zone's active cooling levels, _AC0 to _AC9 (ACPI 6.6, 11.4.1), and the
// power resources that ReBIOS keeps count of.
#define ACTIVE_LEVELS 10
#define MAX_RESOURCES 32

// The steps that a zone throttles its processors by at most, and P_CNT's
// throttling enable, at the start of a P_BLK (ACPI 6.6, 4.8.1.5 and 8.1.1).
#define PASSIVE_STEPS_MAX 15
#define P_BLK_LENGTH 6
#define P_CNT_THT_EN ((uint32_t)1 << 4)
// The fields of a _TSS state (ACPI 6.6, 8.4.4.2).
#define TSS_PERCENT 0
#define TSS_CONTROL 3
#define TSS_FIELDS 5

struct zone {
    uacpi_namespace_node *node;
    // Its trip points, 0 where it has none.
    uint64_t critical;
    uint64_t hot;
    uint64_t poll_ds;
    // Its active trip points, hottest first, and whether ReBIOS has each
    // level's list on.
    uint64_t active[ACTIVE_LEVELS];
    size_t active_count;
    bool active_on[ACTIVE_LEVELS];
    // Its passive trip point, the constants of its trend (ACPI 6.6, 11.1.5.1),
    // the period to sample it at meanwhile, the slowest that its processors
    // may run at, in percent, by its _MTL, the steps that ReBIOS throttles
    // them by, and the last temperature it read, 0 before the first.
    uint64_t passive;
    int64_t tc1;
    int64_t tc2;
    uint64_t sample_ds;
    uint64_t min_percent;
    size_t steps;
    size_t steps_max;
    uint64_t last;
};

// A power resource that ReBIOS turned on, and the lists that want it, as lists
// may share one.
struct resource {
    uacpi_namespace_node *node;
    size_t users;
};

static struct zone zones[MAX_ZONES];
static size_t zone_count;
static struct resource resources[MAX_RESOURCES];
static size_t resource_count;
static uacpi_handle work;
static _Atomic(bool) work_queued;
static struct hv_timer timer;
static bool guest_watches;

static long long celsius(uint64_t tenths_kelvin) {
    return ((long long)tenths_kelvin - ZERO_CELSIUS) / 10;
}

// An integer that the zone may lack, 0 then, or where it cannot be evaluated.
static uint64_t zone_integer(const struct zone *z, const char *name) {
    uint64_t value;
    bool found;
    if (!acpi_eval_optional(z->node, name, &value, &found) || !found) {
        return 0;
    }
    return value;
}

static void read_trip_points(struct zone *z) {
    z->critical = zone_integer(z, "_CRT");
    z->hot = zone_integer(z, "_HOT");
    z->poll_ds = zone_integer(z, "_TZP");
    if (z->poll_ds == 0) {
        z->poll_ds = DEFAULT_POLL_DS;
    }
    z->passive = zone_integer(z, "_PSV");
    z->tc1 = (int64_t)zone_integer(z, "_TC1");
    z->tc2 = (int64_t)zone_integer(z, "_TC2");
    z->sample_ds = zone_integer(z, "_TSP");
    if (z->sample_ds == 0) {
        z->sample_ds = z->poll_ds;
    }
    z->min_percent = zone_integer(z, "_MTL");
    z->steps_max = PASSIVE_STEPS_MAX;
    z->active_count = 0;
    for (size_t x = 0; x < ACTIVE_LEVELS; x++) {
        char name[5] = { '_', 'A', 'C', (char)('0' + x), '\0' };
        uint64_t trip = zone_integer(z, name);
        if (trip == 0) {
            break;
        }
        z->active[z->active_count++] = trip;
    }
}

// A package element that names an object, as a name or as a string, which
// firmware gives either way.
static uacpi_namespace_node *element_node(uacpi_object *element, uacpi_namespace_node *scope) {
    uacpi_data_view text;
    if (uacpi_object_get_string(element, &text) != UACPI_STATUS_OK) {
        return NULL;
    }
    uacpi_namespace_node *node;
    uacpi_status status = uacpi_namespace_node_resolve_from_aml_namepath(
        scope, text.const_text, &node
    );
    return status == UACPI_STATUS_OK ? node : NULL;
}

// A method that the node may lack.
static void run_optional(uacpi_namespace_node *node, const char *name) {
    uacpi_status status = uacpi_eval(node, name, UACPI_NULL, UACPI_NULL);
    if (status != UACPI_STATUS_OK && status != UACPI_STATUS_NOT_FOUND) {
        acpi_log_failure(node, name, status);
    }
}

// Turns a power resource on for its first user, and off as its last goes. One
// that the table has no room for stays on.
static void resource_use(uacpi_namespace_node *node, bool on) {
    struct resource *r = NULL;
    for (size_t i = 0; i < resource_count && r == NULL; i++) {
        if (resources[i].node == node) {
            r = &resources[i];
        }
    }
    if (r == NULL && on && resource_count < MAX_RESOURCES) {
        r = &resources[resource_count++];
        *r = (struct resource){ node, 0 };
    }
    if (r == NULL) {
        if (on) {
            run_optional(node, "_ON");
        }
        return;
    }
    if (on && r->users++ == 0) {
        run_optional(node, "_ON");
    } else if (!on && r->users > 0 && --r->users == 0) {
        run_optional(node, "_OFF");
    }
}

static void device_power(uacpi_namespace_node *device, bool on);

// Turns on or off the power resources that a list of the owner's names, and
// the devices too where an active list may hold them.
static void list_power(uacpi_namespace_node *owner, const char *name, bool on, bool devices) {
    uacpi_object *list;
    uacpi_status status = uacpi_eval_simple_package(owner, name, &list);
    if (status != UACPI_STATUS_OK) {
        if (status != UACPI_STATUS_NOT_FOUND) {
            acpi_log_failure(owner, name, status);
        }
        return;
    }
    uacpi_object_array elements;
    if (uacpi_object_get_package(list, &elements) == UACPI_STATUS_OK) {
        for (size_t i = 0; i < elements.count; i++) {
            uacpi_namespace_node *node = element_node(elements.objects[i], owner);
            uacpi_object_type type;
            if (node == NULL || uacpi_namespace_node_type(node, &type) != UACPI_STATUS_OK) {
                continue;
            }
            if (type == UACPI_OBJECT_POWER_RESOURCE) {
                resource_use(node, on);
            } else if (type == UACPI_OBJECT_DEVICE && devices) {
                device_power(node, on);
            }
        }
    }
    uacpi_object_unref(list);
}

// A device in an active list runs in D0 and stops in D3, through the power
// resources of its _PR0, and its _PS0 and _PS3 (ACPI 6.6, 7.3).
static void device_power(uacpi_namespace_node *device, bool on) {
    if (!on) {
        run_optional(device, "_PS3");
    }
    list_power(device, "_PR0", on, false);
    if (on) {
        run_optional(device, "_PS0");
    }
}

// Each active level runs its list from its trip point up (ACPI 6.6, 11.1.4).
static void cool_actively(struct zone *z, uint64_t now) {
    for (size_t x = 0; x < ACTIVE_LEVELS; x++) {
        bool want = x < z->active_count && now >= z->active[x];
        if (want == z->active_on[x]) {
            continue;
        }
        z->active_on[x] = want;
        char name[5] = { '_', 'A', 'L', (char)('0' + x), '\0' };
        list_power(z->node, name, want, true);
        const char *path = acpi_path(z->node);
        printf(
            "Thermal zone %s at %lld C, its active cooling level %zu %s\n", path, celsius(now), x,
            want ? "on" : "off"
        );
        acpi_path_free(path);
    }
}

// A field of a _TSS state.
static bool tss_field(uacpi_object *state, size_t field, uint64_t *value) {
    uacpi_object_array fields;
    return uacpi_object_get_package(state, &fields) == UACPI_STATUS_OK
        && fields.count >= TSS_FIELDS
        && uacpi_object_get_integer(fields.objects[field], value) == UACPI_STATUS_OK;
}

// What a Processor() gives: its ACPI Processor ID, and its P_BLK.
static bool processor_info(uacpi_namespace_node *processor, uacpi_processor_info *info) {
    uacpi_object *object;
    uacpi_status status = uacpi_eval_typed(
        processor, NULL, NULL, UACPI_OBJECT_PROCESSOR_BIT, &object
    );
    if (status != UACPI_STATUS_OK) {
        return false;
    }
    bool got = uacpi_object_get_processor_info(object, info) == UACPI_STATUS_OK;
    uacpi_object_unref(object);
    return got;
}

// The processor's CPU, by the ACPI Processor UID that its Processor() or its
// _UID gives, as the MADT has it.
static struct cpu *processor_cpu(uacpi_namespace_node *processor, uacpi_object_type type) {
    uint64_t uid;
    uacpi_processor_info info;
    if (type == UACPI_OBJECT_PROCESSOR) {
        if (!processor_info(processor, &info)) {
            return NULL;
        }
        uid = info.id;
    } else if (uacpi_eval_simple_integer(processor, "_UID", &uid) != UACPI_STATUS_OK) {
        return NULL;
    }
    struct uacpi_table table;
    if (uacpi_table_find_by_signature(ACPI_MADT_SIGNATURE, &table) != UACPI_STATUS_OK) {
        return NULL;
    }
    struct cpu *cpu = NULL;
    size_t offset = sizeof(struct acpi_madt);
    const struct acpi_entry_hdr *entry;
    while (cpu == NULL
        && (entry = acpi_madt_next(table.ptr, table.hdr->length, &offset)) != NULL) {
        uint32_t apic_id;
        uint32_t entry_uid;
        uint32_t flags;
        if (acpi_madt_processor(entry, &apic_id, &entry_uid, &flags) && entry_uid == uid) {
            cpu = smp_cpu_by_apic_id(apic_id);
        }
    }
    uacpi_table_unref(&table);
    return cpu;
}

// The register that the processor's _PTC names to control its T-states (ACPI
// 6.6, 8.4.4.1), if it has one.
static bool tstate_register(
    uacpi_namespace_node *processor,
    uacpi_resource_generic_register *reg
) {
    uacpi_object *ptc;
    if (uacpi_eval_simple_package(processor, "_PTC", &ptc) != UACPI_STATUS_OK) {
        return false;
    }
    bool found = false;
    uacpi_object_array registers;
    uacpi_data_view buffer;
    uacpi_resource *resource;
    if (uacpi_object_get_package(ptc, &registers) == UACPI_STATUS_OK && registers.count >= 1
     && uacpi_object_get_buffer(registers.objects[0], &buffer) == UACPI_STATUS_OK
     && uacpi_get_resource_from_buffer(buffer, &resource) == UACPI_STATUS_OK) {
        found = resource->type == UACPI_RESOURCE_TYPE_GENERIC_REGISTER;
        if (found) {
            *reg = resource->generic_register;
        }
        uacpi_free_resource(resource);
    }
    uacpi_object_unref(ptc);
    return found;
}

// A T-state's control value into the register, as Linux 7.3 writes it
// (processor_throttling.c, acpi_write_throttling_state()): a port's bits, or
// for Intel's functional fixed hardware, the processor's
// IA32_CLOCK_MODULATION, which it writes itself.
static bool tstate_write(
    const uacpi_resource_generic_register *reg,
    uacpi_namespace_node *processor,
    uacpi_object_type type,
    uint64_t control
) {
    if (reg->address_space_id == UACPI_ADDRESS_SPACE_SYSTEM_IO && reg->address != 0
     && reg->address <= 0xfffc && reg->bit_width + reg->bit_offset <= 32) {
        uint16_t port = (uint16_t)reg->address;
        uint32_t mask = reg->bit_width < 32 ? ((uint32_t)1 << reg->bit_width) - 1 : UINT32_MAX;
        uint32_t value = ((uint32_t)control & mask) << reg->bit_offset;
        size_t width = reg->bit_width + reg->bit_offset;
        if (width <= 8) {
            outb(port, (uint8_t)value);
        } else if (width <= 16) {
            outw(port, (uint16_t)value);
        } else {
            outl(port, value);
        }
        return true;
    }
    if (reg->address_space_id != UACPI_ADDRESS_SPACE_FFIXEDHW
     || x86_cpu_vendor() != X86_CPU_VENDOR_INTEL || (cpuid(1, 0).edx & CPUID_1_EDX_ACPI) == 0) {
        return false;
    }
    struct cpu *cpu = processor_cpu(processor, type);
    if (cpu == NULL) {
        return false;
    }
    atomic_store_explicit(&cpu->clock_modulation, control, memory_order_release);
    vcpu_kick(cpu);
    return true;
}

// The T-state of the processor's _TSS that is steps slower than the fastest
// that its _TPC allows, or its slowest, but none slower than min_percent (ACPI
// 6.6, 8.4.4), if it has them, and how many steps slower than the fastest the
// slowest is.
static bool tstate(
    uacpi_namespace_node *processor,
    uacpi_object_type type,
    size_t steps,
    uint64_t min_percent,
    size_t *most
) {
    uacpi_resource_generic_register reg;
    uacpi_object *tss;
    if (!tstate_register(processor, &reg)
     || uacpi_eval_simple_package(processor, "_TSS", &tss) != UACPI_STATUS_OK) {
        return false;
    }
    bool done = false;
    uacpi_object_array states;
    if (uacpi_object_get_package(tss, &states) == UACPI_STATUS_OK && states.count != 0) {
        uint64_t first;
        bool found;
        if (!acpi_eval_optional(processor, "_TPC", &first, &found) || first >= states.count) {
            first = 0;
        }
        size_t slowest = states.count - 1;
        uint64_t percent;
        while (slowest > first && tss_field(states.objects[slowest], TSS_PERCENT, &percent)
            && percent < min_percent) {
            slowest--;
        }
        *most = slowest - first;
        size_t index = first + steps < slowest ? first + steps : slowest;
        uint64_t control;
        done = tss_field(states.objects[index], TSS_CONTROL, &control)
            && tstate_write(&reg, processor, type, control);
    }
    uacpi_object_unref(tss);
    return done;
}

// The duty cycle of the processor's P_CNT, at the start of its P_BLK, where
// the FADT's DUTY_OFFSET and DUTY_WIDTH place it, which changes only while
// THT_EN is clear (ACPI 6.6, 4.8.1.5 and 8.1.1): steps settings slower than
// full speed, but none slower than min_percent, if it has one, and how many
// steps slower than full speed the slowest is.
static bool duty_cycle(
    uacpi_namespace_node *processor,
    uacpi_object_type type,
    size_t steps,
    uint64_t min_percent,
    size_t *most
) {
    struct acpi_fadt *fadt;
    uacpi_processor_info info;
    if (type != UACPI_OBJECT_PROCESSOR || uacpi_table_fadt(&fadt) != UACPI_STATUS_OK
     || fadt->duty_width == 0 || fadt->duty_offset + fadt->duty_width > 32
     || !processor_info(processor, &info)) {
        return false;
    }
    uint32_t settings = (uint32_t)1 << fadt->duty_width;
    uint32_t mask = (settings - 1) << fadt->duty_offset;
    if (info.block_length < P_BLK_LENGTH || info.block_address == 0 || info.block_address > 0xfffc
     || (mask & P_CNT_THT_EN) != 0) {
        return false;
    }
    uint32_t slowest = (uint32_t)((min_percent * settings + 99) / 100);
    if (slowest == 0) {
        slowest = 1;
    } else if (slowest >= settings) {
        slowest = settings - 1;
    }
    *most = settings - slowest;
    uint16_t port = (uint16_t)info.block_address;
    uint32_t value = inl(port) & ~P_CNT_THT_EN;
    outl(port, value);
    if (steps == 0) {
        return true;
    }
    uint32_t duty = steps < settings - slowest ? settings - (uint32_t)steps : slowest;
    value = (value & ~mask) | duty << fadt->duty_offset;
    outl(port, value);
    outl(port, value | P_CNT_THT_EN);
    return true;
}

// The processors that the zone's _PSL names, throttled by its steps: the most
// steps that any of them can take.
static size_t throttle(const struct zone *z) {
    size_t most = 0;
    uacpi_object *list;
    uacpi_status status = uacpi_eval_simple_package(z->node, "_PSL", &list);
    if (status != UACPI_STATUS_OK) {
        acpi_log_failure(z->node, "_PSL", status);
        return most;
    }
    uacpi_object_array elements;
    if (uacpi_object_get_package(list, &elements) == UACPI_STATUS_OK) {
        for (size_t i = 0; i < elements.count; i++) {
            uacpi_namespace_node *node = element_node(elements.objects[i], z->node);
            uacpi_object_type type;
            if (node == NULL || uacpi_namespace_node_type(node, &type) != UACPI_STATUS_OK
             || (type != UACPI_OBJECT_PROCESSOR && type != UACPI_OBJECT_DEVICE)) {
                continue;
            }
            size_t its = 0;
            if (!tstate(node, type, z->steps, z->min_percent, &its)
             && !duty_cycle(node, type, z->steps, z->min_percent, &its)) {
                const char *path = acpi_path(node);
                printf("Processor %s has no throttling that ReBIOS can use\n", path);
                acpi_path_free(path);
            }
            if (its > most) {
                most = its;
            }
        }
    }
    uacpi_object_unref(list);
    return most;
}

// The zone's processors are throttled a step at a time as the trend of ACPI
// 6.6's equation 1 goes (11.1.5.1), as Linux 7.3's step-wise governor does
// (thermal.c, thermal_get_trend(), and gov_step_wise.c, get_target_state()):
// a step slower as it rises past the trip point, and one faster as it falls,
// but no less than one, and all of them back once it falls below it.
static void cool_passively(struct zone *z, uint64_t now) {
    if (z->passive == 0) {
        return;
    }
    int64_t rise = z->last != 0 ? (int64_t)now - (int64_t)z->last : 0;
    int64_t trend = z->tc1 * rise + z->tc2 * ((int64_t)now - (int64_t)z->passive);
    z->last = now;
    size_t steps = z->steps;
    if (now >= z->passive && trend > 0 && steps < z->steps_max) {
        steps++;
    } else if (now >= z->passive && trend < 0 && steps > 1) {
        steps--;
    } else if (now < z->passive && trend < 0) {
        steps = 0;
    }
    if (steps == z->steps) {
        return;
    }
    size_t before = z->steps;
    z->steps = steps;
    z->steps_max = throttle(z);
    if (z->steps > z->steps_max) {
        z->steps = z->steps_max;
    }
    if (z->steps == before) {
        return;
    }
    const char *path = acpi_path(z->node);
    printf(
        "Thermal zone %s at %lld C, its processors' throttling at step %zu\n", path, celsius(now),
        z->steps
    );
    acpi_path_free(path);
}

// Past its critical trip point, an OSPM shuts the machine down at once (ACPI
// 6.6, 11.1.6), and past its hot one, where a guest without one could not go
// to sleep, so does ReBIOS.
static void check(struct zone *z) {
    uint64_t now;
    uacpi_status status = uacpi_eval_simple_integer(z->node, "_TMP", &now);
    if (status != UACPI_STATUS_OK) {
        acpi_log_failure(z->node, "_TMP", status);
        return;
    }
    const char *trip = NULL;
    uint64_t at = 0;
    if (z->critical != 0 && now >= z->critical) {
        trip = "critical";
        at = z->critical;
    } else if (z->hot != 0 && now >= z->hot) {
        trip = "hot";
        at = z->hot;
    }
    if (trip != NULL) {
        const char *path = acpi_path(z->node);
        printf(
            "Thermal zone %s at %lld C, past its %s trip point of %lld C\n", path, celsius(now),
            trip, celsius(at)
        );
        acpi_path_free(path);
        power_off();
    }
    cool_actively(z, now);
    cool_passively(z, now);
}

// In the AML task.
static void poll(uacpi_handle ctx) {
    (void)ctx;
    atomic_store_explicit(&work_queued, false, memory_order_release);
    uint64_t next_ds = DEFAULT_POLL_DS;
    bool guest = vacpi_guest_ospm();
    if (guest != guest_watches) {
        guest_watches = guest;
        printf("The thermal zones are the %s's to watch\n", guest ? "guest" : "hypervisor");
        // The guest's OSPM finds the devices as ReBIOS left them, but for its
        // processors at full speed.
        if (guest) {
            for (size_t i = 0; i < zone_count; i++) {
                struct zone *z = &zones[i];
                memset(z->active_on, 0, sizeof(z->active_on));
                if (z->steps != 0) {
                    z->steps = 0;
                    z->steps_max = throttle(z);
                }
                z->last = 0;
            }
            resource_count = 0;
        }
    }
    if (!guest) {
        for (size_t i = 0; i < zone_count; i++) {
            struct zone *z = &zones[i];
            check(z);
            // Passive cooling samples the zone at its own period meanwhile.
            bool sampling = z->passive != 0 && (z->steps != 0 || z->last >= z->passive);
            uint64_t period = sampling ? z->sample_ds : z->poll_ds;
            if (period < next_ds) {
                next_ds = period;
            }
        }
    }
    hv_timer_set(&timer, rdtsc() + mul_div(next_ds, time_tsc_hz(), 10));
}

static void poll_soon(void *ctx) {
    (void)ctx;
    if (!atomic_exchange_explicit(&work_queued, true, memory_order_acq_rel)) {
        uacpi_kernel_schedule_work(UACPI_WORK_NOTIFICATION, work, poll, NULL);
    }
}

// In the AML task.
static uacpi_status zone_notified(uacpi_handle ctx, uacpi_namespace_node *node, uacpi_u64 value) {
    (void)node;
    struct zone *z = ctx;
    if (value == NOTIFY_ZONE_TRIP_POINTS || value == NOTIFY_ZONE_DEVICES) {
        read_trip_points(z);
    }
    if (value == NOTIFY_ZONE_STATUS || value == NOTIFY_ZONE_TRIP_POINTS
     || value == NOTIFY_ZONE_DEVICES) {
        poll_soon(NULL);
    }
    return UACPI_STATUS_OK;
}

static uacpi_iteration_decision zone_found(
    void *ctx,
    uacpi_namespace_node *node,
    uacpi_u32 depth
) {
    (void)ctx;
    (void)depth;
    if (zone_count == MAX_ZONES) {
        return UACPI_ITERATION_DECISION_BREAK;
    }
    struct zone *z = &zones[zone_count++];
    z->node = node;
    read_trip_points(z);
    uacpi_install_notify_handler(node, zone_notified, z);
    return UACPI_ITERATION_DECISION_CONTINUE;
}

void ospm_init(void) {
    if (!acpi_init()) {
        return;
    }
    acpi_lock();
    uacpi_namespace_for_each_child(
        uacpi_namespace_root(), zone_found, NULL, UACPI_OBJECT_THERMAL_ZONE_BIT,
        UACPI_MAX_DEPTH_ANY, NULL
    );
    acpi_unlock();
    if (zone_count == 0) {
        return;
    }
    printf("Thermal zones that ReBIOS watches while the guest has no OSPM: %zu\n", zone_count);
    work = uacpi_kernel_create_work_item();
    timer.fired = poll_soon;
    hv_timer_add(&timer);
    hv_timer_set(&timer, rdtsc());
}
