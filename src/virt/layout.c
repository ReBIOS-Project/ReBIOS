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
#include <efi.h>
#include <memory.h>
#include <core.h>
#include <csm/e820.h>
#include <handoff.h>
#include <iommu/iommu_tables.h>
#include <lib/heap.h>
#include <lib/log.h>
#include <sys/mm.h>
#include <virt/gpt.h>
#include <virt/layout.h>
#include <virt/mmio.h>

#define ONE_MIB 0x100000ull
#define TWO_MIB 0x200000ull
#define ONE_GIB 0x40000000ull
#define FOUR_GIB 0x100000000ull
#define VGA_BASE 0xa0000ull
#define VGA_END 0xc0000ull

#define MAX_RANGES 1024
#define MAX_TAKEN 8

struct range {
    uint64_t base;
    uint64_t end;
};

// Sorted, without overlaps.
struct range_list {
    struct range *r;
    uint32_t count;
};

// A host range the guest finds at its own address, with the firmware's type
// for it.
struct identity_range {
    uint64_t base;
    uint64_t end;
    uint32_t efi_type;
    uint64_t attribute;
};

static struct identity_range *identity;
static uint32_t identity_count;
// The guest's RAM from 1 MiB.
static struct range_list ram;
// What layout_take() may still hand out.
static struct range_list takeable;
static struct {
    uint64_t base;
    uint64_t end;
    uint32_t e820_type;
} taken[MAX_TAKEN];
static uint32_t taken_count;
// Backs the guest's first 2 MiB.
static uint8_t *low;
// What absent memory reads as.
static uint64_t ones_page;

static uint64_t align_up(uint64_t x, uint64_t align) {
    return (x + align - 1) & ~(align - 1);
}

static uint64_t align_down(uint64_t x, uint64_t align) {
    return x & ~(align - 1);
}

static void list_init(struct range_list *list) {
    list->r = kmalloc(MAX_RANGES * sizeof(*list->r));
    list->count = 0;
}

// Adds [base, end), merged with the ranges it overlaps or touches.
static void list_add(struct range_list *list, uint64_t base, uint64_t end) {
    if (base >= end) {
        return;
    }
    uint32_t first = 0;
    while (first < list->count && list->r[first].end < base) {
        first++;
    }
    uint32_t last = first;
    while (last < list->count && list->r[last].base <= end) {
        if (list->r[last].base < base) {
            base = list->r[last].base;
        }
        if (list->r[last].end > end) {
            end = list->r[last].end;
        }
        last++;
    }
    if (last == first) {
        if (list->count == MAX_RANGES) {
            panic("Too many memory ranges\n");
        }
        memmove(&list->r[first + 1], &list->r[first], (list->count - first) * sizeof(*list->r));
        list->count++;
    } else {
        memmove(&list->r[first + 1], &list->r[last], (list->count - last) * sizeof(*list->r));
        list->count -= last - first - 1;
    }
    list->r[first].base = base;
    list->r[first].end = end;
}

static void list_remove(struct range_list *list, uint64_t base, uint64_t end) {
    uint32_t i = 0;
    while (base < end && i < list->count) {
        struct range *r = &list->r[i];
        if (r->end <= base || r->base >= end) {
            i++;
        } else if (r->base < base && r->end > end) {
            if (list->count == MAX_RANGES) {
                panic("Too many memory ranges\n");
            }
            memmove(&list->r[i + 1], r, (list->count - i) * sizeof(*list->r));
            list->count++;
            list->r[i].end = base;
            list->r[i + 1].base = end;
            return;
        } else if (r->base < base) {
            r->end = base;
            i++;
        } else if (r->end > end) {
            r->base = end;
            i++;
        } else {
            memmove(r, r + 1, (list->count - i - 1) * sizeof(*list->r));
            list->count--;
        }
    }
}

static uint64_t list_total(const struct range_list *list) {
    uint64_t total = 0;
    for (uint32_t i = 0; i < list->count; i++) {
        total += list->r[i].end - list->r[i].base;
    }
    return total;
}

// Free for the guest's RAM once the boot services are gone.
static bool type_is_free(uint32_t type) {
    switch (type) {
        case EfiLoaderCode:
        case EfiLoaderData:
        case EfiBootServicesCode:
        case EfiBootServicesData:
        case EfiConventionalMemory: {
            return true;
        }
        default: {
            return false;
        }
    }
}

