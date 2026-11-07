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
#include <uacpi/kernel_api.h>
#include <uacpi/namespace.h>
#include <uacpi/notify.h>
#include <uacpi/platform/types.h>
#include <uacpi/status.h>
#include <uacpi/types.h>
#include <uacpi/uacpi.h>
#include <virt/ospm.h>
#include <hw/acpi.h>
#include <hw/power.h>
#include <lib/log.h>
#include <lib/time.h>
#include <lib/util.h>
#include <memory.h>
#include <sys/x86.h>
#include <virt/hv_timer.h>
#include <virt/vacpi.h>

// How often the zones that give no _TZP are polled, and how often the guest is
// looked at while it has an OSPM, in tenths of a second.
#define DEFAULT_POLL_DS 100
#define MAX_ZONES 16

// A thermal zone's notifications (ACPI 6.6, Table 5.230): its temperature
// changed, and its trip points did.
#define NOTIFY_ZONE_STATUS 0x80
#define NOTIFY_ZONE_TRIP_POINTS 0x81

// 0 C, in the tenths of a kelvin that ACPI gives temperatures in.
#define ZERO_CELSIUS 2732

// A zone's active cooling levels, _AC0 to _AC9 (ACPI 6.6, 11.4.1), and the
// power resources that ReBIOS keeps count of.
#define ACTIVE_LEVELS 10
#define MAX_RESOURCES 32

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
        // The guest's OSPM finds the devices as ReBIOS left them.
        if (guest) {
            for (size_t i = 0; i < zone_count; i++) {
                memset(zones[i].active_on, 0, sizeof(zones[i].active_on));
            }
            resource_count = 0;
        }
    }
    if (!guest) {
        for (size_t i = 0; i < zone_count; i++) {
            check(&zones[i]);
            if (zones[i].poll_ds < next_ds) {
                next_ds = zones[i].poll_ds;
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
    if (value == NOTIFY_ZONE_TRIP_POINTS) {
        read_trip_points(z);
    }
    if (value == NOTIFY_ZONE_STATUS || value == NOTIFY_ZONE_TRIP_POINTS) {
        poll_soon(NULL);
    }
    return UACPI_STATUS_OK;
}

static uacpi_iteration_decision zone_found(
    void *ctx, uacpi_namespace_node *node, uacpi_u32 depth
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
