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
#include <virt/pci_quirks.h>
#include <core.h>
#include <handoff.h>
#include <hw/pci_cfg.h>
#include <hw/pci_enum.h>
#include <hw/pci_regs.h>
#include <lib/heap.h>
#include <lib/log.h>
#include <lib/util.h>
#include <sys/mm.h>
#include <sys/x86.h>
#include <virt/io.h>
#include <virt/lpss_ids.h>
#include <virt/mmio.h>
#include <virt/pci_virt.h>
#include <virt/vcpu.h>

// NVIDIA's GPUs copy their configuration space into BAR0: its first 256 bytes
// at 1800h on the NV04 family, all of it at 88000h from the NV40 family on
// (Linux 7.3's nouveau, drivers/gpu/drm/nouveau/nvkm/subdev/pci/nv04.c and
// nv40.c). The older copy also answers through the VGA ports, after 338h, the
// offset at 3D0h, then 538h to read or 738h to write go to 3D4h, and through
// the address and data registers at 8 and Ch of the I/O BAR 5, while the
// registers at 0 and 4 have their bit 0 set.
#define NVIDIA_MIRROR_OLD 0x1800
#define NVIDIA_MIRROR 0x88000
#define NVIDIA_VGA_DATA 0x3d0
#define NVIDIA_VGA_INDEX 0x3d4
#define NVIDIA_VGA_SELECT 0x338
#define NVIDIA_VGA_READ 0x538
#define NVIDIA_VGA_WRITE 0x738
#define NVIDIA_WINDOW_BAR 5
#define NVIDIA_WINDOW_MASTER 0x0
#define NVIDIA_WINDOW_ENABLE 0x4
#define NVIDIA_WINDOW_ADDRESS 0x8
#define NVIDIA_WINDOW_DATA 0xc
#define NVIDIA_WINDOW_PORTS 16

// AMD's GPUs copy their configuration space into their register BAR at 4000h,
// which the address and data registers at 0 and 4 of their I/O BAR 4 reach
// too.
#define ATI_MIRROR 0x4000
#define ATI_REGISTERS_BAR 2
#define ATI_WINDOW_BAR 4
#define ATI_WINDOW_ADDRESS 0x0
#define ATI_WINDOW_DATA 0x4
#define ATI_WINDOW_PORTS 8

// The RTL8168's ERI registers in its BAR 2, through which Windows' driver
// writes the MSI-X table: the data at ERIDR, then the address at ERIAR with
// ERIAR_MSIX as its type and ERIAR_FLAG to write, which reads back flipped
// once done (Linux 7.3's drivers/net/ethernet/realtek/r8169_main.c).
#define RTL8168_DEVICE_ID 0x8168
#define RTL8168_BAR 2
#define RTL8168_ERIDR 0x70
#define RTL8168_ERIAR 0x74
#define RTL8168_ERIAR_FLAG ((uint32_t)0x80000000)
#define RTL8168_ERIAR_TYPE ((uint32_t)0x7fff0000)
#define RTL8168_ERIAR_MSIX ((uint32_t)0x00010000)
#define RTL8168_ERIAR_OFFSET ((uint32_t)0x00000fff)

// Intel's LPSS functions keep the address of their BAR 0 in their remap
// address registers, which must hold it (Intel 600 and 700 Series PCH
// datasheet 680836-005, Vol. 2, 13.3.13 and 13.3.14), and which their drivers
// fill in with the address they see (Linux 7.3's drivers/mfd/intel-lpss.c,
// intel_lpss_set_remap_addr()): the host's goes there, while the guest reads
// back what it wrote, or the registers' default of 0 before it writes.
#define LPSS_BAR 0
#define LPSS_REMAP_ADDR 0x240
#define LPSS_REMAP_SIZE 8

// A copy of the configuration space in a memory BAR, the rest of the pages it
// is on the device's.
struct mirror {
    struct pci_cfg_addr addr;
    // Where the trapped pages start in the BAR, and on the host.
    uint64_t page_offset;
    uintptr_t host;
    uint64_t start;
    uint64_t size;
    // NVIDIA's, whose driver acknowledges an MSI by writing to the read-only
    // ID and next registers of the MSI capability through the copy, which the
    // device must see.
    uint8_t msi_cap;
};

// An address and data register pair that reaches a copy of the configuration
// space where the address falls in it: matches, under masks, give where.
#define WINDOW_MATCHES 2

