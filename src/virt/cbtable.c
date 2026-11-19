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
#include <uacpi/namespace.h>
#include <uacpi/platform/types.h>
#include <uacpi/resources.h>
#include <uacpi/status.h>
#include <uacpi/types.h>
#include <uacpi/utilities.h>
#include <virt/cbtable.h>
#include <csm/e820.h>
#include <hw/acpi.h>
#include <hw/pci_host.h>
#include <lib/log.h>
#include <lib/printf.h>
#include <lib/util.h>
#include <memory.h>
#include <sys/mm.h>
#include <sys/x86.h>
#include <virt/devmem.h>
#include <virt/gpt.h>
#include <virt/io.h>
#include <virt/layout.h>
#include <virt/mmio.h>
#include <virt/vcpu.h>

// coreboot 26.09's table, src/commonlib/include/commonlib/coreboot_tables.h:
// a header whose signature is "LBIO", then records, each of a tag and a size
// that is a multiple of 4 bytes, with RFC 1071 checksums of the header and of
// the records.
#define HEADER_SIZE 24
#define HEADER_HEADER_BYTES 4
#define HEADER_HEADER_CHECKSUM 8
#define HEADER_TABLE_BYTES 12
#define HEADER_TABLE_CHECKSUM 16
#define HEADER_TABLE_ENTRIES 20
#define RECORD_SIZE 8
#define RECORD_ALIGN 4
// A record's pointer or address, after its tag and size.
#define RECORD_ADDRESS 8

#define LB_TAG_MEMORY 0x01
#define LB_TAG_SERIAL 0x0f
#define LB_TAG_CONSOLE 0x10
#define LB_TAG_SPI_FLASH 0x29
#define LB_TAG_CBMEM_ENTRY 0x31
#define LB_TAG_ACPI_RSDP 0x43

// struct lb_memory_range
#define MEMORY_RANGE_SIZE 20
// struct lb_serial: its type, then its base address.
#define SERIAL_TYPE 8
#define SERIAL_BASE 12
#define SERIAL_SIZE 16
#define LB_SERIAL_TYPE_IO_MAPPED 1
#define UART_PORTS 8
// struct lb_console
#define CONSOLE_TYPE 8
#define CONSOLE_SIZE 10
#define LB_TAG_CONSOLE_SERIAL8250 0
#define LB_TAG_CONSOLE_SERIAL8250MEM 6
// struct lb_spi_flash, up to its windows, the last field its count of them.
#define SPI_FLASH_WINDOWS 20
#define SPI_FLASH_SIZE 24
// struct lb_cbmem_ref, and struct lb_cbmem_entry, with the entry's size after
// its address.
#define DATA_RECORD_SIZE 16
#define CBMEM_ENTRY_SIZE 16
#define CBMEM_ENTRY_RECORD_SIZE 24

// What the guest's table does with a record of the firmware's.
#define HANDLING_DROP 0
// It holds on the guest's machine as it is.
#define HANDLING_KEEP 1
// The memory map, which the guest's replaces.
#define HANDLING_MEMORY 2
// A UART, which holds where the guest reaches its ports.
#define HANDLING_SERIAL 3
// A console, which holds where it is no UART, or the UART's record does.
#define HANDLING_CONSOLE 4
// A pointer to firmware data in CBMEM, which the guest reads at an address of
// its own.
#define HANDLING_DATA 5
// A CBMEM entry, which holds for the firmware data that the guest reads.
#define HANDLING_CBMEM_ENTRY 6
// The SPI flash, which holds without its windows in the host's addresses, as
// the guest's machine has the legacy BIOS's image at the top of 4 GiB.
#define HANDLING_SPI_FLASH 7
// The ACPI RSDP, which becomes the guest's.
#define HANDLING_RSDP 8

