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

struct zone {
    uacpi_namespace_node *node;
    // Its trip points, 0 where it has none.
    uint64_t critical;
    uint64_t hot;
    uint64_t poll_ds;
};

static struct zone zones[MAX_ZONES];
static size_t zone_count;
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
}

// Past its critical trip point, an OSPM shuts the machine down at once (ACPI
// 6.6, 11.1.6), and past its hot one, where a guest without one could not go
// to sleep, so does ReBIOS.
static void check(const struct zone *z) {
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
