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
#include <virt/pci_port.h>
#include <core.h>
#include <handoff.h>
#include <hw/pci_cfg.h>
#include <hw/pci_enum.h>
#include <hw/pci_regs.h>
#include <lib/heap.h>
#include <memory.h>
#include <virt/pci_plan.h>

// A PCI-to-PCI bridge's header, PCI-to-PCI Bridge Architecture Specification
// 1.1, 3.2, and the capabilities' registers as Linux 7.3's
// include/uapi/linux/pci_regs.h lays them out.
#define CONFIG_SIZE 256

#define CLASS_BRIDGE_PCI ((uint32_t)0x060400)
// PCI-to-PCI Bridge Architecture Specification 1.1, 4.6: class code 060401h,
// subtractive decode.
#define CLASS_SUBTRACTIVE ((uint32_t)0x01)
// I/O, memory, bus master, parity error response, SERR#, INTx disable.
#define COMMAND_WRITABLE 0x0547
// Parity error response, SERR#, ISA, VGA, VGA 16-bit decode, secondary bus
// reset.
#define BRIDGE_CONTROL_WRITABLE 0x005f
#define IO_WINDOW 0xf0
#define MEMORY_WINDOW 0xfff0

#define CAP_PCIE 0x40
#define CAP_PM 0x80
// PCI_EXP_FLAGS: version 2, the upstream port of a switch, the one kind of
// bridge below which an OS looks for every device and reaches their extended
// configuration space: it looks for device 0 alone below a root or downstream
// port (Linux 7.3's only_one_child() in drivers/pci/probe.c), and for the
// space only below a port (its pci_bridge_child_ext_cfg_accessible()).
#define PCIE_FLAGS ((uint32_t)0x0052)
#define PCIE_DEVCAP 0x04
#define PCIE_DEVCTL 0x08
#define PCIE_LNKCAP 0x0c
#define PCIE_LNKCTL 0x10
#define PCIE_LNKSTA 0x12
#define PCIE_LNKCAP2 0x2c
#define PCIE_LNKCTL2 0x30
#define DEVCAP_PAYLOAD ((uint32_t)0x7)
#define DEVCAP_RBER ((uint32_t)0x8000)
#define DEVCTL_RELAX_EN ((uint32_t)0x0010)
#define DEVCTL_PAYLOAD_SHIFT 5
#define DEVCTL_PAYLOAD ((uint32_t)0x00e0)
#define DEVCTL_NOSNOOP_EN ((uint32_t)0x0800)
#define DEVCTL_READRQ_512B ((uint32_t)0x2000)
#define DEVCTL_WRITABLE 0x7fff
#define LNKCAP_SLS ((uint32_t)0xf)
#define LNKCAP_MLW_SHIFT 4
#define LNKCAP_MLW ((uint32_t)0x3f0)
#define LNKCAP_ASPM_OPTCOMP ((uint32_t)0x00400000)
// ASPM control, common clock, extended synch, autonomous width disable.
#define LNKCTL_WRITABLE 0x02c3
#define LNKCTL2_WRITABLE 0x007f
#define PM_PMC 2
#define PM_CTRL 4
// PCI Power Management Interface Specification 1.2.
#define PMC_VERSION ((uint32_t)3)
#define PM_CTRL_STATE 0x03
#define PM_CTRL_NO_SOFT_RESET ((uint32_t)0x0008)
#define PM_D3HOT 3

// No device of Intel's or AMD's has it in the PCI ID Repository's list of
// 2026-10-01, nor does a Linux 7.3 quirk match it.
#define PORT_DEVICE_ID 0x5242

struct port {
    uint8_t config[CONFIG_SIZE];
    uint8_t writable[CONFIG_SIZE];
};

static struct port *ports;
static size_t port_count;

static void put(uint8_t *bytes, size_t offset, size_t size, uint32_t value) {
    for (size_t i = 0; i < size; i++) {
        bytes[offset + i] = (uint8_t)(value >> (8 * i));
    }
}

static uint32_t window_word(uintptr_t address) {
    return (uint32_t)(address >> 16) & MEMORY_WINDOW;
}

