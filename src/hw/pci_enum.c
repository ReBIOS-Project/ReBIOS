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

#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <uacpi/acpi.h>
#include <uacpi/status.h>
#include <uacpi/tables.h>
#include <hw/pci_enum.h>
#include <core.h>
#include <handoff.h>
#include <hw/acpi.h>
#include <hw/pci_cfg.h>
#include <hw/pci_regs.h>
#include <lib/heap.h>
#include <lib/log.h>
#include <lib/printf.h>
#include <memory.h>

#define CARDBUS_CAPABILITIES 0x14

#define BAR_IO ((uint32_t)1 << 0)
#define BAR_TYPE ((uint32_t)3 << 1)
#define BAR_TYPE_64 ((uint32_t)2 << 1)
#define BAR_PREFETCHABLE ((uint32_t)1 << 3)
#define BAR_IO_ADDRESS ((uint32_t)0xfffffffc)
#define BAR_MEMORY_ADDRESS ((uint32_t)0xfffffff0)
#define UPPER_32 ((uint64_t)0xffffffff00000000)
#define UPPER_48 ((uint64_t)0xffffffffffff0000)

#define WINDOW_WIDTH ((uint32_t)0xf)
#define WINDOW_IO_32 ((uint32_t)0x1)

#define EXT_CAP_FIRST 0x100
#define EXT_CAP_MAX_ENTRIES 512
#define EXT_CAP_SRIOV 0x0010
#define EXT_CAP_REBAR 0x0015

#define REBAR_CAPABILITY(n) (0x04 + 8 * (n))
#define REBAR_CONTROL(n) (0x08 + 8 * (n))
#define REBAR_MAX_PAIRS 6
#define REBAR_INDEX ((uint32_t)0x7)
#define REBAR_PAIRS_SHIFT 5
#define REBAR_PAIRS ((uint32_t)0x7)
#define REBAR_SIZE_SHIFT 8
#define REBAR_SIZE ((uint32_t)0x3f)
// Size encoding n means 2^(n + 20) bytes, up to 2^63.
#define REBAR_SIZE_BASE 20
#define REBAR_SIZE_LAST 43

#define LOG_LINE_MAX 180
#define LOG_ITEM_MAX 64

#define ADDR_FMT "%04x:%02x:%02x.%x"
#define ADDR_ARGS(a) (a).segment, (a).bus, (a).device, (a).function

struct segment_buses {
    uint16_t segment;
    uint64_t buses[4];
};

// A BAR's registers before and while it is sized.
struct bar_sample {
    uint8_t reg;
    uint8_t kind;
    bool prefetchable;
    bool rom;
    // A 64-bit BAR in the last slot, which has no upper register.
    bool malformed;
    uint32_t low;
    uint32_t high;
    uint64_t readback;
};

struct function_list {
    struct pci_enum_function *items;
    size_t count;
    size_t capacity;
};

struct log_line {
    char text[LOG_LINE_MAX + 1];
    size_t length;
    size_t prefix_length;
};

static struct pci_enum_topology topology;
static _Atomic(bool) scanned;

static void line_flush(struct log_line *line) {
    if (line->length == line->prefix_length) {
        return;
    }
    line->text[line->length] = '\0';
    printf("%s\n", line->text);
    line->length = line->prefix_length;
}

// Items that would take the line past LOG_LINE_MAX go on a new one, which
// repeats the prefix.
__attribute__((format(printf, 2, 0))) static void line_vadd(
    struct log_line *line, const char *fmt, va_list args
) {
    char item[LOG_ITEM_MAX];
    int n = vsnprintf(item, sizeof(item), fmt, args);
    if (n <= 0) {
        return;
    }
    size_t length = (size_t)n < sizeof(item) ? (size_t)n : sizeof(item) - 1;
    if (line->length + length > LOG_LINE_MAX && line->length > line->prefix_length) {
        line_flush(line);
    }
    if (length > LOG_LINE_MAX - line->length) {
        length = LOG_LINE_MAX - line->length;
    }
    memcpy(line->text + line->length, item, length);
    line->length += length;
}

__attribute__((format(printf, 2, 3))) static void line_start(
    struct log_line *line, const char *fmt, ...
) {
    line->length = 0;
    line->prefix_length = 0;
    va_list args;
    va_start(args, fmt);
    line_vadd(line, fmt, args);
    va_end(args);
    line->prefix_length = line->length;
}

__attribute__((format(printf, 2, 3))) static void line_add(
    struct log_line *line, const char *fmt, ...
) {
    va_list args;
    va_start(args, fmt);
    line_vadd(line, fmt, args);
    va_end(args);
}

