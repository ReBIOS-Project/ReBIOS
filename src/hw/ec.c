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
#include <stddef.h>
#include <stdint.h>
#include <uacpi/acpi.h>
#include <uacpi/event.h>
#include <uacpi/kernel_api.h>
#include <uacpi/namespace.h>
#include <uacpi/opregion.h>
#include <uacpi/platform/types.h>
#include <uacpi/resources.h>
#include <uacpi/status.h>
#include <uacpi/tables.h>
#include <uacpi/types.h>
#include <uacpi/uacpi.h>
#include <uacpi/utilities.h>
#include <hw/ec.h>
#include <lib/log.h>
#include <lib/time.h>
#include <sys/task.h>
#include <sys/x86.h>

// EC_SC status bits and commands, ACPI 6.6, 12.2.1 and 12.3.
#define EC_OBF 0x01
#define EC_IBF 0x02
#define EC_SCI_EVT 0x20
#define EC_RD 0x80
#define EC_WR 0x81
#define EC_QR 0x84

// As long as the slowest controllers take to answer a byte.
#define EC_TIMEOUT_NS ((uint64_t)500000000)
// Queries in a row before the SCI_EVT flag counts as stuck.
#define EC_QUERY_LIMIT 64

#define EC_GLOBAL_LOCK_TIMEOUT 0xffff

#define EC_HID "PNP0C09"

static struct {
    uacpi_namespace_node *node;
    uint16_t data;
    uint16_t command;
    bool global_lock;
    unsigned int ports;
    uacpi_handle query_work;
    // A work item must not be scheduled again before its handler runs.
    bool query_pending;
} ec;

static bool wait_status(uint8_t mask, uint8_t value) {
    uintptr_t start = time_ns();
    while ((inb(ec.command) & mask) != value) {
        if (time_ns() - start > EC_TIMEOUT_NS) {
            return false;
        }
        if (!task_yield_if_over()) {
            cpu_relax();
        }
    }
    return true;
}

static bool send_command(uint8_t command) {
    if (!wait_status(EC_IBF, 0)) {
        return false;
    }
    outb(ec.command, command);
    return true;
}

static bool send_data(uint8_t value) {
    if (!wait_status(EC_IBF, 0)) {
        return false;
    }
    outb(ec.data, value);
    return true;
}

static bool receive_data(uint8_t *value) {
    if (!wait_status(EC_OBF, EC_OBF)) {
        return false;
    }
    *value = inb(ec.data);
    return true;
}

static bool ec_read(uint8_t address, uint8_t *value) {
    return send_command(EC_RD) && send_data(address) && receive_data(value);
}

static bool ec_write(uint8_t address, uint8_t value) {
    return send_command(EC_WR) && send_data(address) && send_data(value)
        && wait_status(EC_IBF, 0);
}

// The firmware's fields over the EC's space take the global lock themselves
// where they need it.
static uacpi_status region_handler(uacpi_region_op op, uacpi_handle op_data) {
    switch (op) {
        case UACPI_REGION_OP_ATTACH:
        case UACPI_REGION_OP_DETACH: {
            return UACPI_STATUS_OK;
        }
        case UACPI_REGION_OP_READ:
        case UACPI_REGION_OP_WRITE: {
            uacpi_region_rw_data *rw = op_data;
            if (rw->offset + rw->byte_width > 0x100) {
                return UACPI_STATUS_INVALID_ARGUMENT;
            }
            uint64_t value = op == UACPI_REGION_OP_READ ? 0 : rw->value;
            for (size_t i = 0; i < rw->byte_width; i++) {
                uint8_t address = (uint8_t)(rw->offset + i);
                uint8_t byte = (uint8_t)(value >> (8 * i));
                bool ok = op == UACPI_REGION_OP_READ ? ec_read(address, &byte)
                    : ec_write(address, byte);
                if (!ok) {
                    printf("Embedded controller timed out at %#x\n", address);
                    return UACPI_STATUS_HARDWARE_TIMEOUT;
                }
                value |= (uint64_t)byte << (8 * i);
            }
            if (op == UACPI_REGION_OP_READ) {
                rw->value = value;
            }
            return UACPI_STATUS_OK;
        }
        default: {
            return UACPI_STATUS_INVALID_ARGUMENT;
        }
    }
}