struct window {
    struct pci_cfg_addr addr;
    uint16_t port;
    uint16_t address_offset;
    uint16_t data_offset;
    uint32_t match[WINDOW_MATCHES];
    uint32_t mask[WINDOW_MATCHES];
    size_t match_count;
    // NVIDIA's window answers only while its registers at 0 and 4 enable it.
    bool gated;
    uint32_t master;
    uint32_t enable;
    // Where the address register points into the configuration space, if it
    // does.
    bool open;
    uint32_t config;
};

// Where the VGA ports' sequence has got to.
#define NVIDIA_VGA_NONE 0
#define NVIDIA_VGA_SELECTED 1
#define NVIDIA_VGA_WINDOW 2
#define NVIDIA_VGA_READING 3
#define NVIDIA_VGA_WRITING 4

struct nvidia_vga {
    struct pci_cfg_addr addr;
    unsigned int state;
    uint32_t offset;
};

struct rtl8168 {
    struct pci_cfg_addr addr;
    uintptr_t host;
    bool msix;
    uint32_t address;
    uint32_t data;
};

struct lpss {
    uintptr_t host;
    // The remap address as the guest wrote it.
    uint64_t remap;
};

static uint64_t config_read(struct pci_cfg_addr addr, uint32_t offset, size_t size) {
    if (size == 8) {
        return pci_virt_config_read(addr, offset, 4)
             | (uint64_t)pci_virt_config_read(addr, offset + 4, 4) << 32;
    }
    return pci_virt_config_read(addr, offset, size);
}

static void config_write(struct pci_cfg_addr addr, uint32_t offset, size_t size, uint64_t value) {
    if (size == 8) {
        pci_virt_config_write(addr, offset, 4, (uint32_t)value);
        pci_virt_config_write(addr, offset + 4, 4, (uint32_t)(value >> 32));
        return;
    }
    pci_virt_config_write(addr, offset, size, (uint32_t)value);
}

static bool within(uint64_t at, size_t size, uint64_t start, uint64_t length) {
    return at >= start && at + size <= start + length;
}

static uint64_t mirror_read(struct vcpu *v, void *ctx, uint64_t offset, size_t size) {
    (void)v;
    const struct mirror *m = ctx;
    uint64_t at = m->page_offset + offset;
    // Read anyway, as the device may count on it.
    uint64_t value = mmio_read_host(m->host + offset, size);
    if (within(at, size, m->start, m->size)) {
        value = config_read(m->addr, (uint32_t)(at - m->start), size);
    }
    return value;
}

static void mirror_write(struct vcpu *v, void *ctx, uint64_t offset, size_t size, uint64_t value) {
    (void)v;
    const struct mirror *m = ctx;
    uint64_t at = m->page_offset + offset;
    if (!within(at, size, m->start, m->size)) {
        mmio_write_host(m->host + offset, size, value);
        return;
    }
    uint32_t config = (uint32_t)(at - m->start);
    config_write(m->addr, config, size, value);
    if (m->msi_cap != 0 && within(config, size, m->msi_cap, PCI_MSI_FLAGS)) {
        mmio_write_host(m->host + offset, size, value);
    }
}

static const struct mmio_ops mirror_ops = { .read = mirror_read, .write = mirror_write };

static void mirror_add(
    const struct pci_enum_function *f,
    unsigned int bar,
    uint64_t start,
    uint64_t size,
    bool nvidia
) {
    const struct pci_enum_bar *b = &f->bars[bar];
    if ((b->kind != PCI_ENUM_BAR_MEM32 && b->kind != PCI_ENUM_BAR_MEM64) || b->base == 0
     || start + size > b->size) {
        return;
    }
    struct mirror *m = kmalloc(sizeof(struct mirror));
    *m = (struct mirror){
        .addr = f->addr,
        .page_offset = page_down(start),
        .host = b->base + page_down(start),
        .start = start,
        .size = size,
        .msi_cap = nvidia ? f->msi_cap : 0
    };
    mm_map_device(m->host, page_up(start + size) - page_down(start));
    if (!pci_virt_trap(f->addr, bar, start, size, &mirror_ops, m)) {
        printf(
            "PCI %02x:%02x.%x: the copy of its configuration space at BAR%u+%#llx stays the "
            "host's\n", f->addr.bus, f->addr.device, f->addr.function, bar,
            (unsigned long long)start
        );
    }
}

static uint32_t port_in(uint16_t port, size_t size) {
    switch (size) {
        case 1: {
            return inb(port);
        }
        case 2: {
            return inw(port);
        }
        default: {
            return inl(port);
        }
    }
}