static bool is_host_bridge(const struct pci_enum_function *f) {
    return f->base_class == PCI_BASE_CLASS_BRIDGE && f->subclass == PCI_SUBCLASS_BRIDGE_HOST;
}

// Clears the bits of the command register, and returns what decode_restore()
// needs to set them back. A host bridge is left decoding, as on some chipsets
// one that stops no longer routes the accesses the rest of the system needs.
static uint16_t decode_disable(const struct pci_enum_function *f, uint16_t bits) {
    if (is_host_bridge(f)) {
        return 0;
    }
    uint16_t command = (uint16_t)pci_cfg_read(f->addr, PCI_COMMAND, 2);
    if ((command & bits) != 0) {
        pci_cfg_write(f->addr, PCI_COMMAND, 2, (uint16_t)(command & ~bits));
    }
    return command;
}

static void decode_restore(const struct pci_enum_function *f, uint16_t command, uint16_t bits) {
    if ((command & bits) != 0) {
        pci_cfg_write(f->addr, PCI_COMMAND, 2, command);
    }
}

static void bar_sample_read(
    struct pci_cfg_addr addr, uint8_t reg, bool last_slot, struct bar_sample *s
) {
    *s = (struct bar_sample){ .reg = reg };
    s->low = pci_cfg_read(addr, reg, 4);
    if ((s->low & BAR_IO) != 0) {
        s->kind = PCI_ENUM_BAR_IO;
        return;
    }
    s->prefetchable = (s->low & BAR_PREFETCHABLE) != 0;
    if ((s->low & BAR_TYPE) != BAR_TYPE_64) {
        s->kind = PCI_ENUM_BAR_MEM32;
    } else if (last_slot) {
        s->kind = PCI_ENUM_BAR_NONE;
        s->malformed = true;
    } else {
        s->kind = PCI_ENUM_BAR_MEM64;
        s->high = pci_cfg_read(addr, reg + 4, 4);
    }
}

static void rom_sample_read(struct pci_cfg_addr addr, uint8_t reg, struct bar_sample *s) {
    *s = (struct bar_sample){ .reg = reg, .kind = PCI_ENUM_BAR_MEM32, .rom = true };
    s->low = pci_cfg_read(addr, reg, 4);
}

// Runs with the function's decoding off, except on a host bridge, which
// decodes the transient addresses: setting a 64-bit BAR's upper register first
// and restoring it last keeps those above 4 GiB. The expansion ROM BAR is
// written with its enable bit clear.
static void bar_sample_probe(struct pci_cfg_addr addr, struct bar_sample *s) {
    if (s->kind == PCI_ENUM_BAR_NONE) {
        return;
    }
    bool wide = s->kind == PCI_ENUM_BAR_MEM64;
    if (wide) {
        pci_cfg_write(addr, s->reg + 4, 4, 0xffffffff);
    }
    pci_cfg_write(addr, s->reg, 4, s->rom ? PCI_ROM_ADDRESS_MASK : 0xffffffff);
    s->readback = pci_cfg_read(addr, s->reg, 4);
    if (wide) {
        s->readback |= (uint64_t)pci_cfg_read(addr, s->reg + 4, 4) << 32;
    }
    pci_cfg_write(addr, s->reg, 4, s->low);
    if (wide) {
        pci_cfg_write(addr, s->reg + 4, 4, s->high);
    }
}

// The size is the two's complement of the read-back's address bits, given the
// bits above the register as ones. A device that leaves upper address bits
// unimplemented makes that meaningless, and the lowest bit it implements is
// the size instead. Returns false if so.
static bool bar_sample_result(const struct bar_sample *s, struct pci_enum_bar *bar) {
    *bar = (struct pci_enum_bar){ .reg = s->reg };
    uint64_t bits;
    uint64_t above;
    uintptr_t base;
    switch (s->kind) {
        case PCI_ENUM_BAR_IO: {
            bits = s->readback & BAR_IO_ADDRESS;
            // A 16-bit I/O BAR reads back 0 in its upper half.
            above = (bits >> 16) == 0 ? UPPER_48 : UPPER_32;
            base = s->low & BAR_IO_ADDRESS;
            break;
        }
        case PCI_ENUM_BAR_MEM32: {
            uint32_t address = s->rom ? PCI_ROM_ADDRESS_MASK : BAR_MEMORY_ADDRESS;
            bits = s->readback & address;
            above = UPPER_32;
            base = s->low & address;
            break;
        }
        case PCI_ENUM_BAR_MEM64: {
            bits = s->readback & ~(uint64_t)0xf;
            above = 0;
            base = ((uint64_t)s->high << 32) | (s->low & BAR_MEMORY_ADDRESS);
            break;
        }
        default: {
            return true;
        }
    }
    if (bits == 0) {
        return true;
    }

    uint64_t mask = bits | above;
    size_t size = ~mask + 1;
    bool contiguous = (size & (size - 1)) == 0;
    if (!contiguous) {
        size = mask & (~mask + 1);
    }
    bar->kind = s->kind;
    bar->prefetchable = s->prefetchable;
    bar->base = base;
    bar->size = size;
    return contiguous;
}

