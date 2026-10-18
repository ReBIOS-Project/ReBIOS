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
#include <stddef.h>
#include <stdbool.h>
#include <uacpi/namespace.h>
#include <uacpi/resources.h>
#include <uacpi/status.h>
#include <uacpi/types.h>
#include <uacpi/uacpi.h>
#include <uacpi/utilities.h>
#include <memory.h>
#include <hw/acpi_irq.h>
#include <hw/pci_cfg.h>
#include <hw/pci_enum.h>
#include <lib/heap.h>
#include <lib/log.h>
#include <lib/printf.h>

#define PCI_PINS 4
// The function number of every _PRT entry's Address: all functions, ACPI 6.6,
// 6.2.14.
#define PRT_ANY_FUNCTION 0xffffu
#define STA_PRESENT (1u << 0)
#define STA_ENABLED (1u << 1)
#define ISA_IRQS 16
#define ADDR_TEXT_SIZE 16

struct host_bridge {
    uacpi_namespace_node *node;
    uint16_t segment;
    uint8_t bus;
    bool present;
};

// A bus as the namespace describes it.
struct bus_info {
    uint16_t segment;
    uint8_t number;
    // Whether a host bridge has it: a pin its _PRT does not route goes no
    // further.
    bool root;
    uacpi_namespace_node *node;
    uacpi_pci_routing_table *prt;
};

struct lookup {
    struct host_bridge *host_bridges;
    uint32_t host_bridge_count;
    uint32_t host_bridge_capacity;
    // Each bus looked up has a function on it, if only a bridge, so there are
    // no more of them than functions.
    struct bus_info *buses;
    uint32_t bus_count;
};

struct link_search {
    uint32_t index;
    uint32_t position;
    struct acpi_irq_pci_route *route;
    bool found;
};

static const char *const host_bridge_ids[] = {"PNP0A03", "PNP0A08", NULL};
static const char *const link_ids[] = {"PNP0C0F", NULL};

static struct acpi_irq_pci_route *routes;
static uint32_t route_count;
static bool routes_done;

static uint16_t isa_claimed;
static bool isa_claimed_done;

static bool namespace_loaded(void) {
    return uacpi_get_current_init_level() >= UACPI_INIT_LEVEL_NAMESPACE_LOADED;
}

static void log_failure(uacpi_namespace_node *node, const char *name, uacpi_status status) {
    const char *path = uacpi_namespace_node_generate_absolute_path(node);
    printf("Cannot evaluate %s.%s: %s\n", path != NULL ? path : "?", name,
           uacpi_status_to_string(status));
    if (path != NULL) {
        uacpi_free_absolute_path(path);
    }
}

static bool device_present(uacpi_namespace_node *node) {
    uacpi_u32 sta;
    return uacpi_eval_sta(node, &sta) == UACPI_STATUS_OK && (sta & STA_PRESENT) != 0;
}

// _SEG and _BBN, which are 0 where a host bridge has none.
static bool eval_optional(uacpi_namespace_node *node, const char *name, uint64_t *value) {
    uacpi_status status = uacpi_eval_simple_integer(node, name, value);
    if (status == UACPI_STATUS_NOT_FOUND) {
        *value = 0;
        return true;
    }
    if (status != UACPI_STATUS_OK) {
        log_failure(node, name, status);
        return false;
    }
    return true;
}

static uacpi_iteration_decision host_bridge_found(void *user, uacpi_namespace_node *node,
                                                  uacpi_u32 depth) {
    (void)depth;
    struct lookup *l = user;
    if (!uacpi_device_matches_pnp_id(node, host_bridge_ids)) {
        return UACPI_ITERATION_DECISION_CONTINUE;
    }
    uint64_t segment;
    uint64_t bus;
    if (!eval_optional(node, "_SEG", &segment) || !eval_optional(node, "_BBN", &bus)) {
        return UACPI_ITERATION_DECISION_CONTINUE;
    }

    if (l->host_bridge_count == l->host_bridge_capacity) {
        uint32_t capacity = l->host_bridge_capacity == 0 ? 8 : l->host_bridge_capacity * 2;
        struct host_bridge *items = kmalloc(capacity * sizeof(*items));
        if (l->host_bridge_count != 0) {
            memcpy(items, l->host_bridges, l->host_bridge_count * sizeof(*items));
        }
        kfree(l->host_bridges);
        l->host_bridges = items;
        l->host_bridge_capacity = capacity;
    }
    // The low bits only, ACPI 6.6, 6.5.5 and 6.5.6.
    struct host_bridge *hb = &l->host_bridges[l->host_bridge_count++];
    *hb = (struct host_bridge){
        .node = node,
        .segment = (uint16_t)segment,
        .bus = (uint8_t)bus,
        .present = device_present(node),
    };

    const char *path = uacpi_namespace_node_generate_absolute_path(node);
    printf("PCI host bridge %s: segment %04x, bus %02x%s\n", path != NULL ? path : "?",
           hb->segment, hb->bus, hb->present ? "" : ", not present");
    if (path != NULL) {
        uacpi_free_absolute_path(path);
    }
    return UACPI_ITERATION_DECISION_CONTINUE;
}