// The records the guest's table keeps, by their tags in coreboot 26.09: the
// rest describe the host's machine and not the guest's, or are unknown. Those
// are the framebuffer, as the guest starts in text mode, where coreboot writes
// none (src/lib/coreboot_table.c, lb_framebuffer()); the ACPI GNVS and CNVS,
// the state of the AML that ReBIOS runs, and the SMM store and the TPM's PPI
// buffer, interfaces of the firmware's own; the PCIe controller's, the root
// bridges', and a non-PCI SD host's addresses, and the host's memory that DMA,
// RAM oops, capsules, and the MRC cache take; the ROM's MTRR; the CMOS option
// table, its checksum, VBNV, and the setup menu, as the guest's CMOS RAM is
// its own; and a forwarding pointer.
static const struct {
    uint32_t tag;
    int handling;
} rules[] = {
    // LB_TAG_MEMORY
    { 0x01, HANDLING_MEMORY },
    // LB_TAG_MAINBOARD, and the strings from LB_TAG_VERSION to
    // LB_TAG_ASSEMBLER
    { 0x03, HANDLING_KEEP },
    { 0x04, HANDLING_KEEP },
    { 0x05, HANDLING_KEEP },
    { 0x06, HANDLING_KEEP },
    { 0x07, HANDLING_KEEP },
    { 0x08, HANDLING_KEEP },
    { 0x09, HANDLING_KEEP },
    { 0x0a, HANDLING_KEEP },
    { 0x0b, HANDLING_KEEP },
    { 0x0c, HANDLING_KEEP },
    { 0x0d, HANDLING_KEEP },
    // LB_TAG_SERIAL, LB_TAG_CONSOLE, and LB_TAG_GPIO
    { 0x0f, HANDLING_SERIAL },
    { 0x10, HANDLING_CONSOLE },
    { 0x13, HANDLING_KEEP },
    // LB_TAG_TIMESTAMPS and LB_TAG_CBMEM_CONSOLE
    { 0x16, HANDLING_DATA },
    { 0x17, HANDLING_DATA },
    // LB_TAG_BOARD_ID, LB_TAG_VERSION_TIMESTAMP, LB_TAG_WIFI_CALIBRATION,
    // LB_TAG_RAM_CODE, LB_TAG_SPI_FLASH, LB_TAG_SERIALNO, LB_TAG_VPD, and
    // LB_TAG_SKU_ID
    { 0x25, HANDLING_KEEP },
    { 0x26, HANDLING_KEEP },
    { 0x27, HANDLING_DATA },
    { 0x28, HANDLING_KEEP },
    { 0x29, HANDLING_SPI_FLASH },
    { 0x2a, HANDLING_KEEP },
    { 0x2c, HANDLING_DATA },
    { 0x2d, HANDLING_KEEP },
    // LB_TAG_BOOT_MEDIA_PARAMS, LB_TAG_CBMEM_ENTRY, LB_TAG_TSC_INFO,
    // LB_TAG_MAC_ADDRS, LB_TAG_VBOOT_WORKBUF, LB_TAG_MMC_INFO,
    // LB_TAG_TPM_CB_LOG, LB_TAG_FMAP, and LB_TAG_PLATFORM_BLOB_VERSION
    { 0x30, HANDLING_KEEP },
    { 0x31, HANDLING_CBMEM_ENTRY },
    { 0x32, HANDLING_KEEP },
    { 0x33, HANDLING_KEEP },
    { 0x34, HANDLING_DATA },
    { 0x35, HANDLING_KEEP },
    { 0x36, HANDLING_DATA },
    { 0x37, HANDLING_DATA },
    { 0x38, HANDLING_KEEP },
    // LB_TAG_BOARD_CONFIG, LB_TAG_TYPE_C_INFO, LB_TAG_ACPI_RSDP,
    // LB_TAG_EFI_FW_INFO, LB_TAG_PANEL_POWEROFF, LB_TAG_BOOT_REASON, and
    // LB_TAG_BOOT_MODE
    { 0x40, HANDLING_KEEP },
    { 0x42, HANDLING_DATA },
    { 0x43, HANDLING_RSDP },
    { 0x45, HANDLING_KEEP },
    { 0x49, HANDLING_KEEP },
    { 0x4b, HANDLING_KEEP },
    { 0xcd, HANDLING_KEEP }
};

// The record tags that the log names among those left out, at most.
#define DROPPED_SHOWN 24

static struct {
    bool found;
    bool valid;
    bool built;
    // The host's table, and the room that the firmware's ACPI gives it.
    uintptr_t host;
    size_t room;
    uint32_t header_bytes;
    uint32_t table_bytes;
    uint32_t entries;
    // The host's pages that hold the room.
    uintptr_t area;
    size_t area_size;
    // The guest's table, at the start of the guest's area, mapped for the
    // guest to read.
    uint8_t *page;
    size_t page_size;
} cbtable;

// RFC 1071's checksum, over the bytes as 16-bit words in little-endian order,
// as coreboot 26.09's src/commonlib/bsd/ipchksum.c takes them.
static uint16_t ip_checksum(const uint8_t *data, size_t size) {
    uint32_t sum = 0;
    for (size_t i = 0; i < size; i++) {
        sum += (uint32_t)data[i] << (i % 2 != 0 ? 8 : 0);
        sum = (sum & 0xffff) + (sum >> 16);
    }
    return (uint16_t)~sum;
}

