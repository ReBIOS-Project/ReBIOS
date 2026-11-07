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
#include <efi.h>
#include <loader/fwtables.h>
#include <handoff.h>
#include <lib/log.h>
#include <loader/efi_glue.h>
#include <loader/loader.h>
#include <memory.h>

#define FOUR_GIB ((uint64_t)1 << 32)

struct rsdp {
    char signature[8];
    uint8_t checksum;
    char oem_id[6];
    uint8_t revision;
    uint32_t rsdt_address;
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t extended_checksum;
    uint8_t reserved[3];
} __attribute__((packed));

_Static_assert(sizeof(struct rsdp) == 36, "RSDP layout");

// What the ACPI 1.0 checksum covers, and all a revision 0 RSDP has.
#define RSDP_V1_SIZE 20

// Far beyond any revision's RSDP, so that a corrupt length cannot send the
// checksum on through whatever follows it.
#define RSDP_MAX_SIZE 4096

#define ACPI_TABLE_LENGTH 4
#define ACPI_TABLE_REVISION 8
#define ACPI_TABLE_HEADER_SIZE 36
// ACPI 6.6, 5.2.9.
#define FADT_PREFERRED_PM_PROFILE 45
// Microsoft's SPCR (table revision 4) and DBG2 (2023-04-10) specifications.
#define SPCR_INTERFACE_TYPE 36
#define SPCR_BASE_ADDRESS 40
#define DBG2_DEVICE_OFFSET 36
#define DBG2_DEVICE_COUNT 40
#define DBG2_DEVICE_LENGTH 1
#define DBG2_DEVICE_PORT_TYPE 12
#define DBG2_DEVICE_PORT_SUBTYPE 14
#define DBG2_DEVICE_BASE_OFFSET 18
#define DBG2_DEVICE_MIN_LENGTH 22
#define DBG2_PORT_SERIAL 0x8000
// A Generic Address Structure: its address space, then its address.
#define GAS_SIZE 12
#define GAS_SPACE 0
#define GAS_ADDRESS 4
#define GAS_SPACE_SYSTEM_IO 1
// What 16550s and 16450s a table may name: the full one, the subset that
// DBGP's revision 1 describes, and one with its parameters in the GAS, as
// DBG2's Table 3 has them, which the SPCR's revisions after 1 share. Its
// revision 1 has only the first two.
#define UART_FULL_16550 0x0000
#define UART_16550_SUBSET 0x0001
#define UART_16550_GAS 0x0012

struct smbios2_entry {
    char anchor[4];
    uint8_t checksum;
    uint8_t length;
    uint8_t major;
    uint8_t minor;
    uint16_t max_struct_size;
    uint8_t revision;
    uint8_t formatted[5];
    char intermediate_anchor[5];
    uint8_t intermediate_checksum;
    uint16_t table_length;
    uint32_t table_address;
    uint16_t struct_count;
    uint8_t bcd_revision;
} __attribute__((packed));

_Static_assert(sizeof(struct smbios2_entry) == 0x1f, "SMBIOS 2.1 entry point layout");

// SMBIOS 2.1 gave the entry point's length as 1Eh, short of its BCD revision,
// so 2.1 entry points may say 1Eh or 1Fh (DSP0134 3.9.0, 5.2.1), and Linux
// 7.3's dmi_present() takes 1Eh to 20h. The intermediate checksum covers 0Fh
// bytes from 10h, the BCD revision among them, whatever the length says.
#define SMBIOS2_MIN_LENGTH 0x1e
#define SMBIOS2_MAX_LENGTH 0x20
#define SMBIOS2_INTERMEDIATE_SIZE 0x0f

struct smbios3_entry {
    char anchor[5];
    uint8_t checksum;
    uint8_t length;
    uint8_t major;
    uint8_t minor;
    uint8_t docrev;
    uint8_t revision;
    uint8_t reserved;
    uint32_t table_max_size;
    uint64_t table_address;
} __attribute__((packed));

_Static_assert(sizeof(struct smbios3_entry) == 0x18, "SMBIOS 3.0 entry point layout");

#define SMBIOS_END_OF_TABLE 127

static const EFI_GUID acpi_20_guid = EFI_ACPI_20_TABLE_GUID;
static const EFI_GUID acpi_10_guid = EFI_ACPI_10_TABLE_GUID;
static const EFI_GUID smbios_guid = EFI_SMBIOS_TABLE_GUID;
static const EFI_GUID smbios3_guid = EFI_SMBIOS3_TABLE_GUID;