// Should the namespace give a bus to more than one host bridge, those not
// present make way for one that is.
static uacpi_namespace_node *host_bridge_node(const struct lookup *l, uint16_t segment,
                                              uint8_t bus) {
    uacpi_namespace_node *first = NULL;
    for (uint32_t i = 0; i < l->host_bridge_count; i++) {
        const struct host_bridge *hb = &l->host_bridges[i];
        if (hb->segment != segment || hb->bus != bus) {
            continue;
        }
        if (hb->present) {
            return hb->node;
        }
        if (first == NULL) {
            first = hb->node;
        }
    }
    return first;
}

// The child device object with the address, a present one first, as with
// host bridges.
static uacpi_namespace_node *child_at(uacpi_namespace_node *parent, uint64_t adr) {
    uacpi_namespace_node *first = NULL;
    uacpi_namespace_node *iter = NULL;
    while (uacpi_namespace_node_next_typed(parent, &iter, UACPI_OBJECT_DEVICE_BIT)
           == UACPI_STATUS_OK) {
        uint64_t value;
        if (uacpi_eval_simple_integer(iter, "_ADR", &value) != UACPI_STATUS_OK || value != adr) {
            continue;
        }
        if (device_present(iter)) {
            return iter;
        }
        if (first == NULL) {
            first = iter;
        }
    }
    return first;
}

// The bus's device object is its host bridge, or else the child of the
// object of the bus above that has the address of the bus's bridge.
static struct bus_info *bus_get(struct lookup *l, uint16_t segment, uint8_t number,
                                const struct pci_function *bridge) {
    for (uint32_t i = 0; i < l->bus_count; i++) {
        if (l->buses[i].segment == segment && l->buses[i].number == number) {
            return &l->buses[i];
        }
    }

    uacpi_namespace_node *node = host_bridge_node(l, segment, number);
    bool root = node != NULL;
    if (!root && bridge != NULL) {
        const struct bus_info *above = bus_get(l, bridge->addr.segment, bridge->addr.bus,
                                               bridge->parent);
        if (above->node != NULL) {
            uint64_t adr = (uint64_t)bridge->addr.device << 16 | bridge->addr.function;
            node = child_at(above->node, adr);
        }
    }

    struct bus_info *info = &l->buses[l->bus_count++];
    *info = (struct bus_info){
        .segment = segment,
        .number = number,
        .root = root,
        .node = node,
        .prt = NULL,
    };
    if (node != NULL) {
        uacpi_status status = uacpi_get_pci_routing_table(node, &info->prt);
        if (status != UACPI_STATUS_OK) {
            info->prt = NULL;
            if (status != UACPI_STATUS_NOT_FOUND) {
                log_failure(node, "_PRT", status);
            }
        }
    }
    return info;
}

static const uacpi_pci_routing_table_entry *prt_find(const uacpi_pci_routing_table *prt,
                                                     uint8_t device, uint8_t pin) {
    if (prt == NULL) {
        return NULL;
    }
    uint32_t address = (uint32_t)device << 16 | PRT_ANY_FUNCTION;
    for (size_t i = 0; i < prt->num_entries; i++) {
        const uacpi_pci_routing_table_entry *entry = &prt->entries[i];
        if (entry->address == address && entry->pin == pin - 1) {
            return entry;
        }
    }
    return NULL;
}