static int handling(uint32_t tag) {
    for (size_t i = 0; i < ARRAY_COUNT(rules); i++) {
        if (rules[i].tag == tag) {
            return rules[i].handling;
        }
    }
    return HANDLING_DROP;
}

static const uint8_t *host_records(void) {
    return (const uint8_t *)cbtable.host + cbtable.header_bytes;
}

// What is wrong with the host's table, or NULL: its signature, its sizes
// against its room, as Linux 7.3's drivers/firmware/google/coreboot_table.c
// checks them, its checksums, and its records' sizes.
static const char *table_fault(void) {
    const uint8_t *table = (const uint8_t *)cbtable.host;
    if (cbtable.room < HEADER_SIZE || memcmp(table, "LBIO", 4) != 0) {
        return "has no LBIO signature";
    }
    uint32_t header_bytes = read_le32(table + HEADER_HEADER_BYTES);
    uint32_t table_bytes = read_le32(table + HEADER_TABLE_BYTES);
    if (header_bytes < HEADER_SIZE || header_bytes > cbtable.room
     || table_bytes > cbtable.room - header_bytes) {
        return "is larger than the room for it";
    }
    uint16_t records_sum = ip_checksum(table + header_bytes, table_bytes);
    if (ip_checksum(table, HEADER_SIZE) != 0
     || records_sum != read_le32(table + HEADER_TABLE_CHECKSUM)) {
        return "has a wrong checksum";
    }
    uint32_t entries = read_le32(table + HEADER_TABLE_ENTRIES);
    size_t offset = 0;
    for (size_t i = 0; i < entries; i++) {
        if (table_bytes - offset < RECORD_SIZE) {
            return "has fewer records than its header says";
        }
        uint32_t size = read_le32(table + header_bytes + offset + 4);
        if (size < RECORD_SIZE || size % RECORD_ALIGN != 0 || size > table_bytes - offset) {
            return "has a record of a wrong size";
        }
        offset += size;
    }
    cbtable.header_bytes = header_bytes;
    cbtable.table_bytes = table_bytes;
    cbtable.entries = entries;
    return NULL;
}

// The size of the CBMEM entry at address that the host's table lists, where
// the guest may read it: 0 where it lists none, or the entry shares pages with
// the table.
static size_t data_size(uint64_t address) {
    const uint8_t *r = host_records();
    for (size_t i = 0, offset = 0; i < cbtable.entries; i++) {
        uint32_t size = read_le32(r + offset + 4);
        if (read_le32(r + offset) == LB_TAG_CBMEM_ENTRY && size >= CBMEM_ENTRY_RECORD_SIZE
         && read_le64(r + offset + RECORD_ADDRESS) == address) {
            uint32_t entry_size = read_le32(r + offset + CBMEM_ENTRY_SIZE);
            uint64_t end = address + entry_size;
            if (entry_size == 0 || end < address
             || (page_down(address) < cbtable.area + cbtable.area_size
              && page_up(end) > cbtable.area)) {
                return 0;
            }
            return entry_size;
        }
        offset += size;
    }
    return 0;
}

// Where the guest reads the firmware data at address that a record points to:
// false where it does not.
static bool data_guest(uint64_t address, uint64_t *guest) {
    size_t size = data_size(address);
    return size != 0 && devmem_guest(address, address + size, false, guest);
}

// Whether a record that the guest's table keeps points to the firmware data at
// address.
static bool data_pointed(uint64_t address) {
    const uint8_t *r = host_records();
    for (size_t i = 0, offset = 0; i < cbtable.entries; i++) {
        uint32_t size = read_le32(r + offset + 4);
        if (handling(read_le32(r + offset)) == HANDLING_DATA && size >= DATA_RECORD_SIZE
         && read_le64(r + offset + RECORD_ADDRESS) == address) {
            return true;
        }
        offset += size;
    }
    return false;
}

// Whether the guest reaches the UART that a record of the host's table gives.
static bool serial_kept(void) {
    const uint8_t *r = host_records();
    for (size_t i = 0, offset = 0; i < cbtable.entries; i++) {
        uint32_t size = read_le32(r + offset + 4);
        if (read_le32(r + offset) == LB_TAG_SERIAL && size >= SERIAL_SIZE
         && read_le32(r + offset + SERIAL_TYPE) == LB_SERIAL_TYPE_IO_MAPPED) {
            uint32_t base = read_le32(r + offset + SERIAL_BASE);
            return base <= UINT16_MAX - UART_PORTS + 1 && io_passed((uint16_t)base, UART_PORTS);
        }
        offset += size;
    }
    return false;
}

