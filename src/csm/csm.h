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

#ifndef CSM__CSM_H
#define CSM__CSM_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <handoff.h>

// The structures of the Intel CSM Specification 0.98 that the hypervisor fills
// in, as the CSM16 reads them, with the extensions of the SeaBIOS CSM16 that
// ReBIOS carries.

// The bytes "IFE$", which read as a dword spell "$EFI".
#define CSM_COMPAT16_SIGNATURE 0x24454649

// EFI_COMPATIBILITY16_TABLE.
struct csm_compat16_table {
    uint32_t signature;
    uint8_t table_checksum;
    uint8_t table_length;
    uint8_t efi_major_revision;
    uint8_t efi_minor_revision;
    uint8_t table_major_revision;
    uint8_t table_minor_revision;
    uint16_t reserved;
    uint16_t compatibility16_call_segment;
    uint16_t compatibility16_call_offset;
    uint16_t pnp_installation_check_segment;
    uint16_t pnp_installation_check_offset;
    uint32_t efi_system_table;
    uint32_t oem_id_string_pointer;
    uint32_t acpi_rsd_ptr_pointer;
    uint16_t oem_revision;
    uint32_t e820_pointer;
    uint32_t e820_length;
    uint32_t irq_routing_table_pointer;
    uint32_t irq_routing_table_length;
    uint32_t mp_table_ptr;
    uint32_t mp_table_length;
    uint16_t oem_int_segment;
    uint16_t oem_int_offset;
    uint16_t oem32_segment;
    uint16_t oem32_offset;
    uint16_t oem16_segment;
    uint16_t oem16_offset;
    uint16_t tpm_segment;
    uint16_t tpm_offset;
    uint32_t ibv_pointer;
    uint32_t pci_express_base;
    uint8_t last_pci_bus;
    uint32_t uma_address;
    uint32_t uma_size;
    uint32_t hi_permanent_memory_address;
    uint32_t hi_permanent_memory_size;
    // SeaBIOS's extension, there only if table_length covers it. A non-zero
    // pointer is the address of that many bus numbers, possibly none, those of
    // all the segment 0 root buses but bus 0, so that the CSM16 does not scan
    // for root buses itself.
    uint32_t extra_pci_root_list_pointer;
    uint8_t extra_pci_root_list_count;
    // Another extension: when it holds the value that identifies a
    // hypervisor's hypercall interface, the CSM16 may use that interface.
    uint32_t hypervisor_interface;
    // And another: the address of a boot order list in the format of
    // SeaBIOS's bootorder file, which the CSM16 then follows instead of the
    // BBS table's priorities, or 0.
    uint32_t boot_order_pointer;
} __attribute__((packed));

// The size of the specification's table, without SeaBIOS's extension.
#define CSM_COMPAT16_TABLE_SPEC_SIZE \
    offsetof(struct csm_compat16_table, extra_pci_root_list_pointer)

_Static_assert(offsetof(struct csm_compat16_table, table_length) == 5, "CSM16 table layout");
_Static_assert(offsetof(struct csm_compat16_table, compatibility16_call_segment) == 12,
               "CSM16 table layout");
_Static_assert(offsetof(struct csm_compat16_table, pnp_installation_check_segment) == 16,
               "CSM16 table layout");
_Static_assert(offsetof(struct csm_compat16_table, acpi_rsd_ptr_pointer) == 28,
               "CSM16 table layout");
_Static_assert(offsetof(struct csm_compat16_table, e820_pointer) == 34, "CSM16 table layout");
_Static_assert(offsetof(struct csm_compat16_table, mp_table_ptr) == 50, "CSM16 table layout");
_Static_assert(offsetof(struct csm_compat16_table, ibv_pointer) == 74, "CSM16 table layout");
_Static_assert(offsetof(struct csm_compat16_table, last_pci_bus) == 82, "CSM16 table layout");
_Static_assert(offsetof(struct csm_compat16_table, uma_address) == 83, "CSM16 table layout");
_Static_assert(offsetof(struct csm_compat16_table, hi_permanent_memory_size) == 95,
               "CSM16 table layout");
_Static_assert(CSM_COMPAT16_TABLE_SPEC_SIZE == 99, "CSM16 table layout");
_Static_assert(offsetof(struct csm_compat16_table, extra_pci_root_list_count) == 103,
               "CSM16 table layout");
_Static_assert(offsetof(struct csm_compat16_table, hypervisor_interface) == 104,
               "CSM16 table layout");
_Static_assert(offsetof(struct csm_compat16_table, boot_order_pointer) == 108,
               "CSM16 table layout");
