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
#include <csm/csm.h>
#include <handoff.h>
#include <memory.h>

#define PCI_CLASS_MASS_STORAGE 0x01
#define PCI_SUBCLASS_IDE 0x01

// A sanity bound, well above the 36 bytes of every ACPI revision so far.
#define RSDP_MAX_SIZE 64

static uint8_t byte_sum(const void *p, size_t len) {
    const uint8_t *bytes = p;
    uint8_t sum = 0;
    for (size_t i = 0; i < len; i++) {
        sum += bytes[i];
    }
    return sum;
}

struct csm_compat16_table *csm_find_compat16_table(void *image, size_t size) {
    uint8_t *bytes = image;
    for (size_t off = 0; size - off >= CSM_COMPAT16_TABLE_SPEC_SIZE;
         off += 16) {
        struct csm_compat16_table *table = (struct csm_compat16_table *)(bytes
            + off);
        if (table->signature == CSM_COMPAT16_SIGNATURE
         && table->table_length >= CSM_COMPAT16_TABLE_SPEC_SIZE
         && table->table_length <= size - off
         && byte_sum(table, table->table_length) == 0) {
            return table;
        }
    }
    return NULL;
}

void csm_compat16_table_checksum(struct csm_compat16_table *table) {
    table->table_checksum = 0;
    table->table_checksum = (uint8_t)-byte_sum(table, table->table_length);
}

bool csm_copy_rsdp(
    void *image, uintptr_t image_guest_base, size_t image_size,
    const struct csm_compat16_table *table, const void *rsdp
) {
    const struct acpi_rsdp *src = rsdp;

    // Before revision 2, only the ACPI 1.0 fields, those ahead of the length,
    // are valid.
    size_t length = offsetof(struct acpi_rsdp, length);
    if (src->revision >= 2) {
        length = src->length;
        if (length < sizeof(struct acpi_rsdp) || length > RSDP_MAX_SIZE) {
            return false;
        }
    }

    uint64_t dest = table->acpi_rsd_ptr_pointer;
    if (dest < image_guest_base || dest - image_guest_base > image_size
     || image_size - (dest - image_guest_base) < length) {
        return false;
    }
    memcpy((uint8_t *)image + (dest - image_guest_base), src, length);
    return true;
}

void csm_fill_init_table(
    struct csm_init_table *table, const struct csm_init_params *params
) {
    memset(table, 0, sizeof(struct csm_init_table));
    // Kept by the Intel CSM Specification 0.98 for older CSM16s: where the low
    // PMM block starts.
    table->bios_less_than_1mb = params->low_pmm;
    table->hi_pmm_memory = params->high_pmm;
    table->hi_pmm_memory_size_in_bytes = params->high_pmm_size;
    table->thunk_start = params->thunk;
    table->thunk_size_in_bytes = params->thunk_size;
    table->low_pmm_memory = params->low_pmm;
    table->low_pmm_memory_size_in_bytes = params->low_pmm_size;
}

void csm_fill_boot_table(
    struct csm_boot_table *table, const struct csm_boot_params *params
) {
    memset(table, 0, sizeof(struct csm_boot_table));
    table->major_version = CSM_BOOT_TABLE_MAJOR_VERSION;
    table->minor_version = CSM_BOOT_TABLE_MINOR_VERSION;
    table->acpi_table = params->rsdp;
    table->smbios_table = params->smbios_entry;
    table->smbios_table_length = params->smbios_table_length;
    table->number_e820_entries = params->e820_entry_count;
    table->number_bbs_entries = params->bbs_entry_count;
    table->bbs_table = params->bbs_table;
    table->mp_table = params->mp_floating_pointer;
}

static void bbs_describe(
    struct csm_bbs_entry *entry, const struct handoff_block_device *device,
    uint16_t priority
) {
    memset(entry, 0, sizeof(struct csm_bbs_entry));
    entry->boot_priority = priority;
    entry->bus = device->address.bus;
    entry->device = device->address.devfn >> 3;
    entry->function = device->address.devfn & 7;
    entry->class = device->base_class;
    entry->subclass = device->subclass;
    entry->device_type = device->bbs_type;
    entry->status_flags = CSM_BBS_STATUS_ENABLED
        | CSM_BBS_STATUS_MEDIA_PRESENT;
}

static void bbs_ignore(struct csm_bbs_entry *entry) {
    memset(entry, 0, sizeof(struct csm_bbs_entry));
    entry->boot_priority = CSM_BBS_IGNORE_ENTRY;
}

size_t csm_fill_bbs_table(
    struct csm_bbs_entry *table, size_t max_entries,
    const struct handoff_block_device *devices, size_t device_count
) {
    size_t used = CSM_BBS_DEVICE_FIRST;
    uint16_t next_priority = 1;
    const struct handoff_block_device *ide = NULL;
    uint16_t ide_priority = 0;

    // The boot devices first, then the others, each in the records' order.
    for (size_t pass = 0; pass < 2; pass++) {
        bool boot_pass = pass == 0;
        for (size_t i = 0; i < device_count; i++) {
            const struct handoff_block_device *device = &devices[i];
            if (device->address.segment != 0 || device->boot != boot_pass) {
                continue;
            }
            uint16_t priority = boot_pass ? 0 : next_priority++;
            if (ide == NULL && device->base_class == PCI_CLASS_MASS_STORAGE
             && device->subclass == PCI_SUBCLASS_IDE) {
                ide = device;
                ide_priority = priority;
            }
            if (used < max_entries) {
                bbs_describe(&table[used++], device, priority);
            }
        }
    }

    // The floppy controller's entry is always ignored, and the IDE channels'
    // entries are unless there is an IDE controller to describe.
    for (size_t i = 0; i < CSM_BBS_DEVICE_FIRST && i < max_entries; i++) {
        if (i >= CSM_BBS_IDE_FIRST && ide != NULL) {
            bbs_describe(&table[i], ide, ide_priority);
        } else {
            bbs_ignore(&table[i]);
        }
    }

    return used < max_entries ? used : max_entries;
}

void csm_fill_oprom_table(
    struct csm_oprom_table *table, const struct csm_oprom_params *params
) {
    memset(table, 0, sizeof(struct csm_oprom_table));
    table->pnp_installation_check_segment
        = params->compat16->pnp_installation_check_segment;
    table->pnp_installation_check_offset
        = params->compat16->pnp_installation_check_offset;
    table->oprom_segment = params->segment;
    table->pci_bus = params->address.bus;
    table->pci_device_function = params->address.devfn;
    // The table counts entries in a byte.
    table->number_bbs_entries = UINT8_MAX;
    if (params->bbs_entry_count < UINT8_MAX) {
        table->number_bbs_entries = params->bbs_entry_count;
    }
    table->bbs_table_pointer = params->bbs_table;
}