struct building {
    uint8_t *records;
    size_t room;
    size_t size;
    uint32_t entries;
    bool memory_done;
    bool overflow;
    uint32_t dropped[DROPPED_SHOWN];
    size_t dropped_count;
    size_t data_count;
};

// Puts a record of size bytes at the end of the guest's records, with record
// its bytes, or NULL for the caller to fill it, and returns where it went, or
// NULL where it does not fit.
static uint8_t *record_add(struct building *b, const uint8_t *record, size_t size) {
    if (size > b->room - b->size) {
        b->overflow = true;
        return NULL;
    }
    uint8_t *at = b->records + b->size;
    if (record != NULL) {
        memcpy(at, record, size);
    }
    write_le32(at + 4, (uint32_t)size);
    b->size += size;
    b->entries++;
    return at;
}

static void record_drop(struct building *b, uint32_t tag) {
    for (size_t i = 0; i < b->dropped_count; i++) {
        if (b->dropped[i] == tag) {
            return;
        }
    }
    if (b->dropped_count < DROPPED_SHOWN) {
        b->dropped[b->dropped_count++] = tag;
    }
}

// The guest's memory map, of the E820 types, whose values coreboot 26.09's
// memory ranges share below 16.
static void memory_add(struct building *b) {
    static struct e820_map map;
    if (!layout_e820(&map)) {
        printf("coreboot's table: the guest's memory map lost ranges that did not fit\n");
    }
    uint8_t *at = record_add(b, NULL, RECORD_SIZE + map.count * MEMORY_RANGE_SIZE);
    if (at == NULL) {
        return;
    }
    write_le32(at, LB_TAG_MEMORY);
    for (size_t i = 0; i < map.count; i++) {
        uint8_t *range = at + RECORD_SIZE + i * MEMORY_RANGE_SIZE;
        write_le64(range, map.entries[i].base);
        write_le64(range + 8, map.entries[i].size);
        write_le32(range + 16, map.entries[i].type);
    }
}

// Puts what the guest's table holds of a record of the host's at its end.
static void record_take(struct building *b, const uint8_t *r, bool serial, uint32_t rsdp) {
    uint32_t tag = read_le32(r);
    uint32_t size = read_le32(r + 4);
    int how = handling(tag);
    uint64_t guest;
    switch (how) {
        case HANDLING_KEEP: {
            record_add(b, r, size);
            return;
        }
        case HANDLING_MEMORY: {
            if (!b->memory_done) {
                b->memory_done = true;
                memory_add(b);
            }
            return;
        }
        case HANDLING_SERIAL: {
            if (serial) {
                record_add(b, r, size);
                return;
            }
            break;
        }
        case HANDLING_CONSOLE: {
            uint16_t type = size >= CONSOLE_SIZE ? read_le16(r + CONSOLE_TYPE) : UINT16_MAX;
            if (serial
             || (type != LB_TAG_CONSOLE_SERIAL8250 && type != LB_TAG_CONSOLE_SERIAL8250MEM)) {
                record_add(b, r, size);
                return;
            }
            break;
        }
        case HANDLING_DATA: {
            if (size >= DATA_RECORD_SIZE && data_guest(read_le64(r + RECORD_ADDRESS), &guest)) {
                uint8_t *at = record_add(b, r, size);
                if (at != NULL) {
                    write_le64(at + RECORD_ADDRESS, guest);
                    b->data_count++;
                }
                return;
            }
            break;
        }
        case HANDLING_CBMEM_ENTRY: {
            uint64_t address = size >= CBMEM_ENTRY_RECORD_SIZE ? read_le64(r + RECORD_ADDRESS) : 0;
            if (address != 0 && data_pointed(address) && data_guest(address, &guest)) {
                uint8_t *at = record_add(b, r, size);
                if (at != NULL) {
                    write_le64(at + RECORD_ADDRESS, guest);
                }
                return;
            }
            break;
        }
        case HANDLING_SPI_FLASH: {
            uint8_t *at = record_add(b, r, size >= SPI_FLASH_SIZE ? SPI_FLASH_SIZE : size);
            if (at != NULL && size >= SPI_FLASH_SIZE) {
                write_le32(at + SPI_FLASH_WINDOWS, 0);
            }
            return;
        }
        case HANDLING_RSDP: {
            if (rsdp != 0 && size >= DATA_RECORD_SIZE) {
                uint8_t *at = record_add(b, r, size);
                if (at != NULL) {
                    write_le64(at + RECORD_ADDRESS, rsdp);
                }
                return;
            }
            break;
        }
        default: {
            break;
        }
    }
    record_drop(b, tag);
}