static void *config_table_find(const EFI_GUID *guid) {
    for (size_t i = 0; i < efi_st->NumberOfTableEntries; i++) {
        EFI_CONFIGURATION_TABLE *entry = &efi_st->ConfigurationTable[i];
        if (memcmp(&entry->VendorGuid, guid, sizeof(EFI_GUID)) == 0) {
            return entry->VendorTable;
        }
    }
    return NULL;
}

static uint8_t byte_sum(const void *data, size_t size) {
    const uint8_t *bytes = data;
    uint8_t sum = 0;
    for (size_t i = 0; i < size; i++) {
        sum += bytes[i];
    }
    return sum;
}

static size_t page_count(size_t size) {
    return (size + 4095) / 4096;
}

// Returns why the RSDP is invalid, or NULL.
static const char *rsdp_check(const struct rsdp *rsdp) {
    if (memcmp(rsdp->signature, "RSD PTR ", sizeof(rsdp->signature)) != 0) {
        return "bad signature";
    }
    if (byte_sum(rsdp, RSDP_V1_SIZE) != 0) {
        return "bad checksum";
    }
    if (rsdp->revision >= 2) {
        if (rsdp->length < sizeof(struct rsdp) || rsdp->length > RSDP_MAX_SIZE) {
            return "bad length";
        }
        if (byte_sum(rsdp, rsdp->length) != 0) {
            return "bad extended checksum";
        }
    }
    return NULL;
}

static uint64_t rsdp_find(const EFI_GUID *guid, const char *version) {
    const struct rsdp *rsdp = config_table_find(guid);
    if (rsdp == NULL) {
        return 0;
    }
    const char *problem = rsdp_check(rsdp);
    if (problem != NULL) {
        printf(
            "Ignoring the ACPI %s RSDP at %#llx: %s\n", version,
            (unsigned long long)(uintptr_t)rsdp, problem
        );
        return 0;
    }
    printf(
        "ACPI RSDP revision %u at %#llx\n", rsdp->revision, (unsigned long long)(uintptr_t)rsdp
    );
    return (uintptr_t)rsdp;
}

// Returns why the entry point is invalid, or NULL.
static const char *smbios2_check(const struct smbios2_entry *eps) {
    size_t intermediate = offsetof(struct smbios2_entry, intermediate_anchor);
    if (memcmp(eps->anchor, "_SM_", sizeof(eps->anchor)) != 0) {
        return "bad anchor";
    }
    if (eps->length < SMBIOS2_MIN_LENGTH || eps->length > SMBIOS2_MAX_LENGTH) {
        return "bad length";
    }
    if (byte_sum(eps, eps->length) != 0) {
        return "bad checksum";
    }
    int anchor = memcmp(eps->intermediate_anchor, "_DMI_", sizeof(eps->intermediate_anchor));
    if (anchor != 0) {
        return "bad intermediate anchor";
    }
    uint8_t sum = byte_sum((const uint8_t *)eps + intermediate, SMBIOS2_INTERMEDIATE_SIZE);
    if (sum != 0) {
        return "bad intermediate checksum";
    }
    return NULL;
}

// Returns why the entry point is invalid, or NULL.
static const char *smbios3_check(const struct smbios3_entry *eps) {
    if (memcmp(eps->anchor, "_SM3_", sizeof(eps->anchor)) != 0) {
        return "bad anchor";
    }
    if (eps->length < sizeof(struct smbios3_entry)) {
        return "bad length";
    }
    if (byte_sum(eps, eps->length) != 0) {
        return "bad checksum";
    }
    return NULL;
}

static void smbios2_checksum(struct smbios2_entry *eps) {
    size_t intermediate = offsetof(struct smbios2_entry, intermediate_anchor);
    eps->intermediate_checksum = 0;
    eps->intermediate_checksum = (uint8_t)-byte_sum(
        (uint8_t *)eps + intermediate, SMBIOS2_INTERMEDIATE_SIZE
    );
    eps->checksum = 0;
    eps->checksum = (uint8_t)-byte_sum(eps, eps->length);
}