static void bar_record(
    const struct pci_enum_function *f, const struct bar_sample *s, struct pci_enum_bar *bar
) {
    if (s->malformed) {
        printf(
            "PCI " ADDR_FMT " BAR at %#x: 64-bit BAR in the last slot, taken as unimplemented\n",
            ADDR_ARGS(f->addr), s->reg
        );
    }
    if (!bar_sample_result(s, bar)) {
        printf(
            "PCI " ADDR_FMT
            " BAR at %#x: read back 0x%llx with gaps in its address bits, sized 0x%llx from the "
            "lowest\n", ADDR_ARGS(f->addr), s->reg, (unsigned long long)s->readback,
            (unsigned long long)bar->size
        );
    }
}

// Sizes the BARs in slots first to end - 1, and the expansion ROM BAR at
// rom_reg unless it is 0, with the function's decoding off unless it is a host
// bridge.
static void size_bars(
    struct pci_enum_function *f, unsigned int first, unsigned int end, uint8_t rom_reg
) {
    struct bar_sample samples[6];
    struct bar_sample rom = { .kind = PCI_ENUM_BAR_NONE };

    for (size_t i = first; i < end; i++) {
        bar_sample_read(
            f->addr, (uint8_t)(PCI_BASE_ADDRESS_0 + 4 * i), i + 1 == f->bar_count, &samples[i]
        );
        if (samples[i].kind == PCI_ENUM_BAR_MEM64 && i + 1 < end) {
            i++;
            samples[i] = (struct bar_sample){ .reg = (uint8_t)(PCI_BASE_ADDRESS_0 + 4 * i) };
        }
    }
    if (rom_reg != 0) {
        rom_sample_read(f->addr, rom_reg, &rom);
    }

    uint16_t command = decode_disable(f, PCI_COMMAND_IO | PCI_COMMAND_MEMORY);
    for (size_t i = first; i < end; i++) {
        bar_sample_probe(f->addr, &samples[i]);
    }
    bar_sample_probe(f->addr, &rom);
    decode_restore(f, command, PCI_COMMAND_IO | PCI_COMMAND_MEMORY);

    for (size_t i = first; i < end; i++) {
        bar_record(f, &samples[i], &f->bars[i]);
    }
    if (rom_reg != 0) {
        bar_record(f, &rom, &f->rom);
    }
}

static void walk_capabilities(struct pci_enum_function *f) {
    uint16_t status = (uint16_t)pci_cfg_read(f->addr, PCI_STATUS, 2);
    if ((status & PCI_STATUS_CAP_LIST) == 0) {
        return;
    }

    uint32_t pointer = f->header_type == PCI_HEADER_TYPE_CARDBUS
                     ? CARDBUS_CAPABILITIES
                     : PCI_CAPABILITY_LIST;
    uint32_t offset = pci_cfg_read(f->addr, pointer, 1) & 0xfc;
    for (size_t i = 0; i < PCI_FIND_CAP_TTL && offset >= PCI_STD_HEADER_SIZEOF; i++) {
        uint32_t entry = pci_cfg_read(f->addr, offset, 2);
        uint8_t *found;
        switch (entry & 0xff) {
            case PCI_CAP_ID_PM: {
                found = &f->pm_cap;
                break;
            }
            case PCI_CAP_ID_MSI: {
                found = &f->msi_cap;
                break;
            }
            case PCI_CAP_ID_EXP: {
                found = &f->pcie_cap;
                break;
            }
            case PCI_CAP_ID_MSIX: {
                found = &f->msix_cap;
                break;
            }
            default: {
                found = NULL;
                break;
            }
        }
        if (found != NULL && *found == 0) {
            *found = (uint8_t)offset;
        }
        offset = (entry >> 8) & 0xfc;
    }

    if (f->pcie_cap != 0) {
        uint32_t capabilities = pci_cfg_read(f->addr, f->pcie_cap + PCI_EXP_FLAGS, 2);
        f->pcie_port_type = (capabilities >> 4) & 0xf;
    }
}