static void port_out(uint16_t port, size_t size, uint32_t value) {
    switch (size) {
        case 1: {
            outb(port, (uint8_t)value);
            break;
        }
        case 2: {
            outw(port, (uint16_t)value);
            break;
        }
        default: {
            outl(port, value);
            break;
        }
    }
}

static void window_access(
    struct vcpu *v,
    void *ctx,
    uint16_t offset,
    size_t size,
    bool in,
    uint32_t *value
) {
    (void)v;
    struct window *w = ctx;
    uint16_t port = (uint16_t)(w->port + offset);
    bool live = !w->gated || ((w->master & w->enable) & 1) != 0;
    if (live && offset == w->data_offset && size == 4 && w->open) {
        if (in) {
            port_in(port, size);
            *value = pci_virt_config_read(w->addr, w->config, size);
        } else {
            pci_virt_config_write(w->addr, w->config, size, *value);
        }
        return;
    }
    if (in) {
        *value = port_in(port, size);
        return;
    }
    port_out(port, size, *value);
    if (w->gated && offset == NVIDIA_WINDOW_MASTER && size == 4) {
        w->master = *value;
    } else if (w->gated && offset == NVIDIA_WINDOW_ENABLE && size == 4) {
        w->enable = *value;
    } else if (live && offset == w->address_offset) {
        w->open = false;
        for (size_t i = 0; i < w->match_count && size == 4; i++) {
            if ((*value & ~w->mask[i]) == w->match[i]) {
                w->open = true;
                w->config = *value & w->mask[i];
                break;
            }
        }
    }
}

static void window_add(
    const struct pci_enum_function *f,
    unsigned int bar,
    uint16_t address,
    uint16_t data,
    uint16_t ports,
    bool gated
) {
    const struct pci_enum_bar *b = &f->bars[bar];
    if (b->kind != PCI_ENUM_BAR_IO || b->base == 0 || b->size < ports) {
        return;
    }
    uint32_t config_mask = (f->pcie_cap != 0 ? PCI_CFG_SPACE_EXP_SIZE : PCI_CFG_SPACE_SIZE) - 1;
    struct window *w = kmalloc(sizeof(struct window));
    *w = (struct window){
        .addr = f->addr,
        .port = (uint16_t)b->base,
        .address_offset = address,
        .data_offset = data,
        .gated = gated
    };
    if (gated) {
        w->match[0] = NVIDIA_MIRROR_OLD;
        w->mask[0] = PCI_CFG_SPACE_SIZE - 1;
        w->match[1] = NVIDIA_MIRROR;
        w->mask[1] = config_mask;
        w->match_count = 2;
    } else {
        w->match[0] = ATI_MIRROR;
        w->mask[0] = config_mask;
        w->match_count = 1;
    }
    struct io_handler handler = {
        .base = w->port, .count = ports, .access = window_access, .ctx = w
    };
    io_register(&handler);
}

static void nvidia_vga_access(
    struct vcpu *v,
    void *ctx,
    uint16_t offset,
    size_t size,
    bool in,
    uint32_t *value
) {
    (void)v;
    struct nvidia_vga *n = ctx;
    uint16_t port = (uint16_t)(NVIDIA_VGA_DATA + offset);
    bool index = port >= NVIDIA_VGA_INDEX;
    // The ports between the data and index pairs are no part of it.
    if (port >= NVIDIA_VGA_DATA + 2 && port < NVIDIA_VGA_INDEX) {
        if (in) {
            *value = port_in(port, size);
        } else {
            port_out(port, size, *value);
        }
        return;
    }
    unsigned int was = n->state;
    n->state = NVIDIA_VGA_NONE;
    bool mirrored = (n->offset & ~(uint32_t)(PCI_CFG_SPACE_SIZE - 1)) == NVIDIA_MIRROR_OLD;
    uint32_t config = n->offset & (PCI_CFG_SPACE_SIZE - 1);
    if (in) {
        *value = port_in(port, size);
        if (!index && was == NVIDIA_VGA_READING && mirrored) {
            *value = pci_virt_config_read(n->addr, config, size);
        }
        return;
    }
    if (index) {
        if (*value == NVIDIA_VGA_SELECT && was == NVIDIA_VGA_NONE) {
            n->state = NVIDIA_VGA_SELECTED;
        } else if (*value == NVIDIA_VGA_READ && was == NVIDIA_VGA_WINDOW) {
            n->state = NVIDIA_VGA_READING;
        } else if (*value == NVIDIA_VGA_WRITE && was == NVIDIA_VGA_WINDOW) {
            n->state = NVIDIA_VGA_WRITING;
        }
    } else if (was == NVIDIA_VGA_SELECTED) {
        n->offset = *value;
        n->state = NVIDIA_VGA_WINDOW;
    } else if (was == NVIDIA_VGA_WRITING && mirrored) {
        pci_virt_config_write(n->addr, config, size, *value);
        return;
    }
    port_out(port, size, *value);
}