_Static_assert(sizeof(struct csm_compat16_table) == 112, "CSM16 table layout");

// EFI_COMPATIBILITY_FUNCTIONS, which the CSM16 takes in AX.
#define CSM_COMPAT16_INITIALIZE_YOURSELF 0x0000
#define CSM_COMPAT16_UPDATE_BBS 0x0001
#define CSM_COMPAT16_PREPARE_TO_BOOT 0x0002
#define CSM_COMPAT16_BOOT 0x0003
#define CSM_COMPAT16_RETRIEVE_LAST_BOOT_DEVICE 0x0004
#define CSM_COMPAT16_DISPATCH_OPROM 0x0005
#define CSM_COMPAT16_GET_TABLE_ADDRESS 0x0006
#define CSM_COMPAT16_SET_KEYBOARD_LEDS 0x0007
#define CSM_COMPAT16_INSTALL_PCI_HANDLER 0x0008

// What they return in AX. CSM_STATUS_TBD is Compatibility16Boot()'s report of a
// missing or corrupted master boot record.
#define CSM_STATUS_SUCCESS 0x0000
#define CSM_STATUS_TBD 0x8000
#define CSM_STATUS_OUT_OF_RESOURCES 0x8009
#define CSM_STATUS_ABORTED 0x8015

// Where Compatibility16GetTableAddress() allocates, in BX. 0 means either.
#define CSM_TABLE_REGION_F0000 (1u << 0)
#define CSM_TABLE_REGION_E0000 (1u << 1)

// EFI_TO_COMPATIBILITY16_INIT_TABLE.
struct csm_init_table {
    uint32_t bios_less_than_1mb;
    uint32_t hi_pmm_memory;
    uint32_t hi_pmm_memory_size_in_bytes;
    uint16_t reverse_thunk_call_segment;
    uint16_t reverse_thunk_call_offset;
    uint32_t number_e820_entries;
    uint32_t os_memory_above_1mb;
    uint32_t thunk_start;
    uint32_t thunk_size_in_bytes;
    uint32_t low_pmm_memory;
    uint32_t low_pmm_memory_size_in_bytes;
} __attribute__((packed));

_Static_assert(offsetof(struct csm_init_table, reverse_thunk_call_segment) == 12,
               "CSM init table layout");
_Static_assert(offsetof(struct csm_init_table, thunk_start) == 24, "CSM init table layout");
_Static_assert(offsetof(struct csm_init_table, low_pmm_memory) == 32, "CSM init table layout");
_Static_assert(sizeof(struct csm_init_table) == 40, "CSM init table layout");

// DEVICE_PRODUCER_SERIAL.
struct csm_device_producer_serial {
    uint16_t address;
    uint8_t irq;
    // SERIAL_MODE.
    uint8_t mode;
} __attribute__((packed));

// DEVICE_PRODUCER_PARALLEL.
struct csm_device_producer_parallel {
    uint16_t address;
    uint8_t irq;
    uint8_t dma;
    // PARALLEL_MODE.
    uint8_t mode;
} __attribute__((packed));

// DEVICE_PRODUCER_FLOPPY.
struct csm_device_producer_floppy {
    uint16_t address;
    uint8_t irq;
    uint8_t dma;
    uint8_t number_of_floppy;
} __attribute__((packed));

// LEGACY_DEVICE_FLAGS.
#define CSM_LEGACY_DEVICE_A20_KYBD (1u << 0)
#define CSM_LEGACY_DEVICE_A20_PORT92 (1u << 1)

// DEVICE_PRODUCER_DATA_HEADER.
struct csm_device_producer_data_header {
    struct csm_device_producer_serial serial[4];
    struct csm_device_producer_parallel parallel[3];
    struct csm_device_producer_floppy floppy;
    uint8_t mouse_present;
    // LEGACY_DEVICE_FLAGS.
    uint32_t flags;
} __attribute__((packed));

_Static_assert(sizeof(struct csm_device_producer_serial) == 4, "CSM SIO data layout");
_Static_assert(sizeof(struct csm_device_producer_parallel) == 5, "CSM SIO data layout");
_Static_assert(sizeof(struct csm_device_producer_floppy) == 5, "CSM SIO data layout");
_Static_assert(offsetof(struct csm_device_producer_data_header, floppy) == 31,
               "CSM SIO data layout");
_Static_assert(sizeof(struct csm_device_producer_data_header) == 41, "CSM SIO data layout");

// ATAPI_IDENTIFY.
struct csm_atapi_identify {
    uint16_t raw[256];
} __attribute__((packed));

