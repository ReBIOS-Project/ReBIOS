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

_Static_assert(
    sizeof(struct smbios2_entry) == 0x1f, "SMBIOS 2.1 entry point layout"
);

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

_Static_assert(
    sizeof(struct smbios3_entry) == 0x18, "SMBIOS 3.0 entry point layout"
);

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
        if (rsdp->length < sizeof(struct rsdp)
         || rsdp->length > RSDP_MAX_SIZE) {
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
        "ACPI RSDP revision %u at %#llx\n", rsdp->revision,
        (unsigned long long)(uintptr_t)rsdp
    );
    return (uintptr_t)rsdp;
}

// Returns why the entry point is invalid, or NULL.
static const char *smbios2_check(const struct smbios2_entry *eps) {
    size_t intermediate = offsetof(struct smbios2_entry, intermediate_anchor);
    if (memcmp(eps->anchor, "_SM_", sizeof(eps->anchor)) != 0) {
        return "bad anchor";
    }
    if (eps->length < sizeof(struct smbios2_entry)) {
        return "bad length";
    }
    if (byte_sum(eps, eps->length) != 0) {
        return "bad checksum";
    }
    int anchor = memcmp(
        eps->intermediate_anchor, "_DMI_", sizeof(eps->intermediate_anchor)
    );
    if (anchor != 0) {
        return "bad intermediate anchor";
    }
    uint8_t sum = byte_sum(
        (const uint8_t *)eps + intermediate, eps->length - intermediate
    );
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
        (uint8_t *)eps + intermediate, eps->length - intermediate
    );
    eps->checksum = 0;
    eps->checksum = (uint8_t)-byte_sum(eps, eps->length);
}

static void smbios_log(const char *how, const struct smbios2_entry *eps) {
    printf(
        "SMBIOS %u.%u, 32-bit entry point %s %#llx, structure table at %#llx "
        "(%u bytes, %u structure%s)\n",
        eps->major, eps->minor, how, (unsigned long long)(uintptr_t)eps,
        (unsigned long long)eps->table_address, eps->table_length,
        eps->struct_count, eps->struct_count == 1 ? "" : "s"
    );
}

static uint64_t smbios2_use(const struct smbios2_entry *eps) {
    uintptr_t eps_addr = (uintptr_t)eps;
    uintptr_t table = eps->table_address;
    if (eps_addr + eps->length <= FOUR_GIB
     && table + eps->table_length <= FOUR_GIB) {
        smbios_log("at", eps);
        return eps_addr;
    }

    uintptr_t copy = efi_alloc_pages_typed(
        page_count(eps->length + eps->table_length), UINT32_MAX,
        EfiRuntimeServicesData
    );
    if (copy == 0) {
        printf("Cannot allocate memory below 4GiB for the SMBIOS tables\n");
        return 0;
    }
    struct smbios2_entry *new_eps = (struct smbios2_entry *)(uintptr_t)copy;
    memcpy(new_eps, eps, eps->length);
    memcpy(
        (uint8_t *)new_eps + eps->length, (const void *)(uintptr_t)table,
        eps->table_length
    );
    new_eps->table_address = copy + eps->length;
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
static void smbios_walk(
    const uint8_t *table, size_t max_size, struct smbios_walk *walk
) {
    memset(walk, 0, sizeof(struct smbios_walk));

    size_t off = 0;
    while (max_size - off >= 4) {
        uint8_t type = table[off];
        size_t length = table[off + 1];
        if (length < 4 || length > max_size - off) {
            break;
        }

        uintptr_t end = off + length;
        while (end + 1 < max_size
            && (table[end] != 0 || table[end + 1] != 0)) {
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
            "The SMBIOS structure table at %#llx holds no structures\n",
            (unsigned long long)table
        );
        return 0;
    }
    if (walk.truncated) {
        printf(
            "Cutting the SMBIOS structure table to %zu bytes and %zu "
            "structure%s to fit a 32-bit entry point\n",
            walk.length, walk.count, walk.count == 1 ? "" : "s"
        );
    }

    uintptr_t addr = efi_alloc_pages_typed(
        page_count(sizeof(struct smbios2_entry) + walk.length), UINT32_MAX,
        EfiRuntimeServicesData
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
    memcpy(
        eps->intermediate_anchor, "_DMI_", sizeof(eps->intermediate_anchor)
    );
    eps->table_length = walk.length;
    eps->table_address = addr + sizeof(struct smbios2_entry);
    eps->struct_count = walk.count;
    eps->bcd_revision = (eps3->major % 10) << 4 | eps3->minor % 10;
    memcpy(
        (uint8_t *)eps + sizeof(struct smbios2_entry),
        (const void *)(uintptr_t)table, walk.length
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
static const uint8_t *smbios_structure(
    uint8_t type, const char **strings_end
) {
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
        while (end + 1 < max_size
            && (table[end] != 0 || table[end + 1] != 0)) {
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

void fwtables_collect(void) {
    uint64_t rsdp = rsdp_find(&acpi_20_guid, "2.0");
    if (rsdp == 0) {
        rsdp = rsdp_find(&acpi_10_guid, "1.0");
    }
    if (rsdp == 0) {
        printf("No usable ACPI RSDP\n");
    }
    loader_handoff->acpi_rsdp = rsdp;

    loader_handoff->smbios_entry = smbios_find();
    if (loader_handoff->smbios_entry == 0) {
        printf("No usable SMBIOS entry point\n");
    }
}