// Memory of some kind, as opposed to I/O and address space the firmware
// keeps.
static bool type_is_memory(uint32_t type) {
    switch (type) {
        case EfiRuntimeServicesCode:
        case EfiRuntimeServicesData:
        case EfiUnusableMemory:
        case EfiACPIReclaimMemory:
        case EfiACPIMemoryNVS:
        case EfiPersistentMemory: {
            return true;
        }
        default: {
            return type_is_free(type);
        }
    }
}

// What the guest must find where the host has it: memory-mapped I/O, and what
// the firmware keeps for itself or for devices. The runtime services and the
// firmware's ACPI are the hypervisor's alone.
static bool type_is_identity(uint32_t type) {
    switch (type) {
        case EfiRuntimeServicesCode:
        case EfiRuntimeServicesData:
        case EfiUnusableMemory:
        case EfiACPIReclaimMemory:
        case EfiACPIMemoryNVS:
        case EfiUnacceptedMemoryType: {
            return false;
        }
        default: {
            return !type_is_free(type);
        }
    }
}

static uint8_t identity_memtype(const struct identity_range *r) {
    switch (r->efi_type) {
        case EfiMemoryMappedIO:
        case EfiMemoryMappedIOPortSpace: {
            return MT_UC;
        }
        case EfiPersistentMemory: {
            return MT_WB;
        }
        default: {
            return (r->attribute & EFI_MEMORY_WB) ? MT_WB : MT_UC;
        }
    }
}

static const EFI_MEMORY_DESCRIPTOR *efi_desc(uint32_t index) {
    const struct handoff *h = core_handoff;
    if ((index + 1) * h->efi_mmap_desc_size > h->efi_mmap_size) {
        return NULL;
    }
    return (const void *)(uintptr_t)(h->efi_mmap + index * h->efi_mmap_desc_size);
}

static uint64_t efi_attribute_at(uint64_t addr) {
    const EFI_MEMORY_DESCRIPTOR *d;
    for (uint32_t i = 0; (d = efi_desc(i)) != NULL; i++) {
        if (addr >= d->PhysicalStart && addr - d->PhysicalStart < d->NumberOfPages * PAGE_SIZE) {
            return d->Attribute;
        }
    }
    return 0;
}

// The legacy memory map comes first: nothing keeps its host address below
// 1 MiB.
static void identity_add(uint64_t base, uint64_t end, uint32_t efi_type, uint64_t attribute) {
    if (base < ONE_MIB) {
        base = ONE_MIB;
    }
    if (base >= end) {
        return;
    }
    if (identity_count == MAX_RANGES) {
        panic("Too many host ranges for the guest\n");
    }
    identity[identity_count++] = (struct identity_range){base, end, efi_type, attribute};
}

// Takes a size-aligned block of size bytes off the list.
static bool take_block(struct range_list *list, uint64_t size, uint64_t *block) {
    for (uint32_t i = 0; i < list->count; i++) {
        uint64_t base = align_up(list->r[i].base, size);
        if (base < list->r[i].end && list->r[i].end - base >= size) {
            list_remove(list, base, base + size);
            *block = base;
            return true;
        }
    }
    return false;
}

static void map_ram(uint64_t gpa, uint64_t hpa, uint64_t size) {
    gpt_map(gpa, hpa, size, GPT_RWX, MT_WB);
}