// HDD_INFO.
struct csm_hdd_info {
    uint16_t status;
    uint32_t bus;
    uint32_t device;
    uint32_t function;
    uint16_t command_base_address;
    uint16_t control_base_address;
    uint16_t bus_master_address;
    uint8_t hdd_irq;
    struct csm_atapi_identify identify_drive[2];
} __attribute__((packed));

_Static_assert(offsetof(struct csm_hdd_info, identify_drive) == 21, "CSM HDD info layout");
_Static_assert(sizeof(struct csm_hdd_info) == 1045, "CSM HDD info layout");

// The specification does not give these; they are the values of its reference
// implementation, EDK2.
#define CSM_MAX_IDE_CONTROLLER 8
#define CSM_BOOT_TABLE_MAJOR_VERSION 0x02
#define CSM_BOOT_TABLE_MINOR_VERSION 0x00

// EFI_TO_COMPATIBILITY16_BOOT_TABLE.
struct csm_boot_table {
    uint16_t major_version;
    uint16_t minor_version;
    uint32_t acpi_table;
    uint32_t smbios_table;
    uint32_t smbios_table_length;
    struct csm_device_producer_data_header sio_data;
    uint16_t device_path_type;
    uint16_t pci_irq_mask;
    uint32_t number_e820_entries;
    struct csm_hdd_info hdd_info[CSM_MAX_IDE_CONTROLLER];
    uint32_t number_bbs_entries;
    uint32_t bbs_table;
    uint32_t smm_table;
    uint32_t os_memory_above_1mb;
    uint32_t unconventional_device_table;
    // SeaBIOS's extension: the address, below 4 GiB, of an Intel MultiProcessor
    // Specification 1.4 floating pointer structure, which the CSM16 copies with
    // its configuration table into its F-segment if the two total at most
    // 4 KiB.
    uint32_t mp_table;
} __attribute__((packed));

_Static_assert(offsetof(struct csm_boot_table, sio_data) == 16, "CSM boot table layout");
_Static_assert(offsetof(struct csm_boot_table, number_e820_entries) == 61,
               "CSM boot table layout");
_Static_assert(offsetof(struct csm_boot_table, hdd_info) == 65, "CSM boot table layout");
_Static_assert(offsetof(struct csm_boot_table, number_bbs_entries) == 8425,
               "CSM boot table layout");
_Static_assert(offsetof(struct csm_boot_table, unconventional_device_table) == 8441,
               "CSM boot table layout");
_Static_assert(offsetof(struct csm_boot_table, mp_table) == 8445, "CSM boot table layout");
_Static_assert(sizeof(struct csm_boot_table) == 8449, "CSM boot table layout");

// BBS_TABLE, which is one entry of the table.
struct csm_bbs_entry {
    uint16_t boot_priority;
    uint32_t bus;
    uint32_t device;
    uint32_t function;
    uint8_t class;
    uint8_t subclass;
    uint16_t mfg_string_offset;
    uint16_t mfg_string_segment;
    uint16_t device_type;
    // BBS_STATUS_FLAGS.
    uint16_t status_flags;
    uint16_t boot_handler_offset;
    uint16_t boot_handler_segment;
    uint16_t desc_string_offset;
    uint16_t desc_string_segment;
    uint32_t init_per_reserved;
    uint32_t additional_irq13_handler;
    uint32_t additional_irq18_handler;
    uint32_t additional_irq19_handler;
    uint32_t additional_irq40_handler;
    uint8_t assigned_drive_number;
    uint32_t additional_irq41_handler;
    uint32_t additional_irq46_handler;
    uint32_t ibv1;
    uint32_t ibv2;
} __attribute__((packed));

_Static_assert(offsetof(struct csm_bbs_entry, class) == 14, "BBS table layout");
_Static_assert(offsetof(struct csm_bbs_entry, status_flags) == 22, "BBS table layout");
_Static_assert(offsetof(struct csm_bbs_entry, assigned_drive_number) == 52, "BBS table layout");
_Static_assert(offsetof(struct csm_bbs_entry, ibv2) == 65, "BBS table layout");
_Static_assert(sizeof(struct csm_bbs_entry) == 69, "BBS table layout");

// BBS_STATUS_FLAGS.
#define CSM_BBS_STATUS_OLD_POSITION_MASK 0x000fu
#define CSM_BBS_STATUS_ENABLED (1u << 8)
#define CSM_BBS_STATUS_FAILED (1u << 9)
#define CSM_BBS_STATUS_MEDIA_NONE (0u << 10)
#define CSM_BBS_STATUS_MEDIA_UNKNOWN (1u << 10)
#define CSM_BBS_STATUS_MEDIA_PRESENT (2u << 10)
#define CSM_BBS_STATUS_MEDIA_MASK (3u << 10)