static void smbios_log(const char *how, const struct smbios2_entry *eps) {
    printf(
        "SMBIOS %u.%u, 32-bit entry point %s %#llx, structure table at %#llx (%u bytes, %u "
        "structure%s)\n", eps->major, eps->minor, how, (unsigned long long)(uintptr_t)eps,
        (unsigned long long)eps->table_address, eps->table_length, eps->struct_count,
        eps->struct_count == 1 ? "" : "s"
    );
}

static uint64_t smbios2_use(const struct smbios2_entry *eps) {
    uintptr_t eps_addr = (uintptr_t)eps;
    uintptr_t table = eps->table_address;
    // A 1Eh-byte entry point goes to a copy with its BCD revision, past that
    // length, and the length that 2.2 corrected, from which SeaBIOS's checks
    // take the intermediate checksum's size.
    size_t size = eps->length > sizeof(struct smbios2_entry)
                ? eps->length
                : sizeof(struct smbios2_entry);
    if (eps->length == size && eps_addr + size <= FOUR_GIB
     && table + eps->table_length <= FOUR_GIB) {
        smbios_log("at", eps);
        return eps_addr;
    }

    uintptr_t copy = efi_alloc_pages_typed(
        page_count(size + eps->table_length), UINT32_MAX, EfiRuntimeServicesData
    );
    if (copy == 0) {
        printf("Cannot allocate memory below 4GiB for the SMBIOS tables\n");
        return 0;
    }
    struct smbios2_entry *new_eps = (struct smbios2_entry *)(uintptr_t)copy;
    memcpy(new_eps, eps, size);
    memcpy((uint8_t *)new_eps + size, (const void *)(uintptr_t)table, eps->table_length);
    new_eps->length = (uint8_t)size;
    new_eps->table_address = copy + size;
    smbios2_checksum(new_eps);
    smbios_log("copied to", new_eps);
    return copy;
}

struct smbios_walk {
    size_t length;
    size_t count;
    uint32_t largest;
    bool truncated;
};

// Structures that would end beyond the 16-bit length of a 2.x entry point are
// left out, with those after them, and the ones kept then fit its 16-bit
// structure count and maximum structure size as well.
static void smbios_walk(const uint8_t *table, size_t max_size, struct smbios_walk *walk) {
    memset(walk, 0, sizeof(struct smbios_walk));

    size_t off = 0;
    while (max_size - off >= 4) {
        uint8_t type = table[off];
        size_t length = table[off + 1];
        if (length < 4 || length > max_size - off) {
            break;
        }

        uintptr_t end = off + length;
        while (end + 1 < max_size && (table[end] != 0 || table[end + 1] != 0)) {
            end++;
        }
        if (end + 1 >= max_size) {
            break;
        }
        end += 2;

        if (end > UINT16_MAX) {
            walk->truncated = true;
            break;
        }
        walk->length = end;
        walk->count++;
        if (end - off > walk->largest) {
            walk->largest = end - off;
        }
        if (type == SMBIOS_END_OF_TABLE) {
            break;
        }
        off = end;
    }
}

static uint64_t smbios3_synthesise(const struct smbios3_entry *eps3) {
    uintptr_t table = eps3->table_address;
    size_t max_size = eps3->table_max_size;

    struct smbios_walk walk;
    smbios_walk((const uint8_t *)(uintptr_t)table, max_size, &walk);
    if (walk.count == 0) {
        printf(
            "The SMBIOS structure table at %#llx holds no structures\n", (unsigned long long)table
        );
        return 0;
    }
    if (walk.truncated) {
        printf(
            "Cutting the SMBIOS structure table to %zu bytes and %zu structure%s to fit a 32-bit "
            "entry point\n", walk.length, walk.count, walk.count == 1 ? "" : "s"
        );
    }

    uintptr_t addr = efi_alloc_pages_typed(
        page_count(sizeof(struct smbios2_entry) + walk.length), UINT32_MAX, EfiRuntimeServicesData
    );
    if (addr == 0) {
        printf("Cannot allocate memory below 4GiB for the SMBIOS tables\n");
        return 0;
    }

    struct smbios2_entry *eps = (struct smbios2_entry *)(uintptr_t)addr;
    memset(eps, 0, sizeof(struct smbios2_entry));
    memcpy(eps->anchor, "_SM_", sizeof(eps->anchor));
    eps->length = sizeof(struct smbios2_entry);
    eps->major = eps3->major;
    eps->minor = eps3->minor;
    eps->max_struct_size = walk.largest;
    memcpy(eps->intermediate_anchor, "_DMI_", sizeof(eps->intermediate_anchor));
    eps->table_length = walk.length;
    eps->table_address = addr + sizeof(struct smbios2_entry);
    eps->struct_count = walk.count;
    eps->bcd_revision = (eps3->major % 10) << 4 | eps3->minor % 10;
    memcpy(
        (uint8_t *)eps + sizeof(struct smbios2_entry), (const void *)(uintptr_t)table, walk.length
    );
    smbios2_checksum(eps);
    smbios_log("synthesised at", eps);
    return addr;
}