static size_t root_of(const struct pci_enum_topology *t, const struct pci_enum_function *f) {
    while (f->parent != NULL) {
        f = f->parent;
    }
    for (size_t r = 0; r < t->root_bus_count; r++) {
        if (t->root_buses[r].segment == f->addr.segment && t->root_buses[r].bus == f->addr.bus) {
            return r;
        }
    }
    return SIZE_MAX;
}

// What the PCI Express functions on the port's bus have, so that it asks
// nothing of them that they do not do already: the smallest payload size they
// have set, the largest they take, and their fastest and widest links.
struct below {
    uint32_t payload;
    uint32_t payload_max;
    uint32_t speed;
    uint32_t width;
};

static struct below below_port(
    const struct pci_enum_topology *t, const struct pci_enum_root_bus *rb
) {
    struct below b = { DEVCAP_PAYLOAD, 0, 1, 1 };
    bool any = false;
    for (size_t i = 0; i < t->function_count; i++) {
        const struct pci_enum_function *f = &t->functions[i];
        if (f->addr.segment != rb->segment || f->addr.bus != rb->bus || f->pcie_cap == 0) {
            continue;
        }
        uint32_t devcap = pci_cfg_read(f->addr, f->pcie_cap + PCIE_DEVCAP, 4);
        uint32_t devctl = pci_cfg_read(f->addr, f->pcie_cap + PCIE_DEVCTL, 2);
        uint32_t lnkcap = pci_cfg_read(f->addr, f->pcie_cap + PCIE_LNKCAP, 4);
        uint32_t payload = (devctl & DEVCTL_PAYLOAD) >> DEVCTL_PAYLOAD_SHIFT;
        if (payload < b.payload) {
            b.payload = payload;
        }
        if ((devcap & DEVCAP_PAYLOAD) > b.payload_max) {
            b.payload_max = devcap & DEVCAP_PAYLOAD;
        }
        if ((lnkcap & LNKCAP_SLS) > b.speed) {
            b.speed = lnkcap & LNKCAP_SLS;
        }
        if ((lnkcap & LNKCAP_MLW) >> LNKCAP_MLW_SHIFT > b.width) {
            b.width = (lnkcap & LNKCAP_MLW) >> LNKCAP_MLW_SHIFT;
        }
        any = true;
    }
    if (!any) {
        b.payload = 0;
    }
    if (b.payload_max < b.payload) {
        b.payload_max = b.payload;
    }
    return b;
}

// Whether the display's VGA device is behind the port, and the VGA 16-bit
// decode of the bridge above it there, where it has one.
static bool vga_below(const struct pci_enum_topology *t, size_t root, bool *decode_16) {
    const struct handoff_video *video = &core_handoff->video;
    *decode_16 = true;
    if (!video->vga_present) {
        return false;
    }
    struct pci_cfg_addr a = {
        video->vga_device.segment, video->vga_device.bus, (uint8_t)(video->vga_device.devfn >> 3),
        (uint8_t)(video->vga_device.devfn & 7)
    };
    const struct pci_enum_function *f = pci_enum_find(a);
    if (f == NULL || root_of(t, f) != root) {
        return false;
    }
    if (f->parent != NULL) {
        *decode_16 = (f->parent->bridge_control & PCI_BRIDGE_CTL_VGA_16BIT) != 0;
    }
    return true;
}

// Whether an ISA bridge is behind the port, whose devices' legacy addresses no
// window of the port has: it then forwards what nothing else on bus 0 claims,
// as the host bridge in its place does.
static bool isa_below(const struct pci_enum_topology *t, size_t root) {
    for (size_t i = 0; i < t->function_count; i++) {
        const struct pci_enum_function *f = &t->functions[i];
        if (f->base_class == PCI_BASE_CLASS_BRIDGE && f->subclass == PCI_SUBCLASS_BRIDGE_ISA
         && root_of(t, f) == root) {
            return true;
        }
    }
    return false;
}