static uint64_t table_read(struct vcpu *v, void *ctx, uint64_t offset, size_t size) {
    (void)v;
    (void)ctx;
    uint64_t value = 0;
    if (offset < cbtable.page_size) {
        memcpy(&value, cbtable.page + offset, size);
    }
    return value;
}

static const struct mmio_ops table_ops = {
    .read = table_read,
    .write = mmio_absent_write,
    .reads_virtual = true,
    .writes_virtual = true,
    .writes_ignored = true
};

// A device's first memory resource, as Linux 7.3 gives a platform device its
// resources from _CRS (drivers/acpi/resource.c).
struct first_memory {
    bool found;
    uint64_t base;
    uint64_t length;
};

static uacpi_iteration_decision memory_resource(void *user, uacpi_resource *resource) {
    struct first_memory *m = user;
    const uacpi_resource_address_common *common = NULL;
    switch (resource->type) {
        case UACPI_RESOURCE_TYPE_MEMORY24: {
            m->base = (uint64_t)resource->memory24.minimum << 8;
            m->length = (uint64_t)resource->memory24.length << 8;
            break;
        }
        case UACPI_RESOURCE_TYPE_MEMORY32: {
            m->base = resource->memory32.minimum;
            m->length = resource->memory32.length;
            break;
        }
        case UACPI_RESOURCE_TYPE_FIXED_MEMORY32: {
            m->base = resource->fixed_memory32.address;
            m->length = resource->fixed_memory32.length;
            break;
        }
        case UACPI_RESOURCE_TYPE_ADDRESS16: {
            common = &resource->address16.common;
            m->base = resource->address16.minimum;
            m->length = resource->address16.address_length;
            break;
        }
        case UACPI_RESOURCE_TYPE_ADDRESS32: {
            common = &resource->address32.common;
            m->base = resource->address32.minimum;
            m->length = resource->address32.address_length;
            break;
        }
        case UACPI_RESOURCE_TYPE_ADDRESS64: {
            common = &resource->address64.common;
            m->base = resource->address64.minimum;
            m->length = resource->address64.address_length;
            break;
        }
        case UACPI_RESOURCE_TYPE_ADDRESS64_EXTENDED: {
            common = &resource->address64_extended.common;
            m->base = resource->address64_extended.minimum;
            m->length = resource->address64_extended.address_length;
            break;
        }
        default: {
            return UACPI_ITERATION_DECISION_CONTINUE;
        }
    }
    if (common != NULL && common->type != UACPI_RANGE_MEMORY) {
        return UACPI_ITERATION_DECISION_CONTINUE;
    }
    m->found = true;
    return UACPI_ITERATION_DECISION_BREAK;
}

static uacpi_iteration_decision table_device(
    void *user,
    uacpi_namespace_node *node,
    uacpi_u32 depth
) {
    (void)depth;
    struct first_memory *m = user;
    uacpi_resources *resources;
    if (uacpi_get_current_resources(node, &resources) != UACPI_STATUS_OK) {
        return UACPI_ITERATION_DECISION_CONTINUE;
    }
    uacpi_for_each_resource(resources, memory_resource, m);
    uacpi_free_resources(resources);
    return m->found ? UACPI_ITERATION_DECISION_BREAK : UACPI_ITERATION_DECISION_CONTINUE;
}