// The pin of the bridge above that a device's pin reaches, PCI-to-PCI Bridge
// Architecture Specification 1.1, Table 9-1.
static uint8_t bridge_pin(uint8_t device, uint8_t pin) {
    return (uint8_t)((pin - 1 + device) % PCI_PINS + 1);
}

static void route_set(struct acpi_irq_pci_route *route, uint32_t gsi, uint8_t triggering,
                      uint8_t polarity) {
    route->gsi = gsi;
    route->level = triggering == UACPI_TRIGGERING_LEVEL;
    route->active_low = polarity == UACPI_POLARITY_ACTIVE_LOW;
}

static uacpi_iteration_decision link_resource(void *user, uacpi_resource *resource) {
    struct link_search *search = user;
    if (search->position++ != search->index) {
        return UACPI_ITERATION_DECISION_CONTINUE;
    }
    if (resource->type == UACPI_RESOURCE_TYPE_IRQ && resource->irq.num_irqs != 0) {
        route_set(search->route, resource->irq.irqs[0], resource->irq.triggering,
                  resource->irq.polarity);
        search->found = true;
    } else if (resource->type == UACPI_RESOURCE_TYPE_EXTENDED_IRQ
               && resource->extended_irq.num_irqs != 0) {
        route_set(search->route, resource->extended_irq.irqs[0],
                  resource->extended_irq.triggering, resource->extended_irq.polarity);
        search->found = true;
    }
    return UACPI_ITERATION_DECISION_BREAK;
}

static bool link_interrupt(uacpi_namespace_node *link, uint32_t index,
                           struct acpi_irq_pci_route *route) {
    uacpi_resources *resources;
    uacpi_status status = uacpi_get_current_resources(link, &resources);
    if (status != UACPI_STATUS_OK) {
        log_failure(link, "_CRS", status);
        return false;
    }
    struct link_search search = {.index = index, .route = route};
    uacpi_for_each_resource(resources, link_resource, &search);
    uacpi_free_resources(resources);
    return search.found;
}

// ACPI 6.6, 6.2.14: without a Source, the Source Index is the GSI.
static bool entry_resolve(const uacpi_pci_routing_table_entry *entry,
                          struct acpi_irq_pci_route *route) {
    if (entry->source == NULL) {
        // As PCI interrupt lines are.
        route_set(route, entry->index, UACPI_TRIGGERING_LEVEL, UACPI_POLARITY_ACTIVE_LOW);
        return true;
    }
    return link_interrupt(entry->source, entry->index, route);
}

static bool route_find(struct lookup *l, const struct pci_function *f,
                       struct acpi_irq_pci_route *route) {
    const struct pci_function *at = f;
    uint8_t device = f->addr.device;
    uint8_t pin = f->interrupt_pin;
    for (;;) {
        const struct bus_info *bus = bus_get(l, at->addr.segment, at->addr.bus, at->parent);
        const uacpi_pci_routing_table_entry *entry = prt_find(bus->prt, device, pin);
        if (entry != NULL) {
            return entry_resolve(entry, route);
        }
        if (bus->root || at->parent == NULL) {
            return false;
        }
        pin = bridge_pin(device, pin);
        at = at->parent;
        device = at->addr.device;
    }
}

static void format_addr(char *buf, size_t size, struct pci_addr addr) {
    if (addr.segment != 0) {
        snprintf(buf, size, "%04x:%02x:%02x.%x", addr.segment, addr.bus, addr.device,
                 addr.function);
    } else {
        snprintf(buf, size, "%02x:%02x.%x", addr.bus, addr.device, addr.function);
    }
}

static void routes_find(void) {
    const struct pci_topology *topology = pci_enum_scan();
    if (!namespace_loaded()) {
        printf("No ACPI namespace, so no PCI interrupt routes\n");
        return;
    }

    struct lookup l = {
        .host_bridges = NULL,
        .buses = kmalloc(topology->function_count * sizeof(struct bus_info)),
    };
    uacpi_namespace_for_each_child(uacpi_namespace_root(), host_bridge_found, NULL,
                                   UACPI_OBJECT_DEVICE_BIT, UACPI_MAX_DEPTH_ANY, &l);

    routes = kmalloc(topology->function_count * sizeof(*routes));
    for (uint32_t i = 0; i < topology->function_count; i++) {
        const struct pci_function *f = &topology->functions[i];
        if (f->interrupt_pin < 1 || f->interrupt_pin > PCI_PINS) {
            continue;
        }
        struct acpi_irq_pci_route route = {.addr = f->addr, .pin = f->interrupt_pin};
        char text[ADDR_TEXT_SIZE];
        format_addr(text, sizeof(text), f->addr);
        char pin = (char)('A' + f->interrupt_pin - 1);
        if (route_find(&l, f, &route)) {
            routes[route_count++] = route;
            printf("PCI %s INT%c# is GSI %u, %s, active %s\n", text, pin, route.gsi,
                   route.level ? "level" : "edge", route.active_low ? "low" : "high");
        } else {
            printf("PCI %s INT%c# has no route\n", text, pin);
        }
    }

    for (uint32_t i = 0; i < l.bus_count; i++) {
        uacpi_free_pci_routing_table(l.buses[i].prt);
    }
    kfree(l.buses);
    kfree(l.host_bridges);
}