// Boot priorities, of which 0 is the highest.
#define CSM_BBS_DO_NOT_BOOT_FROM 0xfffc
#define CSM_BBS_LOWEST_PRIORITY 0xfffd
#define CSM_BBS_UNPRIORITIZED_ENTRY 0xfffe
#define CSM_BBS_IGNORE_ENTRY 0xffff

// Device types.
#define CSM_BBS_FLOPPY 0x01
#define CSM_BBS_HARDDISK 0x02
#define CSM_BBS_CDROM 0x03
#define CSM_BBS_PCMCIA 0x04
#define CSM_BBS_USB 0x05
#define CSM_BBS_EMBED_NETWORK 0x06
#define CSM_BBS_BEV_DEVICE 0x80
#define CSM_BBS_UNKNOWN 0xff

// The CSM16 looks entries up by position: entry 0 is the legacy floppy
// controller, and entries 1 to 4 the onboard IDE channels' primary master,
// primary slave, secondary master, and secondary slave. Block devices follow.
#define CSM_BBS_IDE_FIRST 1
#define CSM_BBS_DEVICE_FIRST 5
#define CSM_BBS_MAX_ENTRIES (CSM_BBS_DEVICE_FIRST + HANDOFF_MAX_BLOCK_DEVICES)

// EFI_DISPATCH_OPROM_TABLE.
struct csm_oprom_table {
    uint16_t pnp_installation_check_segment;
    uint16_t pnp_installation_check_offset;
    uint16_t oprom_segment;
    uint8_t pci_bus;
    uint8_t pci_device_function;
    uint8_t number_bbs_entries;
    uint32_t bbs_table_pointer;
    uint16_t oprom_destination_segment;
} __attribute__((packed));

_Static_assert(offsetof(struct csm_oprom_table, number_bbs_entries) == 8,
               "CSM option ROM table layout");
_Static_assert(offsetof(struct csm_oprom_table, bbs_table_pointer) == 9,
               "CSM option ROM table layout");
_Static_assert(sizeof(struct csm_oprom_table) == 15, "CSM option ROM table layout");

// Guest addresses and sizes.
struct csm_init_params {
    // Memory the CSM16 allocates from, below 1 MiB and below 4 GiB.
    uint32_t low_pmm;
    uint32_t low_pmm_size;
    uint32_t high_pmm;
    uint32_t high_pmm_size;
    // The hypervisor's calling code and these tables.
    uint32_t thunk;
    uint32_t thunk_size;
};

// Guest addresses, 0 for a table there is none of, and the tables' sizes.
struct csm_boot_params {
    uint32_t rsdp;
    // An SMBIOS 2.x entry point, and the length of its structure table.
    uint32_t smbios_entry;
    uint32_t smbios_table_length;
    uint32_t bbs_table;
    uint32_t bbs_entry_count;
    // An MP floating pointer structure.
    uint32_t mp_floating_pointer;
    uint32_t e820_entry_count;
};

struct csm_oprom_params {
    // Holds the PnP installation check pointer.
    const struct csm_compat16_table *compat16;
    // Where the option ROM was copied to in the guest.
    uint16_t segment;
    // Of the ROM's device, in segment 0.
    struct handoff_pci_address address;
    uint32_t bbs_table;
    uint32_t bbs_entry_count;
};

// Finds the CSM16 table in a CSM16 image, as the specification says the
// firmware does. Returns NULL if there is none. The image's guest address must
// be 16-byte aligned. The extension fields are only valid if table_length is
// at least sizeof(struct csm_compat16_table).
struct csm_compat16_table *csm_find_compat16_table(void *image, uint64_t size);

// Recomputes the CSM16 table's checksum after its fields were changed.
void csm_compat16_table_checksum(struct csm_compat16_table *table);

// Copies an ACPI RSDP into the CSM16 image, where the CSM16 table's
// AcpiRsdPtrPointer says it goes.
bool csm_copy_rsdp(void *image, uint64_t image_guest_base, uint64_t image_size,
                   const struct csm_compat16_table *table, const void *rsdp);

void csm_fill_init_table(struct csm_init_table *table, const struct csm_init_params *params);
void csm_fill_boot_table(struct csm_boot_table *table, const struct csm_boot_params *params);

// Fills a BBS table from the firmware's block devices. Returns the number of
// entries used.
uint32_t csm_fill_bbs_table(struct csm_bbs_entry *table, uint32_t max_entries,
                            const struct handoff_block_device *devices, uint32_t device_count);

void csm_fill_oprom_table(struct csm_oprom_table *table, const struct csm_oprom_params *params);

#endif