static bool query(uint8_t *value) {
    uint32_t seq = 0;
    if (ec.global_lock
     && uacpi_acquire_global_lock(EC_GLOBAL_LOCK_TIMEOUT, &seq)
     != UACPI_STATUS_OK) {
        return false;
    }
    bool ok = send_command(EC_QR) && receive_data(value);
    if (ec.global_lock) {
        uacpi_release_global_lock(seq);
    }
    return ok;
}

// Runs the _Qxx method of each event the EC has pending, 0 meaning none.
static void query_work(uacpi_handle ctx) {
    (void)ctx;
    ec.query_pending = false;
    for (size_t i = 0; i < EC_QUERY_LIMIT; i++) {
        if ((inb(ec.command) & EC_SCI_EVT) == 0) {
            return;
        }
        uint8_t q;
        if (!query(&q) || q == 0) {
            return;
        }
        static const char hex[] = "0123456789ABCDEF";
        char method[5] = { '_', 'Q', hex[q >> 4], hex[q & 0xf], '\0' };
        uacpi_status status = uacpi_execute(ec.node, method, NULL);
        if (status != UACPI_STATUS_OK) {
            printf(
                "Embedded controller event %#x: %s %s\n", q, method,
                uacpi_status_to_string(status)
            );
        }
    }
    printf(
        "Embedded controller events keep coming, %u in a row\n", EC_QUERY_LIMIT
    );
}

// The EC's GPE is edge-triggered, ACPI 6.6, 12.6.
static uacpi_interrupt_ret gpe_handler(
    uacpi_handle ctx, uacpi_namespace_node *gpe_device, uacpi_u16 idx
) {
    (void)ctx;
    (void)gpe_device;
    (void)idx;
    if (!ec.query_pending) {
        ec.query_pending = true;
        uacpi_kernel_schedule_work(
            UACPI_WORK_GPE_EXECUTION, ec.query_work, query_work, NULL
        );
    }
    return UACPI_INTERRUPT_HANDLED | UACPI_GPE_REENABLE;
}

// The first port is the data register, the second the command and status
// register, ACPI 6.6, Table 12.19.
static uacpi_iteration_decision port_found(void *user, uacpi_resource *r) {
    (void)user;
    uint16_t port;
    if (r->type == UACPI_RESOURCE_TYPE_IO) {
        port = r->io.minimum;
    } else if (r->type == UACPI_RESOURCE_TYPE_FIXED_IO) {
        port = r->fixed_io.address;
    } else {
        return UACPI_ITERATION_DECISION_CONTINUE;
    }
    if (ec.ports == 0) {
        ec.data = port;
    } else {
        ec.command = port;
    }
    ec.ports++;
    return ec.ports == 2 ? UACPI_ITERATION_DECISION_BREAK
        : UACPI_ITERATION_DECISION_CONTINUE;
}

static uacpi_iteration_decision device_found(
    void *user, uacpi_namespace_node *node, uacpi_u32 depth
) {
    (void)user;
    (void)depth;
    uacpi_u32 sta;
    if (uacpi_eval_sta(node, &sta) == UACPI_STATUS_OK
     && (sta & ACPI_STA_RESULT_DEVICE_PRESENT) == 0) {
        return UACPI_ITERATION_DECISION_CONTINUE;
    }
    ec.ports = 0;
    uacpi_for_each_device_resource(node, "_CRS", port_found, NULL);
    if (ec.ports < 2) {
        return UACPI_ITERATION_DECISION_CONTINUE;
    }
    ec.node = node;
    return UACPI_ITERATION_DECISION_BREAK;
}