static void walk_extended_capabilities(struct pci_enum_function *f) {
    if (f->pcie_cap == 0 || pci_cfg_ecam_base(f->addr.segment, f->addr.bus) == 0) {
        return;
    }

    uint32_t offset = EXT_CAP_FIRST;
    for (size_t i = 0; i < EXT_CAP_MAX_ENTRIES; i++) {
        uint32_t header = pci_cfg_read(f->addr, offset, 4);
        if (header == 0 || header == 0xffffffff) {
            break;
        }
        uint32_t id = header & 0xffff;
        if (id == EXT_CAP_REBAR && f->rebar_ext_cap == 0) {
            f->rebar_ext_cap = (uint16_t)offset;
        } else if (id == EXT_CAP_SRIOV && f->sriov_ext_cap == 0) {
            f->sriov_ext_cap = (uint16_t)offset;
        }
        // The low 2 bits of the next offset are reserved.
        offset = (header >> 20) & 0xffc;
        if (offset < EXT_CAP_FIRST) {
            break;
        }
    }
}

static void read_bridge(struct pci_enum_function *f) {
    uint32_t buses = pci_cfg_read(f->addr, PCI_PRIMARY_BUS, 4);
    f->primary_bus = (uint8_t)buses;
    f->secondary_bus = (uint8_t)(buses >> 8);
    f->subordinate_bus = (uint8_t)(buses >> 16);
    if (f->header_type != PCI_HEADER_TYPE_BRIDGE) {
        return;
    }

    // The I/O and prefetchable windows are optional, and a bridge without one
    // has its base and limit registers read as 0, which would otherwise be
    // taken for an open window at 0.
    uint32_t io = pci_cfg_read(f->addr, PCI_IO_BASE, 2);
    if (io == 0) {
        f->io_base = PCI_IO_GRANULE;
        f->io_limit = PCI_IO_GRANULE - 1;
    } else {
        f->io_base = (io & 0xf0) << 8;
        f->io_limit = (((io >> 8) & 0xf0) << 8) | (PCI_IO_GRANULE - 1);
        if ((io & WINDOW_WIDTH) == WINDOW_IO_32) {
            uint32_t upper = pci_cfg_read(f->addr, PCI_IO_BASE_UPPER16, 4);
            f->io_base |= (upper & 0xffff) << 16;
            f->io_limit |= upper & 0xffff0000;
        }
    }

    uint32_t memory = pci_cfg_read(f->addr, PCI_MEMORY_BASE, 4);
    f->mem_base = (memory & 0xfff0) << 16;
    f->mem_limit = (memory & 0xfff00000) | (PCI_MEMORY_GRANULE - 1);

    uint32_t pref = pci_cfg_read(f->addr, PCI_PREF_MEMORY_BASE, 4);
    if (pref == 0) {
        f->pref_base = PCI_MEMORY_GRANULE;
        f->pref_limit = PCI_MEMORY_GRANULE - 1;
    } else {
        f->pref_base = (uint64_t)(pref & 0xfff0) << 16;
        f->pref_limit = (pref & 0xfff00000) | (PCI_MEMORY_GRANULE - 1);
        if ((pref & WINDOW_WIDTH) == PCI_PREF_RANGE_TYPE_64) {
            f->pref_base |= (uint64_t)pci_cfg_read(f->addr, PCI_PREF_BASE_UPPER32, 4) << 32;
            f->pref_limit |= (uint64_t)pci_cfg_read(f->addr, PCI_PREF_LIMIT_UPPER32, 4) << 32;
        }
    }

    f->bridge_control = (uint16_t)pci_cfg_read(f->addr, PCI_BRIDGE_CONTROL, 2);
}

static bool is_bridge(const struct pci_enum_function *f) {
    return f->header_type == PCI_HEADER_TYPE_BRIDGE || f->header_type == PCI_HEADER_TYPE_CARDBUS;
}

static const char *bar_kind_name(const struct pci_enum_bar *bar) {
    switch (bar->kind) {
        case PCI_ENUM_BAR_IO: {
            return "io";
        }
        case PCI_ENUM_BAR_MEM32: {
            return bar->prefetchable ? "mem32-pref" : "mem32";
        }
        default: {
            return bar->prefetchable ? "mem64-pref" : "mem64";
        }
    }
}