// The structure table the firmware's identification strings are read from, the
// 3.x entry point's first, as Linux 7.3's dmi_scan_machine() prefers it.
static const uint8_t *smbios_structures(uint64_t *size) {
    const struct smbios3_entry *eps3 = config_table_find(&smbios3_guid);
    if (eps3 != NULL && smbios3_check(eps3) == NULL) {
        *size = eps3->table_max_size;
        return (const uint8_t *)(uintptr_t)eps3->table_address;
    }
    const struct smbios2_entry *eps2 = config_table_find(&smbios_guid);
    if (eps2 != NULL && smbios2_check(eps2) == NULL) {
        *size = eps2->table_length;
        return (const uint8_t *)(uintptr_t)eps2->table_address;
    }
    return NULL;
}

// The firmware's first SMBIOS structure of type, and the first of the two NULs
// its strings end in.
static const uint8_t *smbios_structure(uint8_t type, const char **strings_end) {
    size_t max_size;
    const uint8_t *table = smbios_structures(&max_size);
    if (table == NULL) {
        return NULL;
    }

    size_t off = 0;
    while (max_size - off >= 4) {
        const uint8_t *structure = table + off;
        size_t length = structure[1];
        if (length < 4 || length > max_size - off) {
            return NULL;
        }
        uintptr_t end = off + length;
        while (end + 1 < max_size && (table[end] != 0 || table[end + 1] != 0)) {
            end++;
        }
        if (end + 1 >= max_size) {
            return NULL;
        }

        if (structure[0] == type) {
            *strings_end = (const char *)table + end;
            return structure;
        }
        if (structure[0] == SMBIOS_END_OF_TABLE) {
            return NULL;
        }
        off = end + 2;
    }
    return NULL;
}

const char *fwtables_smbios_string(uint8_t type, uint8_t offset) {
    const char *strings_end;
    const uint8_t *structure = smbios_structure(type, &strings_end);
    if (structure == NULL || offset >= structure[1]) {
        return NULL;
    }

    const char *string = (const char *)structure + structure[1];
    for (size_t s = structure[offset]; s > 1 && string < strings_end; s--) {
        while (*string != 0) {
            string++;
        }
        string++;
    }
    const char *nonblank = string;
    while (nonblank < strings_end && *nonblank == ' ') {
        nonblank++;
    }
    if (structure[offset] == 0 || nonblank >= strings_end || *nonblank == 0) {
        return "";
    }
    return string;
}

static bool root_sound(uintptr_t address, const char *signature) {
    const uint8_t *root = (const uint8_t *)address;
    if (root == NULL || memcmp(root, signature, 4) != 0) {
        return false;
    }
    uint32_t length;
    memcpy(&length, root + ACPI_TABLE_LENGTH, sizeof(uint32_t));
    return length >= ACPI_TABLE_HEADER_SIZE && byte_sum(root, length) == 0;
}

// The XSDT, or the RSDT where the XSDT is broken, as Linux 7.3 falls back for
// boards with corrupt XSDTs (arch/x86/kernel/acpi/boot.c, disable_acpi_xsdt()
// and acpi=rsdt), and NULL where neither is sound.
static const uint8_t *acpi_root(const struct rsdp *rsdp, size_t *entry_size) {
    if (rsdp->revision >= 2 && rsdp->xsdt_address != 0
     && root_sound(rsdp->xsdt_address, "XSDT")) {
        *entry_size = sizeof(uint64_t);
        return (const uint8_t *)(uintptr_t)rsdp->xsdt_address;
    }
    if (root_sound(rsdp->rsdt_address, "RSDT")) {
        *entry_size = sizeof(uint32_t);
        return (const uint8_t *)(uintptr_t)rsdp->rsdt_address;
    }
    return NULL;
}