// Backs the size-aligned blocks of guest with size-aligned blocks of host, in
// order, until either list runs out, so that they can be mapped with pages
// that large, and takes them off both lists.
static void assign_blocks(struct range_list *guest, struct range_list *host, uint64_t size) {
    struct range_list guest_done;
    struct range_list host_done;
    list_init(&guest_done);
    list_init(&host_done);

    uint32_t h = 0;
    uint64_t hpa = host->count != 0 ? align_up(host->r[0].base, size) : 0;
    for (uint32_t g = 0; g < guest->count && h < host->count; g++) {
        uint64_t gpa = align_up(guest->r[g].base, size);
        uint64_t gend = align_down(guest->r[g].end, size);
        while (gpa < gend) {
            while (h < host->count && (hpa < host->r[h].base || hpa >= host->r[h].end
                                       || host->r[h].end - hpa < size)) {
                h++;
                if (h < host->count) {
                    hpa = align_up(host->r[h].base, size);
                }
            }
            if (h == host->count) {
                break;
            }
            uint64_t len = align_down(host->r[h].end - hpa, size);
            if (len > gend - gpa) {
                len = gend - gpa;
            }
            map_ram(gpa, hpa, len);
            list_add(&guest_done, gpa, gpa + len);
            list_add(&host_done, hpa, hpa + len);
            gpa += len;
            hpa += len;
        }
    }

    for (uint32_t i = 0; i < guest_done.count; i++) {
        list_remove(guest, guest_done.r[i].base, guest_done.r[i].end);
    }
    for (uint32_t i = 0; i < host_done.count; i++) {
        list_remove(host, host_done.r[i].base, host_done.r[i].end);
    }
    kfree(guest_done.r);
    kfree(host_done.r);
}

// Backs the rest of guest with what is left of host, in order. host must hold
// enough.
static void assign_rest(const struct range_list *guest, const struct range_list *host) {
    uint32_t h = 0;
    uint64_t hpa = host->r[0].base;
    for (uint32_t g = 0; g < guest->count; g++) {
        uint64_t gpa = guest->r[g].base;
        while (gpa < guest->r[g].end) {
            if (hpa == host->r[h].end) {
                h++;
                hpa = host->r[h].base;
            }
            uint64_t len = host->r[h].end - hpa;
            if (len > guest->r[g].end - gpa) {
                len = guest->r[g].end - gpa;
            }
            map_ram(gpa, hpa, len);
            gpa += len;
            hpa += len;
        }
    }
}