static void nvidia_vga_add(const struct pci_enum_function *f) {
    struct nvidia_vga *n = kmalloc(sizeof(struct nvidia_vga));
    *n = (struct nvidia_vga){ .addr = f->addr, .state = NVIDIA_VGA_NONE };
    struct io_handler handler = {
        .base = NVIDIA_VGA_DATA, .count = NVIDIA_VGA_INDEX + 2 - NVIDIA_VGA_DATA,
        .access = nvidia_vga_access, .ctx = n
    };
    io_register(&handler);
}

static uint64_t rtl8168_read(struct vcpu *v, void *ctx, uint64_t offset, size_t size) {
    (void)v;
    const struct rtl8168 *r = ctx;
    uint64_t value = mmio_read_host(r->host + offset, size);
    if (r->msix && size == 4 && offset == RTL8168_ERIAR) {
        value = r->address ^ RTL8168_ERIAR_FLAG;
    } else if (r->msix && size == 4 && offset == RTL8168_ERIDR) {
        uint64_t entry;
        if (pci_virt_msix_access(r->addr, r->address & RTL8168_ERIAR_OFFSET, 4, false, &entry)) {
            value = entry;
        }
    }
    return value;
}

static void rtl8168_write(
    struct vcpu *v,
    void *ctx,
    uint64_t offset,
    size_t size,
    uint64_t value
) {
    (void)v;
    struct rtl8168 *r = ctx;
    if (size == 4 && offset == RTL8168_ERIDR) {
        r->data = (uint32_t)value;
    } else if (size == 4 && offset == RTL8168_ERIAR) {
        r->msix = ((uint32_t)value & RTL8168_ERIAR_TYPE) == RTL8168_ERIAR_MSIX;
        r->address = (uint32_t)value;
        if (r->msix && ((uint32_t)value & RTL8168_ERIAR_FLAG) != 0) {
            uint64_t entry = r->data;
            pci_virt_msix_access(r->addr, value & RTL8168_ERIAR_OFFSET, 4, true, &entry);
            return;
        }
    }
    mmio_write_host(r->host + offset, size, value);
}

static const struct mmio_ops rtl8168_ops = { .read = rtl8168_read, .write = rtl8168_write };

static void rtl8168_add(const struct pci_enum_function *f) {
    const struct pci_enum_bar *b = &f->bars[RTL8168_BAR];
    if (f->msix_cap == 0 || (b->kind != PCI_ENUM_BAR_MEM32 && b->kind != PCI_ENUM_BAR_MEM64)
     || b->base == 0 || b->size < RTL8168_ERIAR + 4) {
        return;
    }
    struct rtl8168 *r = kmalloc(sizeof(struct rtl8168));
    *r = (struct rtl8168){ .addr = f->addr, .host = b->base };
    mm_map_device(r->host, PAGE_SIZE);
    if (!pci_virt_trap(f->addr, RTL8168_BAR, RTL8168_ERIDR, 8, &rtl8168_ops, r)) {
        printf(
            "PCI %02x:%02x.%x: its MSI-X table stays reachable through its ERI registers\n",
            f->addr.bus, f->addr.device, f->addr.function
        );
    }
}

// The bits of an access of size bytes, at the bottom.
static uint64_t size_mask(size_t size) {
    return size == 8 ? UINT64_MAX : ((uint64_t)1 << size * 8) - 1;
}

static uint64_t lpss_read(struct vcpu *v, void *ctx, uint64_t offset, size_t size) {
    (void)v;
    const struct lpss *l = ctx;
    if (!within(offset, size, LPSS_REMAP_ADDR, LPSS_REMAP_SIZE)) {
        return mmio_read_host(l->host + offset, size);
    }
    return l->remap >> (offset - LPSS_REMAP_ADDR) * 8 & size_mask(size);
}