const struct acpi_irq_pci_route *acpi_irq_pci_routes(uint32_t *count) {
    if (!routes_done) {
        routes_done = true;
        routes_find();
    }
    *count = route_count;
    return routes;
}

static uacpi_iteration_decision claim_resource(void *user, uacpi_resource *resource) {
    uint16_t *claimed = user;
    if (resource->type == UACPI_RESOURCE_TYPE_IRQ) {
        for (unsigned i = 0; i < resource->irq.num_irqs; i++) {
            if (resource->irq.irqs[i] < ISA_IRQS) {
                *claimed |= (uint16_t)(1u << resource->irq.irqs[i]);
            }
        }
    } else if (resource->type == UACPI_RESOURCE_TYPE_EXTENDED_IRQ) {
        for (unsigned i = 0; i < resource->extended_irq.num_irqs; i++) {
            if (resource->extended_irq.irqs[i] < ISA_IRQS) {
                *claimed |= (uint16_t)(1u << resource->extended_irq.irqs[i]);
            }
        }
    }
    return UACPI_ITERATION_DECISION_CONTINUE;
}

static uacpi_iteration_decision claim_device(void *user, uacpi_namespace_node *node,
                                             uacpi_u32 depth) {
    (void)depth;
    if (uacpi_device_matches_pnp_id(node, link_ids)) {
        return UACPI_ITERATION_DECISION_CONTINUE;
    }
    uacpi_u32 sta;
    uacpi_status status = uacpi_eval_sta(node, &sta);
    if (status != UACPI_STATUS_OK) {
        log_failure(node, "_STA", status);
        return UACPI_ITERATION_DECISION_CONTINUE;
    }
    if ((sta & (STA_PRESENT | STA_ENABLED)) != (STA_PRESENT | STA_ENABLED)) {
        return UACPI_ITERATION_DECISION_CONTINUE;
    }

    uacpi_resources *resources;
    status = uacpi_get_current_resources(node, &resources);
    if (status != UACPI_STATUS_OK) {
        if (status != UACPI_STATUS_NOT_FOUND) {
            log_failure(node, "_CRS", status);
        }
        return UACPI_ITERATION_DECISION_CONTINUE;
    }
    uacpi_for_each_resource(resources, claim_resource, user);
    uacpi_free_resources(resources);
    return UACPI_ITERATION_DECISION_CONTINUE;
}

uint16_t acpi_irq_isa_claimed(void) {
    if (isa_claimed_done) {
        return isa_claimed;
    }
    isa_claimed_done = true;
    if (!namespace_loaded()) {
        printf("No ACPI namespace, so no ISA IRQs known to be in use\n");
        return isa_claimed;
    }
    uacpi_namespace_for_each_child(uacpi_namespace_root(), claim_device, NULL,
                                   UACPI_OBJECT_DEVICE_BIT, UACPI_MAX_DEPTH_ANY, &isa_claimed);

    char text[ISA_IRQS * 4 + 1];
    size_t length = 0;
    for (unsigned irq = 0; irq < ISA_IRQS; irq++) {
        if (isa_claimed & (1u << irq)) {
            int n = snprintf(text + length, sizeof(text) - length, "%s%u",
                             length == 0 ? "" : ", ", irq);
            length += (size_t)n;
        }
    }
    if (length == 0) {
        printf("ACPI devices use no ISA IRQs\n");
    } else {
        printf("ACPI devices use ISA IRQs %s\n", text);
    }
    return isa_claimed;
}