bool fwtables_fadt_pm_profile(uint8_t *profile) {
    const struct rsdp *rsdp = (const struct rsdp *)(uintptr_t)loader_handoff->acpi_rsdp;
    if (rsdp == NULL) {
        return false;
    }
    size_t entry_size;
    const uint8_t *root = acpi_root(rsdp, &entry_size);
    if (root == NULL) {
        return false;
    }

    uint32_t length;
    memcpy(&length, root + ACPI_TABLE_LENGTH, sizeof(uint32_t));
    for (size_t off = ACPI_TABLE_HEADER_SIZE; off + entry_size <= length; off += entry_size) {
        uintptr_t address = 0;
        memcpy(&address, root + off, entry_size);
        const uint8_t *table = (const uint8_t *)address;
        if (table == NULL || memcmp(table, "FACP", 4) != 0) {
            continue;
        }
        uint32_t table_length;
        memcpy(&table_length, table + ACPI_TABLE_LENGTH, sizeof(uint32_t));
        if (table_length <= FADT_PREFERRED_PM_PROFILE) {
            return false;
        }
        *profile = table[FADT_PREFERRED_PM_PROFILE];
        return true;
    }
    return false;
}

// A table the RSDP in the firmware's configuration tables leads to.
static const uint8_t *acpi_table(const char *signature) {
    const struct rsdp *rsdp = config_table_find(&acpi_20_guid);
    if (rsdp == NULL || rsdp_check(rsdp) != NULL) {
        rsdp = config_table_find(&acpi_10_guid);
        if (rsdp == NULL || rsdp_check(rsdp) != NULL) {
            return NULL;
        }
    }
    size_t entry_size;
    const uint8_t *root = acpi_root(rsdp, &entry_size);
    if (root == NULL) {
        return NULL;
    }
    uint32_t length;
    memcpy(&length, root + ACPI_TABLE_LENGTH, sizeof(uint32_t));
    for (size_t off = ACPI_TABLE_HEADER_SIZE; off + entry_size <= length; off += entry_size) {
        uintptr_t address = 0;
        memcpy(&address, root + off, entry_size);
        const uint8_t *table = (const uint8_t *)address;
        if (table == NULL || memcmp(table, signature, 4) != 0) {
            continue;
        }
        uint32_t table_length;
        memcpy(&table_length, table + ACPI_TABLE_LENGTH, sizeof(uint32_t));
        if (table_length < ACPI_TABLE_HEADER_SIZE || byte_sum(table, table_length) != 0) {
            return NULL;
        }
        return table;
    }
    return NULL;
}

static uint32_t table_length(const uint8_t *table) {
    uint32_t length;
    memcpy(&length, table + ACPI_TABLE_LENGTH, sizeof(uint32_t));
    return length;
}

// The port a Generic Address Structure gives, if it is one.
static uint16_t gas_port(const uint8_t *gas) {
    uint64_t address;
    memcpy(&address, gas + GAS_ADDRESS, sizeof(uint64_t));
    if (gas[GAS_SPACE] != GAS_SPACE_SYSTEM_IO || address == 0 || address > 0xfff8) {
        return 0;
    }
    return (uint16_t)address;
}

static uint16_t spcr_port(void) {
    const uint8_t *spcr = acpi_table("SPCR");
    if (spcr == NULL || table_length(spcr) < SPCR_BASE_ADDRESS + GAS_SIZE) {
        return 0;
    }
    uint8_t type = spcr[SPCR_INTERFACE_TYPE];
    bool uart = type == UART_FULL_16550 || type == UART_16550_SUBSET
             || (spcr[ACPI_TABLE_REVISION] > 1 && type == UART_16550_GAS);
    return uart ? gas_port(spcr + SPCR_BASE_ADDRESS) : 0;
}