static void lpss_write(struct vcpu *v, void *ctx, uint64_t offset, size_t size, uint64_t value) {
    (void)v;
    struct lpss *l = ctx;
    if (!within(offset, size, LPSS_REMAP_ADDR, LPSS_REMAP_SIZE)) {
        mmio_write_host(l->host + offset, size, value);
        return;
    }
    size_t shift = (offset - LPSS_REMAP_ADDR) * 8;
    uint64_t mask = size_mask(size) << shift;
    l->remap = (l->remap & ~mask) | (value << shift & mask);
    mmio_write_host(l->host + LPSS_REMAP_ADDR, 4, (uint32_t)l->host);
    mmio_write_host(l->host + LPSS_REMAP_ADDR + 4, 4, (uint32_t)(l->host >> 32));
}

static const struct mmio_ops lpss_ops = { .read = lpss_read, .write = lpss_write };

static bool lpss_function(const struct pci_enum_function *f) {
    if (f->vendor_id != PCI_VENDOR_ID_INTEL) {
        return false;
    }
    for (size_t i = 0; i < lpss_ids_count; i++) {
        if (lpss_ids[i] == f->device_id) {
            return true;
        }
    }
    return false;
}

static void lpss_add(const struct pci_enum_function *f) {
    const struct pci_enum_bar *b = &f->bars[LPSS_BAR];
    if ((b->kind != PCI_ENUM_BAR_MEM32 && b->kind != PCI_ENUM_BAR_MEM64) || b->base == 0
     || b->size < LPSS_REMAP_ADDR + LPSS_REMAP_SIZE) {
        return;
    }
    struct lpss *l = kmalloc(sizeof(struct lpss));
    *l = (struct lpss){ .host = b->base };
    mm_map_device(l->host, PAGE_SIZE);
    if (!pci_virt_trap(f->addr, LPSS_BAR, LPSS_REMAP_ADDR, LPSS_REMAP_SIZE, &lpss_ops, l)) {
        printf(
            "PCI %02x:%02x.%x: its remap address holds what the guest writes there\n",
            f->addr.bus, f->addr.device, f->addr.function
        );
    }
}

void pci_quirks_init(void) {
    const struct pci_enum_topology *t = pci_enum_topology();
    const struct handoff_video *video = &core_handoff->video;
    for (size_t i = 0; t != NULL && i < t->function_count; i++) {
        const struct pci_enum_function *f = &t->functions[i];
        // The GPU whose VBIOS runs is the one with the VGA ports, which its
        // VBIOS reaches the copies through.
        bool vga = video->vbios_present && f->addr.segment == video->vga_device.segment
                && f->addr.bus == video->vga_device.bus
                && (uint32_t)(f->addr.device << 3 | f->addr.function) == video->vga_device.devfn;
        uint32_t config_size = f->pcie_cap != 0 ? PCI_CFG_SPACE_EXP_SIZE : PCI_CFG_SPACE_SIZE;
        if (f->vendor_id == PCI_VENDOR_ID_NVIDIA && f->base_class == PCI_BASE_CLASS_DISPLAY) {
            mirror_add(f, 0, NVIDIA_MIRROR, config_size, true);
            if (vga) {
                mirror_add(f, 0, NVIDIA_MIRROR_OLD, PCI_CFG_SPACE_SIZE, true);
                window_add(
                    f, NVIDIA_WINDOW_BAR, NVIDIA_WINDOW_ADDRESS, NVIDIA_WINDOW_DATA,
                    NVIDIA_WINDOW_PORTS, true
                );
                nvidia_vga_add(f);
            }
        } else if (f->vendor_id == PCI_VENDOR_ID_ATI && f->base_class == PCI_BASE_CLASS_DISPLAY
                && vga) {
            // Where the registers are in BAR 5, BAR 2 holds doorbells instead.
            const struct pci_enum_bar *registers = &f->bars[5];
            if (f->bars[ATI_REGISTERS_BAR].kind == PCI_ENUM_BAR_MEM64
             && registers->kind != PCI_ENUM_BAR_MEM32 && registers->kind != PCI_ENUM_BAR_MEM64) {
                mirror_add(f, ATI_REGISTERS_BAR, ATI_MIRROR, PCI_CFG_SPACE_SIZE, false);
            }
            window_add(
                f, ATI_WINDOW_BAR, ATI_WINDOW_ADDRESS, ATI_WINDOW_DATA, ATI_WINDOW_PORTS, false
            );
        } else if (f->vendor_id == PCI_VENDOR_ID_REALTEK && f->device_id == RTL8168_DEVICE_ID) {
            rtl8168_add(f);
        } else if (lpss_function(f)) {
            lpss_add(f);
        }
    }
}