void layout_init(void) {
    const struct handoff *h = core_handoff;
    struct range_list free_ram;
    struct range_list pinned;
    list_init(&free_ram);
    list_init(&pinned);
    list_init(&ram);
    list_init(&takeable);
    identity = kmalloc(MAX_RANGES * sizeof(*identity));

    uint64_t low_top = ONE_MIB;
    uint64_t high_top = FOUR_GIB;
    const EFI_MEMORY_DESCRIPTOR *d;
    for (uint32_t i = 0; (d = efi_desc(i)) != NULL; i++) {
        uint64_t base = d->PhysicalStart;
        uint64_t end = base + d->NumberOfPages * PAGE_SIZE;
        if (type_is_free(d->Type)) {
            list_add(&free_ram, base, end);
        }
        if (type_is_identity(d->Type)) {
            identity_add(base, end, d->Type, d->Attribute);
        }
        if (type_is_memory(d->Type)) {
            if (base < FOUR_GIB && end > low_top) {
                low_top = end < FOUR_GIB ? end : FOUR_GIB;
            }
            if (end > high_top) {
                high_top = end;
            }
        }
    }

    // One domain serves every device, so what one device needs one to one
    // stays so for the guest too.
    const struct iommu_tables *tables = iommu_tables_parse();
    for (uint32_t i = 0; i < tables->reserved_count; i++) {
        const struct iommu_reserved_region *r = &tables->reserved[i];
        uint64_t base = align_down(r->base, PAGE_SIZE);
        uint64_t end = align_down(r->last, PAGE_SIZE) + PAGE_SIZE;
        if (base < ONE_MIB) {
            printf("IOMMU region %#llx-%#llx is in the guest's legacy memory\n",
                   (unsigned long long)r->base, (unsigned long long)r->last);
        }
        identity_add(base, end, EfiReservedMemoryType, efi_attribute_at(base));
    }

    for (uint32_t i = 0; i < identity_count; i++) {
        list_add(&pinned, identity[i].base, identity[i].end);
    }
    for (uint32_t i = 0; i < h->reserved_count; i++) {
        list_remove(&free_ram, h->reserved[i].base, h->reserved[i].base + h->reserved[i].size);
    }
    for (uint32_t i = 0; i < pinned.count; i++) {
        list_remove(&free_ram, pinned.r[i].base, pinned.r[i].end);
    }
    // The host's own legacy region is left alone, for its shadowing to be
    // probed after the guest programs the chipset.
    list_remove(&free_ram, VGA_BASE, ONE_MIB);

    // As much as the host has, from 1 MiB to the end of its memory below
    // 4 GiB, then from 4 GiB, around what must keep its address.
    list_add(&ram, ONE_MIB, low_top);
    list_add(&ram, FOUR_GIB, high_top);
    for (uint32_t i = 0; i < pinned.count; i++) {
        list_remove(&ram, pinned.r[i].base, pinned.r[i].end);
    }

    uint64_t low_hpa;
    if (!take_block(&free_ram, TWO_MIB, &low_hpa)) {
        panic("No 2 MiB block of host memory for the guest's low memory\n");
    }
    low = (uint8_t *)(uintptr_t)low_hpa;
    memset(low, 0, TWO_MIB);

    struct range_list fill;
    list_init(&fill);
    for (uint32_t i = 0; i < ram.count; i++) {
        list_add(&fill, ram.r[i].base, ram.r[i].end);
    }
    list_remove(&fill, 0, TWO_MIB);
    uint64_t available = list_total(&free_ram);
    uint64_t wanted = list_total(&fill);
    if (wanted > available) {
        uint64_t sum = 0;
        for (uint32_t i = 0; i < fill.count; i++) {
            uint64_t size = fill.r[i].end - fill.r[i].base;
            if (sum + size > available) {
                uint64_t cut = fill.r[i].base + (available - sum);
                list_remove(&fill, cut, UINT64_MAX);
                list_remove(&ram, cut, UINT64_MAX);
                break;
            }
            sum += size;
        }
        wanted = available;
    }

    // One to one, for what the host's memory map leaves out, such as the PCI
    // hole, but none of the host's memory except what the guest must share.
    gpt_map(0, 0, h->map_top, GPT_RWX, MT_UC);
    for (uint32_t i = 0; (d = efi_desc(i)) != NULL; i++) {
        if (type_is_memory(d->Type)) {
            gpt_map(d->PhysicalStart, 0, d->NumberOfPages * PAGE_SIZE, 0, 0);
        }
    }
    for (uint32_t i = 0; i < h->reserved_count; i++) {
        gpt_map(h->reserved[i].base, 0, h->reserved[i].size, 0, 0);
    }
    for (uint32_t i = 0; i < identity_count; i++) {
        const struct identity_range *r = &identity[i];
        gpt_map(r->base, r->base, r->end - r->base, GPT_RWX, identity_memtype(r));
    }

    // Low memory and the ROM area, around the VGA window, which goes to the
    // VGA device, then the rest of the first 2 MiB.
    map_ram(0, low_hpa, VGA_BASE);
    gpt_map(VGA_BASE, VGA_BASE, VGA_END - VGA_BASE, GPT_RWX, MT_UC);
    map_ram(VGA_END, low_hpa + VGA_END, ONE_MIB - VGA_END);
    for (uint32_t i = 0; i < ram.count && ram.r[i].base < TWO_MIB; i++) {
        uint64_t end = ram.r[i].end < TWO_MIB ? ram.r[i].end : TWO_MIB;
        map_ram(ram.r[i].base, low_hpa + ram.r[i].base, end - ram.r[i].base);
    }

    assign_blocks(&fill, &free_ram, ONE_GIB);
    assign_blocks(&fill, &free_ram, TWO_MIB);
    assign_rest(&fill, &free_ram);

    uint64_t below = 0;
    uint64_t above = 0;
    for (uint32_t i = 0; i < ram.count; i++) {
        if (ram.r[i].base < FOUR_GIB) {
            list_add(&takeable, ram.r[i].base, ram.r[i].end);
            below += ram.r[i].end - ram.r[i].base;
        } else {
            above += ram.r[i].end - ram.r[i].base;
        }
    }
    printf("Guest RAM: %llu MiB from 1 MiB, to %#llx, and %llu MiB from 4 GiB\n",
           (unsigned long long)(below >> 20),
           (unsigned long long)(takeable.count != 0 ? takeable.r[takeable.count - 1].end : 0),
           (unsigned long long)(above >> 20));
    if (available > wanted) {
        printf("%llu MiB of host memory left over\n",
               (unsigned long long)((available - wanted) >> 20));
    }

    kfree(fill.r);
    kfree(pinned.r);
    kfree(free_ram.r);
}

void *layout_low(uint32_t gpa) {
    if (gpa >= ONE_MIB || (gpa >= VGA_BASE && gpa < VGA_END)) {
        panic("No guest low memory at %#x\n", gpa);
    }
    return low + gpa;
}