static uint16_t vendor_id(const struct pci_enum_topology *t, size_t primary) {
    const struct pci_enum_root_bus *rb = &t->root_buses[primary];
    for (size_t i = 0; i < t->function_count; i++) {
        const struct pci_enum_function *f = &t->functions[i];
        if (f->addr.segment == rb->segment && f->addr.bus == rb->bus) {
            return f->vendor_id;
        }
    }
    return 0x8086;
}

static void port_build(
    struct port *p, const struct pci_plan_port *pp, bool multi, uint16_t vendor
) {
    const struct pci_enum_topology *t = pci_enum_topology();
    uint8_t *c = p->config;
    uint8_t *w = p->writable;
    memset(p, 0, sizeof(struct port));

    put(c, PCI_VENDOR_ID, 2, vendor);
    put(c, PCI_DEVICE_ID, 2, PORT_DEVICE_ID);
    put(c, PCI_STATUS, 2, PCI_STATUS_CAP_LIST);
    uint32_t class = CLASS_BRIDGE_PCI | (isa_below(t, pp->root) ? CLASS_SUBTRACTIVE : 0);
    put(c, PCI_CLASS_REVISION, 4, class << 8);
    put(c, PCI_HEADER_TYPE, 1, PCI_HEADER_TYPE_BRIDGE | (multi ? PCI_HEADER_TYPE_MFD : 0));
    put(c, PCI_PRIMARY_BUS, 3, (uint32_t)pp->secondary << 8 | (uint32_t)pp->subordinate << 16);
    put(c, PCI_IO_BASE, 1, (pp->io_base >> 8) & IO_WINDOW);
    put(c, PCI_IO_LIMIT, 1, (pp->io_limit >> 8) & IO_WINDOW);
    put(c, PCI_MEMORY_BASE, 2, window_word(pp->mem_base));
    put(c, PCI_MEMORY_LIMIT, 2, window_word(pp->mem_limit));
    put(c, PCI_PREF_MEMORY_BASE, 2, window_word(pp->pref_base) | PCI_PREF_RANGE_TYPE_64);
    put(c, PCI_PREF_MEMORY_LIMIT, 2, window_word(pp->pref_limit) | PCI_PREF_RANGE_TYPE_64);
    put(c, PCI_PREF_BASE_UPPER32, 4, (uint32_t)(pp->pref_base >> 32));
    put(c, PCI_PREF_LIMIT_UPPER32, 4, (uint32_t)(pp->pref_limit >> 32));
    put(c, PCI_CAPABILITY_LIST, 1, CAP_PCIE);
    bool decode_16;
    if (vga_below(t, pp->root, &decode_16)) {
        put(
            c, PCI_BRIDGE_CONTROL, 2,
            PCI_BRIDGE_CTL_VGA | (decode_16 ? PCI_BRIDGE_CTL_VGA_16BIT : 0)
        );
    }

    struct below b = below_port(t, &t->root_buses[pp->root]);
    put(c, CAP_PCIE, 1, PCI_CAP_ID_EXP);
    put(c, CAP_PCIE + 1, 1, CAP_PM);
    put(c, CAP_PCIE + 2, 2, PCIE_FLAGS);
    put(c, CAP_PCIE + PCIE_DEVCAP, 4, b.payload_max | DEVCAP_RBER);
    put(
        c, CAP_PCIE + PCIE_DEVCTL, 2,
        DEVCTL_RELAX_EN | b.payload << DEVCTL_PAYLOAD_SHIFT | DEVCTL_NOSNOOP_EN
      | DEVCTL_READRQ_512B
    );
    put(c, CAP_PCIE + PCIE_LNKCAP, 4, b.speed | b.width << LNKCAP_MLW_SHIFT | LNKCAP_ASPM_OPTCOMP);
    put(c, CAP_PCIE + PCIE_LNKSTA, 2, b.speed | b.width << LNKCAP_MLW_SHIFT);
    put(c, CAP_PCIE + PCIE_LNKCAP2, 4, (((uint32_t)1 << b.speed) - 1) << 1);
    put(c, CAP_PCIE + PCIE_LNKCTL2, 2, b.speed);
    put(c, CAP_PM, 1, PCI_CAP_ID_PM);
    put(c, CAP_PM + PM_PMC, 2, PMC_VERSION);
    put(c, CAP_PM + PM_CTRL, 2, PM_CTRL_NO_SOFT_RESET);

    put(w, PCI_COMMAND, 2, COMMAND_WRITABLE);
    put(w, PCI_CACHE_LINE_SIZE, 1, 0xff);
    put(w, PCI_PRIMARY_BUS, 3, 0xffffff);
    put(w, PCI_IO_BASE, 1, IO_WINDOW);
    put(w, PCI_IO_LIMIT, 1, IO_WINDOW);
    put(w, PCI_MEMORY_BASE, 2, MEMORY_WINDOW);
    put(w, PCI_MEMORY_LIMIT, 2, MEMORY_WINDOW);
    put(w, PCI_PREF_MEMORY_BASE, 2, MEMORY_WINDOW);
    put(w, PCI_PREF_MEMORY_LIMIT, 2, MEMORY_WINDOW);
    put(w, PCI_PREF_BASE_UPPER32, 4, 0xffffffff);
    put(w, PCI_PREF_LIMIT_UPPER32, 4, 0xffffffff);
    put(w, PCI_INTERRUPT_LINE, 1, 0xff);
    put(w, PCI_BRIDGE_CONTROL, 2, BRIDGE_CONTROL_WRITABLE);
    put(w, CAP_PCIE + PCIE_DEVCTL, 2, DEVCTL_WRITABLE);
    put(w, CAP_PCIE + PCIE_LNKCTL, 2, LNKCTL_WRITABLE);
    put(w, CAP_PCIE + PCIE_LNKCTL2, 2, LNKCTL2_WRITABLE);
    put(w, CAP_PM + PM_CTRL, 1, PM_CTRL_STATE);
}