static uint16_t dbg2_port(void) {
    const uint8_t *dbg2 = acpi_table("DBG2");
    if (dbg2 == NULL || table_length(dbg2) < DBG2_DEVICE_COUNT + sizeof(uint32_t)) {
        return 0;
    }
    uint32_t length = table_length(dbg2);
    uint32_t offset;
    uint32_t count;
    memcpy(&offset, dbg2 + DBG2_DEVICE_OFFSET, sizeof(uint32_t));
    memcpy(&count, dbg2 + DBG2_DEVICE_COUNT, sizeof(uint32_t));
    for (size_t i = 0; i < count && offset <= length - DBG2_DEVICE_MIN_LENGTH; i++) {
        const uint8_t *device = dbg2 + offset;
        uint16_t size;
        uint16_t type;
        uint16_t subtype;
        uint16_t base;
        memcpy(&size, device + DBG2_DEVICE_LENGTH, sizeof(uint16_t));
        memcpy(&type, device + DBG2_DEVICE_PORT_TYPE, sizeof(uint16_t));
        memcpy(&subtype, device + DBG2_DEVICE_PORT_SUBTYPE, sizeof(uint16_t));
        memcpy(&base, device + DBG2_DEVICE_BASE_OFFSET, sizeof(uint16_t));
        if (size < DBG2_DEVICE_MIN_LENGTH || size > length - offset) {
            return 0;
        }
        bool uart = type == DBG2_PORT_SERIAL
                 && (subtype == UART_FULL_16550 || subtype == UART_16550_SUBSET
                  || subtype == UART_16550_GAS);
        if (uart && base <= size - GAS_SIZE) {
            uint16_t port = gas_port(device + base);
            if (port != 0) {
                return port;
            }
        }
        offset += size;
    }
    return 0;
}

uint16_t fwtables_serial_port(void) {
    uint16_t port = spcr_port();
    return port != 0 ? port : dbg2_port();
}

bool fwtables_smbios_byte(uint8_t type, uint8_t offset, uint8_t *value) {
    const char *strings_end;
    const uint8_t *structure = smbios_structure(type, &strings_end);
    if (structure == NULL || offset >= structure[1]) {
        return false;
    }
    *value = structure[offset];
    return true;
}

static uint64_t smbios_find(void) {
    const struct smbios2_entry *eps2 = config_table_find(&smbios_guid);
    if (eps2 != NULL) {
        const char *problem = smbios2_check(eps2);
        if (problem == NULL) {
            return smbios2_use(eps2);
        }
        printf(
            "Ignoring the SMBIOS 2.x entry point at %#llx: %s\n",
            (unsigned long long)(uintptr_t)eps2, problem
        );
    }

    const struct smbios3_entry *eps3 = config_table_find(&smbios3_guid);
    if (eps3 != NULL) {
        const char *problem = smbios3_check(eps3);
        if (problem == NULL) {
            return smbios3_synthesise(eps3);
        }
        printf(
            "Ignoring the SMBIOS 3.x entry point at %#llx: %s\n",
            (unsigned long long)(uintptr_t)eps3, problem
        );
    }

    return 0;
}

// uACPI takes the XSDT wherever the RSDP gives one, so where only the RSDT is
// sound, it gets an ACPI 1.0 RSDP, which gives the RSDT alone.
static uint64_t rsdp_for_rsdt(const struct rsdp *rsdp) {
    uintptr_t copy = efi_alloc_pages_typed(1, UINT32_MAX, EfiACPIReclaimMemory);
    if (copy == 0) {
        return (uintptr_t)rsdp;
    }
    struct rsdp *v1 = (struct rsdp *)copy;
    memset(v1, 0, sizeof(struct rsdp));
    memcpy(v1, rsdp, RSDP_V1_SIZE);
    v1->revision = 0;
    v1->checksum = 0;
    v1->checksum = (uint8_t)-byte_sum(v1, RSDP_V1_SIZE);
    return copy;
}

void fwtables_collect(void) {
    uint64_t rsdp = rsdp_find(&acpi_20_guid, "2.0");
    if (rsdp == 0) {
        rsdp = rsdp_find(&acpi_10_guid, "1.0");
    }
    if (rsdp == 0) {
        printf("No usable ACPI RSDP\n");
    }
    const struct rsdp *found = (const struct rsdp *)(uintptr_t)rsdp;
    if (found != NULL && found->revision >= 2 && found->xsdt_address != 0
     && !root_sound(found->xsdt_address, "XSDT") && root_sound(found->rsdt_address, "RSDT")) {
        printf(
            "The XSDT at %#llx is broken, so the ACPI tables come from the RSDT at %#x\n",
            (unsigned long long)found->xsdt_address, found->rsdt_address
        );
        rsdp = rsdp_for_rsdt(found);
    }
    loader_handoff->acpi_rsdp = rsdp;

    loader_handoff->smbios_entry = smbios_find();
    if (loader_handoff->smbios_entry == 0) {
        printf("No usable SMBIOS entry point\n");
    }
}