static void line_add_window(
    struct log_line *line, const char *name, uintptr_t base, uint64_t limit
) {
    if (limit < base) {
        line_add(line, " %s closed", name);
    } else {
        line_add(
            line, " %s 0x%llx-0x%llx", name, (unsigned long long)base, (unsigned long long)limit
        );
    }
}

static void log_function(const struct pci_enum_function *f) {
    struct log_line line;
    line_start(&line, "PCI " ADDR_FMT, ADDR_ARGS(f->addr));
    line_add(
        &line, " %04x:%04x class %02x%02x%02x type %u", f->vendor_id, f->device_id, f->base_class,
        f->subclass, f->prog_if, f->header_type
    );
    for (size_t i = 0; i < f->bar_count; i++) {
        const struct pci_enum_bar *bar = &f->bars[i];
        if (bar->size != 0) {
            line_add(
                &line, " BAR%zu %s 0x%llx+0x%llx", i, bar_kind_name(bar),
                (unsigned long long)bar->base, (unsigned long long)bar->size
            );
        }
    }
    if (f->rom.size != 0) {
        line_add(
            &line, " ROM 0x%llx+0x%llx", (unsigned long long)f->rom.base,
            (unsigned long long)f->rom.size
        );
    }
    if (is_bridge(f)) {
        line_add(
            &line, " pri %02x sec %02x sub %02x", f->primary_bus, f->secondary_bus,
            f->subordinate_bus
        );
    }
    if (f->header_type == PCI_HEADER_TYPE_BRIDGE) {
        line_add_window(&line, "io", f->io_base, f->io_limit);
        line_add_window(&line, "mem", f->mem_base, f->mem_limit);
        line_add_window(&line, "pref", f->pref_base, f->pref_limit);
    }
    line_flush(&line);
}

static struct pci_enum_function *function_list_add(struct function_list *list) {
    list->items = heap_grow(
        list->items, list->count, &list->capacity, sizeof(struct pci_enum_function), 64
    );
    return &list->items[list->count++];
}

static void scan_function(struct function_list *list, struct pci_cfg_addr addr, uint32_t id) {
    struct pci_enum_function *f = function_list_add(list);
    f->addr = addr;
    f->vendor_id = (uint16_t)id;
    f->device_id = (uint16_t)(id >> 16);
    uint32_t class_revision = pci_cfg_read(addr, PCI_CLASS_REVISION, 4);
    f->revision = (uint8_t)class_revision;
    f->prog_if = (uint8_t)(class_revision >> 8);
    f->subclass = (uint8_t)(class_revision >> 16);
    f->base_class = (uint8_t)(class_revision >> 24);
    f->header_type = pci_cfg_read(addr, PCI_HEADER_TYPE, 1) & PCI_HEADER_TYPE_MASK;
    uint32_t interrupt = pci_cfg_read(addr, PCI_INTERRUPT_LINE, 2);
    f->interrupt_line = (uint8_t)interrupt;
    f->interrupt_pin = (uint8_t)(interrupt >> 8);

    walk_capabilities(f);
    walk_extended_capabilities(f);

    switch (f->header_type) {
        case PCI_HEADER_TYPE_NORMAL: {
            f->bar_count = 6;
            size_bars(f, 0, f->bar_count, PCI_ROM_ADDRESS);
            break;
        }
        case PCI_HEADER_TYPE_BRIDGE: {
            f->bar_count = 2;
            size_bars(f, 0, f->bar_count, PCI_ROM_ADDRESS1);
            read_bridge(f);
            break;
        }
        case PCI_HEADER_TYPE_CARDBUS: {
            read_bridge(f);
            break;
        }
        default: {
            break;
        }
    }

    log_function(f);
}

static void scan_bus(struct function_list *list, uint16_t segment, uint8_t bus) {
    for (size_t device = 0; device < 32; device++) {
        struct pci_cfg_addr addr = {
            .segment = segment,
            .bus = bus,
            .device = (uint8_t)device,
            .function = 0
        };
        uint32_t id = pci_cfg_read(addr, PCI_VENDOR_ID, 4);
        if ((id & 0xffff) == 0xffff) {
            continue;
        }
        uint32_t header = pci_cfg_read(addr, PCI_HEADER_TYPE, 1);
        unsigned int functions = (header & PCI_HEADER_TYPE_MFD) != 0 ? 8 : 1;
        scan_function(list, addr, id);

        for (size_t function = 1; function < functions; function++) {
            addr.function = (uint8_t)function;
            id = pci_cfg_read(addr, PCI_VENDOR_ID, 4);
            if ((id & 0xffff) != 0xffff) {
                scan_function(list, addr, id);
            }
        }
    }
}