void pci_port_init(void) {
    const struct pci_plan *plan = pci_plan();
    const struct pci_enum_topology *t = pci_enum_topology();
    if (plan == NULL || t == NULL || plan->port_count == 0) {
        return;
    }
    port_count = plan->port_count;
    ports = kmalloc(port_count * sizeof(struct port));
    uint16_t vendor = vendor_id(t, plan->primary);
    for (size_t k = 0; k < port_count; k++) {
        const struct pci_plan_port *pp = &plan->ports[k];
        bool multi = false;
        for (size_t j = 0; j < port_count && pp->function == 0; j++) {
            multi |= plan->ports[j].device == pp->device && plan->ports[j].function != 0;
        }
        port_build(&ports[k], pp, multi, vendor);
    }
}

int pci_port_at(uint8_t device, uint8_t function) {
    const struct pci_plan *plan = pci_plan();
    for (size_t k = 0; k < port_count; k++) {
        if (plan->ports[k].device == device && plan->ports[k].function == function) {
            return (int)k;
        }
    }
    return -1;
}

// Its extended configuration space has no capability, which a header of 0 at
// its start says.
uint32_t pci_port_read(size_t port, uint32_t offset, size_t size) {
    uint32_t value = 0;
    for (size_t i = 0; i < size; i++) {
        uint32_t at = offset + (uint32_t)i;
        uint8_t byte = at < CONFIG_SIZE ? ports[port].config[at] : 0;
        value |= (uint32_t)byte << (8 * i);
    }
    return value;
}

void pci_port_write(size_t port, uint32_t offset, size_t size, uint32_t value) {
    struct port *p = &ports[port];
    for (size_t i = 0; i < size; i++) {
        uint32_t at = offset + (uint32_t)i;
        uint8_t byte = (uint8_t)(value >> (8 * i));
        if (at >= CONFIG_SIZE) {
            break;
        }
        // It has D0 and D3hot only, and a write of another state changes
        // nothing: PCI Power Management Interface Specification 1.2, 3.2.4.
        uint8_t state = byte & PM_CTRL_STATE;
        if (at == CAP_PM + PM_CTRL && state != 0 && state != PM_D3HOT) {
            continue;
        }
        p->config[at] = (uint8_t)((p->config[at] & ~p->writable[at]) | (byte & p->writable[at]));
    }
}