// Without such a device, the ECDT can still name one.
static bool ecdt_find(void) {
    uacpi_table table;
    if (uacpi_table_find_by_signature(ACPI_ECDT_SIGNATURE, &table)
     != UACPI_STATUS_OK) {
        return false;
    }
    const struct acpi_ecdt *ecdt = table.ptr;
    bool found = false;
    if (ecdt->hdr.length > sizeof(struct acpi_ecdt)
     && ecdt->ec_control.address_space_id == UACPI_ADDRESS_SPACE_SYSTEM_IO
     && ecdt->ec_data.address_space_id == UACPI_ADDRESS_SPACE_SYSTEM_IO
     && uacpi_namespace_node_find(NULL, ecdt->ec_id, &ec.node)
     == UACPI_STATUS_OK) {
        ec.command = (uint16_t)ecdt->ec_control.address;
        ec.data = (uint16_t)ecdt->ec_data.address;
        found = true;
    }
    uacpi_table_unref(&table);
    return found;
}

// _GPE is an index into the FADT's GPE blocks, or a GPE block device and an
// index into it.
static bool gpe_find(uacpi_namespace_node **device, uacpi_u16 *idx) {
    uacpi_object *obj;
    if (uacpi_eval(ec.node, "_GPE", NULL, &obj) != UACPI_STATUS_OK) {
        return false;
    }
    bool found = false;
    uint64_t value;
    uacpi_object_array package;
    *device = NULL;
    if (uacpi_object_get_integer(obj, &value) == UACPI_STATUS_OK) {
        *idx = (uacpi_u16)value;
        found = true;
    } else if (uacpi_object_get_package(obj, &package) == UACPI_STATUS_OK
            && package.count == 2) {
        uacpi_namespace_node *node;
        bool resolved = uacpi_object_resolve_as_aml_namepath(
            package.objects[0], ec.node, &node
        ) == UACPI_STATUS_OK
            && uacpi_object_get_integer(package.objects[1], &value)
                == UACPI_STATUS_OK;
        if (resolved) {
            *device = node;
            *idx = (uacpi_u16)value;
            found = true;
        }
    }
    uacpi_object_unref(obj);
    return found;
}

void ec_init(void) {
    uacpi_find_devices(EC_HID, device_found, NULL);
    if (ec.node == NULL && !ecdt_find()) {
        return;
    }

    uint64_t glk;
    ec.global_lock
        = uacpi_eval_simple_integer(ec.node, "_GLK", &glk) == UACPI_STATUS_OK
        && glk != 0;
    const char *path = uacpi_namespace_node_generate_absolute_path(ec.node);
    printf(
        "Embedded controller %s, data port %#x, command port %#x%s\n", path,
        ec.data, ec.command, ec.global_lock ? ", global lock" : ""
    );
    uacpi_free_absolute_path(path);

    uacpi_status status = uacpi_install_address_space_handler(
        ec.node, UACPI_ADDRESS_SPACE_EMBEDDED_CONTROLLER, region_handler, NULL
    );
    if (status != UACPI_STATUS_OK) {
        printf(
            "Cannot handle the embedded controller's space: %s\n",
            uacpi_status_to_string(status)
        );
    }

    uacpi_namespace_node *gpe_device;
    uacpi_u16 idx;
    if (!gpe_find(&gpe_device, &idx)) {
        printf("The embedded controller has no usable _GPE\n");
        return;
    }
    ec.query_work = uacpi_kernel_create_work_item();
    status = uacpi_install_gpe_handler(
        gpe_device, idx, UACPI_GPE_TRIGGERING_EDGE, gpe_handler, NULL
    );
    if (status != UACPI_STATUS_OK) {
        printf(
            "Cannot handle the embedded controller's GPE %#x: %s\n", idx,
            uacpi_status_to_string(status)
        );
    }
}