// All of segment 0's buses are scanned, through the legacy mechanism where no
// configuration space window covers them; other segments are only reachable
// through their windows. Sorted by segment.
static uint32_t buses_to_scan(struct segment_buses **out) {
    struct uacpi_table table;
    const uint8_t *mcfg = NULL;
    size_t entry_count = 0;
    if (acpi_tables_ready()
     && uacpi_table_find_by_signature(ACPI_MCFG_SIGNATURE, &table) == UACPI_STATUS_OK) {
        mcfg = table.ptr;
        if (table.hdr->length > sizeof(struct acpi_mcfg)) {
            entry_count = (table.hdr->length - sizeof(struct acpi_mcfg))
                        / sizeof(struct acpi_mcfg_allocation);
        }
    }

    struct segment_buses *list = kmalloc((entry_count + 1) * sizeof(struct segment_buses));
    size_t count = 1;
    list[0].segment = 0;
    memset(list[0].buses, 0xff, sizeof(list[0].buses));

    for (size_t i = 0; i < entry_count; i++) {
        struct acpi_mcfg_allocation entry;
        memcpy(
            &entry, mcfg + sizeof(struct acpi_mcfg) + i * sizeof(struct acpi_mcfg_allocation),
            sizeof(struct acpi_mcfg_allocation)
        );
        if (entry.segment == 0) {
            continue;
        }

        size_t j = 0;
        while (j < count && list[j].segment < entry.segment) {
            j++;
        }
        if (j == count || list[j].segment != entry.segment) {
            memmove(&list[j + 1], &list[j], (count - j) * sizeof(struct segment_buses));
            list[j] = (struct segment_buses){ .segment = entry.segment };
            count++;
        }
        for (size_t bus = entry.start_bus; bus <= entry.end_bus; bus++) {
            if (pci_cfg_ecam_base(entry.segment, (uint8_t)bus) != 0) {
                list[j].buses[bus / 64] |= (uint64_t)1 << (bus % 64);
            }
        }
    }
    if (mcfg != NULL) {
        uacpi_table_unref(&table);
    }

    *out = list;
    return count;
}

// A bridge whose secondary bus is not above its own bus has not been given
// one, and must not be taken for the parent of the functions on bus 0.
static void link_topology(void) {
    size_t count = topology.function_count;
    struct pci_enum_function *functions = topology.functions;
    if (count == 0) {
        return;
    }

    struct pci_enum_function **bridges = kmalloc(256 * sizeof(struct pci_enum_function *));
    struct pci_enum_root_bus *roots = kmalloc(count * sizeof(struct pci_enum_root_bus));
    size_t root_count = 0;

    size_t first = 0;
    while (first < count) {
        uint16_t segment = functions[first].addr.segment;
        size_t end = first + 1;
        while (end < count && functions[end].addr.segment == segment) {
            end++;
        }

        memset(bridges, 0, 256 * sizeof(struct pci_enum_function *));
        for (size_t i = first; i < end; i++) {
            struct pci_enum_function *f = &functions[i];
            if (!is_bridge(f) || f->secondary_bus <= f->addr.bus) {
                continue;
            }
            struct pci_enum_function *other = bridges[f->secondary_bus];
            if (other != NULL) {
                printf(
                    "PCI " ADDR_FMT ": secondary bus %02x is already behind " ADDR_FMT
                    ", ignoring this bridge for it\n", ADDR_ARGS(f->addr), f->secondary_bus,
                    ADDR_ARGS(other->addr)
                );
                continue;
            }
            bridges[f->secondary_bus] = f;
        }

        for (size_t i = first; i < end; i++) {
            struct pci_enum_function *f = &functions[i];
            f->parent = bridges[f->addr.bus];
            bool first_on_bus = i == first || functions[i - 1].addr.bus != f->addr.bus;
            if (f->parent == NULL && first_on_bus) {
                roots[root_count].segment = segment;
                roots[root_count].bus = f->addr.bus;
                root_count++;
            }
        }
        first = end;
    }

    kfree(bridges);
    topology.root_buses = roots;
    topology.root_bus_count = root_count;
}