uint32_t layout_take(uint32_t size, uint32_t align, uint32_t e820_type) {
    // Whole pages, as the memory map and the page tables have them.
    size = (uint32_t)align_up(size, PAGE_SIZE);
    if (align < PAGE_SIZE) {
        align = PAGE_SIZE;
    }
    for (uint32_t i = takeable.count; i-- > 0;) {
        uint64_t end = takeable.r[i].end;
        if (end - takeable.r[i].base < size) {
            continue;
        }
        uint64_t base = align_down(end - size, align);
        if (base < takeable.r[i].base) {
            continue;
        }
        if (e820_type != E820_RAM) {
            if (taken_count == MAX_TAKEN) {
                return 0;
            }
            taken[taken_count].base = base;
            taken[taken_count].end = base + size;
            taken[taken_count].e820_type = e820_type;
            taken_count++;
        }
        list_remove(&takeable, base, end);
        return (uint32_t)base;
    }
    return 0;
}

static uint64_t absent_read(struct vcpu *v, void *ctx, uint64_t offset, unsigned size) {
    (void)v;
    (void)ctx;
    (void)offset;
    return size == 8 ? UINT64_MAX : (1ull << (size * 8)) - 1;
}

static void absent_write(struct vcpu *v, void *ctx, uint64_t offset, unsigned size,
                         uint64_t value) {
    (void)v;
    (void)ctx;
    (void)offset;
    (void)size;
    (void)value;
}

static const struct mmio_ops absent_ops = {absent_read, absent_write, true};

void layout_absent(uint64_t gpa, uint64_t size) {
    if (size == 0) {
        return;
    }
    if (ones_page == 0) {
        uint8_t *page = page_alloc(1);
        memset(page, 0xff, PAGE_SIZE);
        ones_page = (uint64_t)(uintptr_t)page;
    }
    // Reads hit the page of ones, writes exit.
    mmio_register(gpa, size, &absent_ops, NULL);
    for (uint64_t offset = 0; offset < size; offset += PAGE_SIZE) {
        gpt_map(gpa + offset, ones_page, PAGE_SIZE, GPT_R | GPT_X, MT_WB);
    }
    gpt_flush();
}

static uint64_t rom_read(struct vcpu *v, void *ctx, uint64_t offset, unsigned size) {
    (void)v;
    uint64_t value = 0;
    memcpy(&value, low + (uintptr_t)ctx + offset, size);
    return value;
}

static void rom_write(struct vcpu *v, void *ctx, uint64_t offset, unsigned size,
                      uint64_t value) {
    (void)v;
    (void)ctx;
    (void)offset;
    (void)size;
    (void)value;
}

static const struct mmio_ops rom_ops = {rom_read, rom_write, true};

void layout_read_only(uint64_t gpa, uint64_t size) {
    if (size == 0 || gpa >= ONE_MIB || size > ONE_MIB - gpa) {
        return;
    }
    // Reads go to the memory, writes exit to be dropped.
    mmio_register(gpa, size, &rom_ops, (void *)(uintptr_t)gpa);
    gpt_map(gpa, (uint64_t)(uintptr_t)(low + gpa), size, GPT_R | GPT_X, MT_WB);
    gpt_flush();
}

bool layout_e820(struct e820_map *map) {
    bool ok = true;
    e820_init(map);
    for (uint32_t i = 0; i < identity_count; i++) {
        const struct identity_range *r = &identity[i];
        EFI_MEMORY_DESCRIPTOR desc = {
            .Type = r->efi_type,
            .PhysicalStart = r->base,
            .NumberOfPages = (r->end - r->base) / PAGE_SIZE,
            .Attribute = r->attribute,
        };
        ok &= e820_set_efi_map(map, &desc, sizeof(desc), sizeof(desc));
    }
    for (uint32_t i = 0; i < ram.count; i++) {
        ok &= e820_set(map, ram.r[i].base, ram.r[i].end - ram.r[i].base, E820_RAM);
    }
    for (uint32_t i = 0; i < taken_count; i++) {
        ok &= e820_set(map, taken[i].base, taken[i].end - taken[i].base, taken[i].e820_type);
    }
    ok &= e820_set_low_memory(map);
    return ok;
}