void cbtable_find(void) {
    if (pci_host_scan()->table_count == 0) {
        return;
    }
    struct first_memory m = { .found = false };
    uacpi_find_devices_at(uacpi_namespace_root(), acpi_coreboot_table_ids, table_device, &m);
    if (!m.found || m.length == 0 || m.base + m.length < m.base) {
        return;
    }
    cbtable.found = true;
    cbtable.host = m.base;
    cbtable.room = m.length;
    cbtable.area = page_down(m.base);
    cbtable.area_size = page_up(m.base + m.length) - cbtable.area;
    // The guest finds nothing of the host's there, a table of its own or none.
    devmem_add_trapped(cbtable.area, cbtable.area + cbtable.area_size);
    mm_map_device(cbtable.area, cbtable.area_size);
    const char *fault = table_fault();
    if (fault != NULL) {
        printf(
            "coreboot's table at %#llx %s: the guest finds none there\n",
            (unsigned long long)m.base, fault
        );
        return;
    }
    cbtable.valid = true;
    const uint8_t *r = host_records();
    for (size_t i = 0, offset = 0; i < cbtable.entries; i++) {
        uint32_t size = read_le32(r + offset + 4);
        if (handling(read_le32(r + offset)) == HANDLING_DATA && size >= DATA_RECORD_SIZE) {
            uint64_t address = read_le64(r + offset + RECORD_ADDRESS);
            size_t data = data_size(address);
            if (data != 0) {
                devmem_add_read_only(address, address + data);
            }
        }
        offset += size;
    }
}

void cbtable_build(uint32_t rsdp) {
    uint64_t area_guest;
    if (!cbtable.found || cbtable.built
     || !devmem_trapped_guest(cbtable.area, cbtable.area + cbtable.area_size, &area_guest)) {
        return;
    }
    cbtable.built = true;
    if (mmio_register(area_guest, cbtable.area_size, &table_ops, NULL) < 0) {
        printf("No room to give the guest a coreboot table\n");
        return;
    }
    if (!cbtable.valid) {
        return;
    }
    size_t offset = cbtable.host - cbtable.area;
    size_t most = HEADER_SIZE + cbtable.table_bytes + RECORD_SIZE
                + E820_MAX_ENTRIES * MEMORY_RANGE_SIZE;
    cbtable.page_size = page_count(offset + most) * PAGE_SIZE;
    if (cbtable.page_size > cbtable.area_size) {
        cbtable.page_size = cbtable.area_size;
    }
    cbtable.page = page_alloc(cbtable.page_size / PAGE_SIZE);
    gpt_map(area_guest, (uintptr_t)cbtable.page, cbtable.page_size, GPT_R, MT_WB);
    gpt_flush();

    uint8_t *header = cbtable.page + offset;
    // As Linux 7.3 reads no more of a table than the room for it holds.
    size_t room = cbtable.room - HEADER_SIZE;
    if (room > cbtable.page_size - offset - HEADER_SIZE) {
        room = cbtable.page_size - offset - HEADER_SIZE;
    }
    struct building b = { .records = header + HEADER_SIZE, .room = room };
    bool serial = serial_kept();
    const uint8_t *r = host_records();
    for (size_t i = 0, at = 0; i < cbtable.entries; i++) {
        record_take(&b, r + at, serial, rsdp);
        at += read_le32(r + at + 4);
    }
    if (b.overflow) {
        memset(header, 0, HEADER_SIZE + b.size);
        printf(
            "coreboot's table at %#llx: the guest's does not fit the room for it, so the guest "
            "finds none\n",
            (unsigned long long)cbtable.host
        );
        return;
    }
    memcpy(header, "LBIO", 4);
    write_le32(header + HEADER_HEADER_BYTES, HEADER_SIZE);
    write_le32(header + HEADER_TABLE_BYTES, (uint32_t)b.size);
    write_le32(header + HEADER_TABLE_CHECKSUM, ip_checksum(b.records, b.size));
    write_le32(header + HEADER_TABLE_ENTRIES, b.entries);
    write_le32(header + HEADER_HEADER_CHECKSUM, ip_checksum(header, HEADER_SIZE));

    printf(
        "coreboot's table at %#llx: the guest finds one of its own at %#llx, with its memory map, "
        "%u of the firmware's %u records, and the firmware data that %zu of them point to\n",
        (unsigned long long)cbtable.host, (unsigned long long)(area_guest + offset), b.entries,
        cbtable.entries, b.data_count
    );
    if (b.dropped_count != 0) {
        // ", 0x" and 8 digits for each.
        char tags[DROPPED_SHOWN * 12 + 1];
        size_t length = 0;
        for (size_t i = 0; i < b.dropped_count; i++) {
            int n = snprintf(
                tags + length, sizeof(tags) - length, "%s%#x",
                i != 0 ? ", " : "", (unsigned int)b.dropped[i]
            );
            if (n > 0 && (size_t)n < sizeof(tags) - length) {
                length += (size_t)n;
            }
        }
        printf(
            "coreboot's table: the guest's leaves out the records of tags %s, which describe "
            "the host\n",
            tags
        );
    }
}