const struct pci_enum_topology *pci_enum_scan(void) {
    if (atomic_load_explicit(&scanned, memory_order_acquire)) {
        return &topology;
    }

    struct segment_buses *segments;
    size_t segment_count = buses_to_scan(&segments);
    struct function_list list = { .items = NULL, .count = 0, .capacity = 0 };
    for (size_t i = 0; i < segment_count; i++) {
        for (size_t bus = 0; bus < 256; bus++) {
            if ((segments[i].buses[bus / 64] & ((uint64_t)1 << (bus % 64))) != 0) {
                scan_bus(&list, segments[i].segment, (uint8_t)bus);
            }
        }
    }
    kfree(segments);

    topology.functions = list.items;
    topology.function_count = list.count;
    link_topology();

    printf(
        "PCI: %zu functions, %zu root buses\n", topology.function_count, topology.root_bus_count
    );
    if (topology.root_bus_count != 0) {
        struct log_line line;
        line_start(&line, "PCI root buses:");
        for (size_t i = 0; i < topology.root_bus_count; i++) {
            line_add(
                &line, " %04x:%02x", topology.root_buses[i].segment, topology.root_buses[i].bus
            );
        }
        line_flush(&line);
    }

    atomic_store_explicit(&scanned, true, memory_order_release);
    return &topology;
}

const struct pci_enum_topology *pci_enum_topology(void) {
    return atomic_load_explicit(&scanned, memory_order_acquire) ? &topology : NULL;
}

static uint32_t addr_key(struct pci_cfg_addr addr) {
    return (uint32_t)addr.segment << 16 | (uint32_t)addr.bus << 8 | (uint32_t)addr.device << 3
         | addr.function;
}

struct pci_enum_function *pci_enum_find(struct pci_cfg_addr addr) {
    if (!atomic_load_explicit(&scanned, memory_order_acquire) || addr.device >= 32
     || addr.function >= 8) {
        return NULL;
    }

    uint32_t key = addr_key(addr);
    uint32_t low = 0;
    uint32_t high = topology.function_count;
    while (low < high) {
        uint32_t middle = low + (high - low) / 2;
        uint32_t middle_key = addr_key(topology.functions[middle].addr);
        if (middle_key == key) {
            return &topology.functions[middle];
        }
        if (middle_key < key) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return NULL;
}

static size_t rebar_size(unsigned int encoding) {
    return (uint64_t)1 << (encoding + REBAR_SIZE_BASE);
}

// Bit n set for each supported size encoding n: the capability register's bits
// 31:4 are the sizes from 1 MiB to 128 TiB, and the control register's bits
// 31:16 the larger ones that follow.
static uint64_t rebar_supported(uint32_t capability, uint32_t control) {
    return (uint64_t)(capability >> 4) | ((uint64_t)(control >> 16) << 28);
}

static bool rebar_has(uint64_t supported, int encoding) {
    return ((supported >> encoding) & 1) != 0;
}

static void rebar_resize(
    struct pci_enum_function *f, uint8_t index, uint32_t control_reg, unsigned int encoding
) {
    struct pci_enum_bar *bar = &f->bars[index];
    bool wide = bar->kind == PCI_ENUM_BAR_MEM64;
    size_t old_size = bar->size;
    size_t new_size = rebar_size(encoding);

    uint32_t low = pci_cfg_read(f->addr, bar->reg, 4);
    uint32_t high = wide ? pci_cfg_read(f->addr, bar->reg + 4, 4) : 0;
    uint16_t command = decode_disable(f, PCI_COMMAND_MEMORY);
    uint32_t control = pci_cfg_read(f->addr, control_reg, 4);
    control &= ~(REBAR_SIZE << REBAR_SIZE_SHIFT);
    control |= encoding << REBAR_SIZE_SHIFT;
    pci_cfg_write(f->addr, control_reg, 4, control);
    // The BAR's contents are undefined after a resize. A host bridge keeps
    // decoding, so the upper register goes back first, lest a BAR that belongs
    // above 4 GiB decode at its lower register's address below it.
    if (wide) {
        pci_cfg_write(f->addr, bar->reg + 4, 4, high);
    }
    pci_cfg_write(f->addr, bar->reg, 4, low);
    decode_restore(f, command, PCI_COMMAND_MEMORY);

    unsigned int now = (pci_cfg_read(f->addr, control_reg, 4) >> REBAR_SIZE_SHIFT) & REBAR_SIZE;
    if (now != encoding) {
        printf(
            "PCI " ADDR_FMT " BAR%u: Resizable BAR control reads size %u after writing size %u\n",
            ADDR_ARGS(f->addr), index, now, encoding
        );
    }
    size_bars(f, index, index + 1, 0);
    if (bar->size != new_size) {
        printf(
            "PCI " ADDR_FMT " BAR%u: sizes to 0x%llx after resizing to 0x%llx\n",
            ADDR_ARGS(f->addr), index, (unsigned long long)bar->size, (unsigned long long)new_size
        );
    }
    printf(
        "PCI " ADDR_FMT " BAR%u: Resizable BAR resized from 0x%llx to 0x%llx\n",
        ADDR_ARGS(f->addr), index, (unsigned long long)old_size, (unsigned long long)bar->size
    );
}

static void rebar_shrink(
    struct pci_enum_function *f, uint32_t control_reg, uint32_t capability, uint32_t control,
    size_t max_size
) {
    uint8_t index = control & REBAR_INDEX;
    if (index >= f->bar_count) {
        printf(
            "PCI " ADDR_FMT
            ": Resizable BAR control for BAR%u, which the function does not have\n",
            ADDR_ARGS(f->addr), index
        );
        return;
    }
    struct pci_enum_bar *bar = &f->bars[index];
    if ((bar->kind != PCI_ENUM_BAR_MEM32 && bar->kind != PCI_ENUM_BAR_MEM64) || bar->size == 0) {
        return;
    }

    unsigned int current = (control >> REBAR_SIZE_SHIFT) & REBAR_SIZE;
    if (current > REBAR_SIZE_LAST) {
        printf(
            "PCI " ADDR_FMT " BAR%u: Resizable BAR size %u is out of range, leaving it\n",
            ADDR_ARGS(f->addr), index, current
        );
        return;
    }
    size_t current_size = rebar_size(current);
    if (current_size <= max_size) {
        return;
    }

    uint64_t supported = rebar_supported(capability, control);
    int pick = -1;
    for (size_t n = REBAR_SIZE_LAST + 1; n-- > 0; ) {
        if (rebar_has(supported, n) && rebar_size((unsigned int)n) <= max_size) {
            pick = n;
            break;
        }
    }
    if (pick < 0 && supported != 0) {
        pick = __builtin_ctzll(supported);
    }
    if (pick < 0 || rebar_size((unsigned int)pick) >= current_size) {
        printf(
            "PCI " ADDR_FMT
            " BAR%u: Resizable BAR of 0x%llx supports no smaller size, leaving it\n",
            ADDR_ARGS(f->addr), index, (unsigned long long)current_size
        );
        return;
    }

    const struct handoff_framebuffer *fb = &core_handoff->video.framebuffer;
    if (fb->present && fb->base >= bar->base && fb->base - bar->base < current_size) {
        size_t offset = fb->base - bar->base;
        size_t length = fb->size != 0 ? fb->size : 1;
        uint64_t needed = length > UINT64_MAX - offset ? UINT64_MAX : offset + length;
        int fit = -1;
        for (size_t n = pick; n <= REBAR_SIZE_LAST; n++) {
            if (rebar_has(supported, n) && rebar_size((unsigned int)n) >= needed) {
                fit = n;
                break;
            }
        }
        if (fit < 0 || rebar_size((unsigned int)fit) >= current_size) {
            printf(
                "PCI " ADDR_FMT
                " BAR%u: Resizable BAR of 0x%llx holds the framebuffer at 0x%llx+0x%llx, leaving "
                "it\n", ADDR_ARGS(f->addr), index, (unsigned long long)current_size,
                (unsigned long long)fb->base, (unsigned long long)fb->size
            );
            return;
        }
        pick = fit;
    }

    rebar_resize(f, index, control_reg, (unsigned int)pick);
}

void pci_enum_shrink_rebars(size_t max_size) {
    if (!atomic_load_explicit(&scanned, memory_order_acquire)) {
        return;
    }

    for (size_t i = 0; i < topology.function_count; i++) {
        struct pci_enum_function *f = &topology.functions[i];
        if (f->rebar_ext_cap == 0) {
            continue;
        }

        uint32_t first_control = pci_cfg_read(f->addr, f->rebar_ext_cap + REBAR_CONTROL(0), 4);
        unsigned int pairs = (first_control >> REBAR_PAIRS_SHIFT) & REBAR_PAIRS;
        if (pairs == 0 || pairs > REBAR_MAX_PAIRS) {
            printf(
                "PCI " ADDR_FMT ": Resizable BAR capability with %u BARs, ignoring it\n",
                ADDR_ARGS(f->addr), pairs
            );
            continue;
        }
        for (size_t n = 0; n < pairs; n++) {
            uint32_t control_reg = f->rebar_ext_cap + REBAR_CONTROL(n);
            uint32_t capability = pci_cfg_read(f->addr, f->rebar_ext_cap + REBAR_CAPABILITY(n), 4);
            uint32_t control = pci_cfg_read(f->addr, control_reg, 4);
            rebar_shrink(f, control_reg, capability, control, max_size);
        }
    }
}
