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
#include <virt/layout.h>
#include <core.h>
#include <csm/e820.h>
#include <handoff.h>
#include <hw/pci_enum.h>
#include <hw/pci_host.h>
#include <iommu/iommu_tables.h>
#include <lib/heap.h>
#include <lib/log.h>
#include <lib/range.h>
#include <lib/util.h>
#include <memory.h>
#include <sys/mm.h>
#include <sys/x86.h>
#include <virt/gpt.h>
#include <virt/igfx.h>
#include <virt/mmio.h>
#include <virt/pci_plan.h>
#include <virt/vcpu.h>
#include <virt/vga.h>

// AMD's IOMMUs give guest-physical DMA addresses from FD_0000_0000h to 1 TiB
// special meanings where they do not ignore the HyperTransport range (AMD
// IOMMU 48882 rev. 3.11, 2.1.2, Table 3; erratum 1155, 57095 rev. 1.05): the
// guest has nothing there, as Xen 4.22 keeps dom0 from it and Linux 7.3's AMD
// IOMMU driver reserves it.
#define HT_BASE ((uint64_t)0xfd00000000)
#define HT_END ((uint64_t)0x10000000000)
#define VGA_BASE ((uint64_t)0xa0000)
#define VGA_END ((uint64_t)0xc0000)

#define MAX_RANGES 1024
#define MAX_TAKEN 8
// What layout_contiguous() may take of the hypervisor's memory.
#define MAX_MOVED_PAGES 64

// A host range the guest finds at its own address, with the firmware's type
// for it.
struct identity_range {
    uintptr_t base;
    uintptr_t end;
    uint32_t efi_type;
    uint64_t attribute;
};

static struct identity_range *identity;
static size_t identity_count;
// The host's memory that the firmware leaves free, in whole pages, but where a
// descriptor of another type overlaps it: such a range keeps the other type,
// as Linux 7.3's e820__update_table() resolves overlaps.
static struct range_list host_free;
// Where the identity ranges are, merged.
static struct range_list pinned;
// The guest's RAM from 1 MiB.
static struct range_list ram;
// What layout_take() may still hand out.
static struct range_list takeable;
// The guest's address space where it has no RAM for want of host memory.
static struct range_list kept;
// Where the guest has neither memory nor devices.
static struct range_list holes;
static struct {
    uintptr_t base;
    uintptr_t end;
    uint32_t e820_type;
} taken[MAX_TAKEN];
static size_t taken_count;
// The emulated VGA's linear framebuffer.
static uintptr_t lfb;
// The guest's address for the host's stolen memory, and the host's.
static struct range stolen;
static uintptr_t stolen_hpa;
// Backs the guest's first 2 MiB.
static uint8_t *low;
// What absent memory reads as.
static uintptr_t ones_page;
static size_t moved_pages;

static uint64_t list_total(const struct range_list *list) {
    size_t total = 0;
    for (size_t i = 0; i < list->count; i++) {
        total += list->r[i].end - list->r[i].base;
    }
    return total;
}

// Free once the boot services are gone.
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

// Free for the guest's RAM, but for what Linux 7.3's do_add_efi_memmap() keeps
// from its own: specific-purpose memory (UEFI 2.11, 7.2.3), earmarked for
// particular drivers or applications, and memory that cannot be cached
// write-back.
static bool desc_is_free(const EFI_MEMORY_DESCRIPTOR *d) {
    return type_is_free(d->Type) && (d->Attribute & EFI_MEMORY_SP) == 0
        && (d->Attribute & EFI_MEMORY_WB) != 0;
}

static uint8_t identity_memtype(const struct identity_range *r) {
    if (r->efi_type == EfiMemoryMappedIO) {
        return MT_UC;
    }
    return (r->attribute & EFI_MEMORY_WB) != 0 ? MT_WB : MT_UC;
}

static uint64_t efi_attribute_at(uintptr_t addr) {
    const EFI_MEMORY_DESCRIPTOR *d;
    for (size_t i = 0; (d = handoff_efi_desc(core_handoff, i)) != NULL; i++) {
        if (addr >= d->PhysicalStart && addr - d->PhysicalStart < d->NumberOfPages * PAGE_SIZE) {
            return d->Attribute;
        }
    }
    return 0;
}

// The legacy memory map comes first: nothing keeps its host address below 1
// MiB.
static void identity_add(uintptr_t base, uintptr_t end, uint32_t efi_type, uint64_t attribute) {
    if (base < ONE_MIB) {
        base = ONE_MIB;
    }
    if (base >= end) {
        return;
    }
    if (identity_count == MAX_RANGES) {
        panic("Too many host ranges for the guest\n");
    }
    identity[identity_count++] = (struct identity_range){ base, end, efi_type, attribute };
}

// Takes [base, end) out of the identity ranges, which may split one.
static void identity_remove(uintptr_t base, uintptr_t end) {
    for (size_t i = 0; i < identity_count; i++) {
        struct identity_range *r = &identity[i];
        if (r->end <= base || r->base >= end) {
            continue;
        }
        if (r->base < base && r->end > end) {
            identity_add(end, r->end, r->efi_type, r->attribute);
            identity[i].end = base;
        } else if (r->base < base) {
            r->end = base;
        } else if (r->end > end) {
            r->base = end;
        } else {
            identity[i--] = identity[--identity_count];
        }
    }
}

// The highest align-aligned size bytes in [lo, hi), or 0.
static uintptr_t fit_top(uint64_t lo, uint64_t hi, size_t size, uint64_t align) {
    if (hi <= lo || hi - lo < size) {
        return 0;
    }
    uintptr_t base = align_down(hi - size, align);
    return base >= lo ? base : 0;
}

// Takes the highest align-aligned size bytes clear of [avoid_base, avoid_end)
// and of the first 2 MiB off the list, or returns 0.
static uintptr_t take_top(
    struct range_list *list, size_t size, uint64_t align, uintptr_t avoid_base, uintptr_t avoid_end
) {
    for (size_t i = list->count; i-- > 0; ) {
        uint64_t lo = list->r[i].base > TWO_MIB ? list->r[i].base : TWO_MIB;
        uint64_t hi = list->r[i].end;
        uintptr_t base = fit_top(lo > avoid_end ? lo : avoid_end, hi, size, align);
        if (base == 0) {
            base = fit_top(lo, hi < avoid_base ? hi : avoid_base, size, align);
        }
        if (base != 0) {
            range_list_remove(list, base, base + size);
            return base;
        }
    }
    return 0;
}

// Takes a size-aligned block of size bytes off the list.
static bool take_block(struct range_list *list, size_t size, uint64_t *block) {
    for (size_t i = 0; i < list->count; i++) {
        uintptr_t base = align_up(list->r[i].base, size);
        if (base < list->r[i].end && list->r[i].end - base >= size) {
            range_list_remove(list, base, base + size);
            *block = base;
            return true;
        }
    }
    return false;
}

static void map_ram(uintptr_t gpa, uintptr_t hpa, size_t size) {
    gpt_map(gpa, hpa, size, GPT_RWX, MT_WB);
}

// Backs the size-aligned blocks of guest with size-aligned blocks of host, in
// order, until either list runs out, so that they can be mapped with pages
// that large, and takes them off both lists.
static void assign_blocks(struct range_list *guest, struct range_list *host, size_t size) {
    struct range_list guest_done = { 0 };
    struct range_list host_done = { 0 };

    uint32_t h = 0;
    uintptr_t hpa = host->count != 0 ? align_up(host->r[0].base, size) : 0;
    for (size_t g = 0; g < guest->count && h < host->count; g++) {
        uintptr_t gpa = align_up(guest->r[g].base, size);
        uint64_t gend = align_down(guest->r[g].end, size);
        while (gpa < gend) {
            while (h < host->count
                && (hpa < host->r[h].base || hpa >= host->r[h].end
                 || host->r[h].end - hpa < size)) {
                h++;
                if (h < host->count) {
                    hpa = align_up(host->r[h].base, size);
                }
            }
            if (h == host->count) {
                break;
            }
            size_t len = align_down(host->r[h].end - hpa, size);
            if (len > gend - gpa) {
                len = gend - gpa;
            }
            map_ram(gpa, hpa, len);
            range_list_add(&guest_done, gpa, gpa + len);
            range_list_add(&host_done, hpa, hpa + len);
            gpa += len;
            hpa += len;
        }
    }

    for (size_t i = 0; i < guest_done.count; i++) {
        range_list_remove(guest, guest_done.r[i].base, guest_done.r[i].end);
    }
    for (size_t i = 0; i < host_done.count; i++) {
        range_list_remove(host, host_done.r[i].base, host_done.r[i].end);
    }
    range_list_free(&guest_done);
    range_list_free(&host_done);
}

// Backs the rest of guest with what is left of host, in order. host must hold
// enough.
static void assign_rest(const struct range_list *guest, const struct range_list *host) {
    uint32_t h = 0;
    uintptr_t hpa = host->r[0].base;
    for (size_t g = 0; g < guest->count; g++) {
        uintptr_t gpa = guest->r[g].base;
        while (gpa < guest->r[g].end) {
            if (hpa == host->r[h].end) {
                h++;
                hpa = host->r[h].base;
            }
            size_t len = host->r[h].end - hpa;
            if (len > guest->r[g].end - gpa) {
                len = guest->r[g].end - gpa;
            }
            map_ram(gpa, hpa, len);
            gpa += len;
            hpa += len;
        }
    }
}

// Above 4 GiB, from 4 GiB on, as much as there is around what keeps its place.
// Lays size bytes of RAM out from 4 GiB around what is to be avoided, into ram
// and fill where given; returns where it ends.
static uint64_t add_high(struct range_list *fill, size_t size, const struct range_list *avoid) {
    uintptr_t at = FOUR_GIB;
    for (size_t i = 0; i <= avoid->count && size != 0; i++) {
        uint64_t stop = i < avoid->count ? avoid->r[i].base : UINT64_MAX;
        if (stop <= at) {
            if (i < avoid->count && avoid->r[i].end > at) {
                at = avoid->r[i].end;
            }
            continue;
        }
        size_t len = stop - at < size ? stop - at : size;
        if (fill != NULL) {
            range_list_add(&ram, at, at + len);
            range_list_add(fill, at, at + len);
        }
        size -= len;
        at += len;
        if (size != 0 && i < avoid->count) {
            at = avoid->r[i].end;
        }
    }
    return at;
}

void layout_init(void) {
    const struct handoff *h = core_handoff;
    identity = kmalloc(MAX_RANGES * sizeof(struct identity_range));

    const EFI_MEMORY_DESCRIPTOR *d;
    for (size_t i = 0; (d = handoff_efi_desc(core_handoff, i)) != NULL; i++) {
        uintptr_t end;
        if (!handoff_efi_desc_end(d, &end)) {
            printf(
                "Firmware memory map: the descriptor at %#llx runs past the end of memory, so "
                "it is ignored\n", (unsigned long long)d->PhysicalStart
            );
        } else if (desc_is_free(d)) {
            uintptr_t base = page_up(d->PhysicalStart);
            range_list_add(&host_free, base, page_down(end));
        } else if (type_is_free(d->Type)) {
            const char *what = (d->Attribute & EFI_MEMORY_SP) != 0
                             ? "specific-purpose memory"
                             : "memory that cannot be cached write-back";
            printf(
                "Firmware memory map: the guest does not get the %s at %#llx-%#llx\n", what,
                (unsigned long long)d->PhysicalStart, (unsigned long long)end
            );
        }
    }
    for (size_t i = 0; (d = handoff_efi_desc(core_handoff, i)) != NULL; i++) {
        uintptr_t end;
        if (desc_is_free(d) || !handoff_efi_desc_end(d, &end)) {
            continue;
        }
        uintptr_t base = page_down(d->PhysicalStart);
        end = page_up(end);
        if (range_list_overlaps(&host_free, base, end)) {
            printf(
                "Firmware memory map: type %u memory at %#llx-%#llx overlaps free memory, which "
                "the guest does not get\n", d->Type, (unsigned long long)base,
                (unsigned long long)end
            );
            range_list_remove(&host_free, base, end);
        }
    }
    struct range_list free_ram = { 0 };
    for (size_t i = 0; i < host_free.count; i++) {
        range_list_add(&free_ram, host_free.r[i].base, host_free.r[i].end);
    }

    // One domain serves every device, so what one device needs one to one
    // stays so for the guest too.
    const struct iommu_tables *tables = iommu_tables_parse();
    for (size_t i = 0; i < tables->reserved_count; i++) {
        const struct iommu_tables_reserved_region *r = &tables->reserved[i];
        uintptr_t base = page_down(r->base);
        uintptr_t end = page_down(r->last) + PAGE_SIZE;
        if (base < ONE_MIB) {
            printf(
                "IOMMU region %#llx-%#llx is in the guest's legacy memory\n",
                (unsigned long long)r->base, (unsigned long long)r->last
            );
        }
        identity_add(base, end, EfiReservedMemoryType, efi_attribute_at(base));
    }
    // What the OpRegion that the guest shares with the host gives by address.
    uintptr_t vbt_base;
    uintptr_t vbt_end;
    if (igfx_one_to_one(&vbt_base, &vbt_end)) {
        identity_add(vbt_base, vbt_end, EfiReservedMemoryType, efi_attribute_at(vbt_base));
    }

    // The interrupt controllers, the timers, and what the guest's ACPI
    // describes, but never the configuration space windows or the IOMMUs.
    const struct pci_host *host = pci_host_scan();
    for (size_t i = 0; i < host->claimed_count; i++) {
        identity_add(host->claimed[i].base, host->claimed[i].end, EfiMemoryMappedIO, 0);
    }
    for (size_t i = 0; i < host->private_count; i++) {
        identity_remove(host->private[i].base, host->private[i].end);
    }

    // A legacy video BIOS gives the OS its linear framebuffer's address as the
    // card has it, not as the guest's BARs do (NVIDIA's VBE 3.0 BIOS on a
    // Quadro RTX 4000 reports its BAR3 at the host's address): the card's
    // memory below 4 GiB stays at its host addresses too, beside its BARs.
    if (h->video.vbios_present) {
        const struct handoff_pci_address *va = &h->video.vga_device;
        struct pci_enum_function *f = pci_enum_find((struct pci_cfg_addr){
            va->segment, va->bus, (uint8_t)(va->devfn >> 3),
            (uint8_t)(va->devfn & 7)
        });
        for (size_t b = 0; f != NULL && b < f->bar_count && b < 6; b++) {
            const struct pci_enum_bar *bar = &f->bars[b];
            if ((bar->kind != PCI_ENUM_BAR_MEM32 && bar->kind != PCI_ENUM_BAR_MEM64)
             || bar->size == 0 || bar->base == 0
             || bar->base + bar->size > FOUR_GIB) {
                continue;
            }
            printf(
                "VBIOS card BAR%zu kept at the host's 0x%llx-0x%llx\n", b,
                (unsigned long long)bar->base,
                (unsigned long long)(bar->base + bar->size)
            );
            identity_add(
                align_down(bar->base, PAGE_SIZE),
                align_up(bar->base + bar->size, PAGE_SIZE), EfiMemoryMappedIO,
                0
            );
        }
    }

    for (size_t i = 0; i < identity_count; i++) {
        range_list_add(&pinned, identity[i].base, identity[i].end);
    }
    if (tables->kind == IOMMU_TABLES_KIND_AMD) {
        range_list_add(&holes, HT_BASE, HT_END);
    }
    struct range_list avoid = { 0 };
    for (size_t i = 0; i < pinned.count; i++) {
        range_list_add(&avoid, pinned.r[i].base, pinned.r[i].end);
    }
    for (size_t i = 0; i < holes.count; i++) {
        range_list_add(&avoid, holes.r[i].base, holes.r[i].end);
    }
    for (size_t i = 0; i < h->reserved_count; i++) {
        range_list_remove(
            &free_ram, h->reserved[i].base, h->reserved[i].base + h->reserved[i].size
        );
    }
    for (size_t i = 0; i < pinned.count; i++) {
        range_list_remove(&free_ram, pinned.r[i].base, pinned.r[i].end);
    }
    // The host's own legacy region is left alone, for its shadowing to be
    // probed after the guest programs the chipset, and so is its page 0, whose
    // address is the null pointer to the hypervisor's accesses.
    range_list_remove(&free_ram, VGA_BASE, ONE_MIB);
    range_list_remove(&free_ram, 0, PAGE_SIZE);

    uintptr_t low_hpa;
    if (!take_block(&free_ram, TWO_MIB, &low_hpa)) {
        panic("No 2 MiB block of host memory for the guest's low memory\n");
    }
    low = (uint8_t *)(uintptr_t)low_hpa;
    memset(low, 0, TWO_MIB);
    uint64_t available = list_total(&free_ram);

    // The PCI hole as high as its BARs allow, and the larger BARs above
    // whatever RAM the guest could have above 4 GiB.
    struct pci_plan_range *obstacles = kmalloc((avoid.count + 1) * sizeof(struct pci_plan_range));
    for (size_t i = 0; i < avoid.count; i++) {
        obstacles[i] = (struct pci_plan_range){ avoid.r[i].base, avoid.r[i].end };
    }
    uintptr_t pinned_top = FOUR_GIB;
    if (pinned.count != 0 && pinned.r[pinned.count - 1].end > pinned_top) {
        pinned_top = pinned.r[pinned.count - 1].end;
    }
    uint64_t high_floor = add_high(NULL, available, &avoid);
    high_floor = align_up(high_floor > pinned_top ? high_floor : pinned_top, ONE_GIB);
    const struct pci_plan *plan = pci_plan_make(
        obstacles, avoid.count, vga_lfb_size(), high_floor
    );
    kfree(obstacles);

    // From 1 MiB to the PCI hole, around what must keep its address, then
    // above 4 GiB what the host's memory has left.
    range_list_add(&ram, ONE_MIB, plan->tolud);
    for (size_t i = 0; i < pinned.count; i++) {
        range_list_remove(&ram, pinned.r[i].base, pinned.r[i].end);
    }
    lfb = plan->lfb;

    // The host's stolen memory where nothing backs RAM, clear of the host's
    // addresses for it.
    size_t stolen_size;
    uintptr_t clear_base;
    uintptr_t clear_end;
    if (igfx_stolen(&stolen_hpa, &stolen_size, &clear_base, &clear_end)) {
        uintptr_t gpa = take_top(&ram, stolen_size, TWO_MIB, clear_base, clear_end);
        if (gpa != 0) {
            stolen = (struct range){ gpa, gpa + stolen_size };
            igfx_stolen_placed(gpa);
        } else if (igfx_stolen_rmrr_dropped()) {
            printf("No room in the guest's memory for the stolen memory: it keeps its address\n");
            identity_add(
                clear_base, clear_end, EfiReservedMemoryType, efi_attribute_at(stolen_hpa)
            );
            range_list_add(&pinned, clear_base, clear_end);
            range_list_remove(&ram, clear_base, clear_end);
        }
    }
    struct range_list fill = { 0 };
    for (size_t i = 0; i < ram.count; i++) {
        range_list_add(&fill, ram.r[i].base, ram.r[i].end);
    }
    range_list_remove(&fill, 0, TWO_MIB);
    uint64_t wanted = list_total(&fill);
    if (wanted < available) {
        add_high(&fill, available - wanted, &avoid);
        wanted = available;
    } else if (wanted > available) {
        uint64_t sum = 0;
        for (size_t i = 0; i < fill.count; i++) {
            size_t size = fill.r[i].end - fill.r[i].base;
            if (sum + size > available) {
                uintptr_t cut = fill.r[i].base + (available - sum);
                // What the hypervisor's own use of the host's memory leaves
                // the guest without shows as reserved, as memory a BIOS keeps
                // does, for an OS not to take it for room for devices.
                for (size_t j = 0; j < ram.count; j++) {
                    if (ram.r[j].end > cut) {
                        range_list_add(
                            &kept, ram.r[j].base > cut ? ram.r[j].base : cut, ram.r[j].end
                        );
                    }
                }
                range_list_remove(&fill, cut, UINT64_MAX);
                range_list_remove(&ram, cut, UINT64_MAX);
                break;
            }
            sum += size;
        }
        wanted = available;
    }

    // Nothing is mapped but what the guest has: what reaches nothing reads as
    // absent.
    for (size_t i = 0; i < identity_count; i++) {
        const struct identity_range *r = &identity[i];
        gpt_map(r->base, r->base, r->end - r->base, GPT_RWX, identity_memtype(r));
    }
    if (stolen.end != 0) {
        struct identity_range r = {
            stolen_hpa, stolen_hpa + (stolen.end - stolen.base), EfiReservedMemoryType,
            efi_attribute_at(stolen_hpa)
        };
        gpt_map(stolen.base, stolen_hpa, stolen.end - stolen.base, GPT_RWX, identity_memtype(&r));
    }

    // Low memory and the ROM area, around the VGA window, which goes to the
    // VGA device, then the rest of the first 2 MiB.
    map_ram(0, low_hpa, VGA_BASE);
    gpt_map(VGA_BASE, VGA_BASE, VGA_END - VGA_BASE, GPT_RWX, MT_UC);
    map_ram(VGA_END, low_hpa + VGA_END, ONE_MIB - VGA_END);
    for (size_t i = 0; i < ram.count && ram.r[i].base < TWO_MIB; i++) {
        uintptr_t end = ram.r[i].end < TWO_MIB ? ram.r[i].end : TWO_MIB;
        map_ram(ram.r[i].base, low_hpa + ram.r[i].base, end - ram.r[i].base);
    }

    assign_blocks(&fill, &free_ram, ONE_GIB);
    assign_blocks(&fill, &free_ram, TWO_MIB);
    assign_rest(&fill, &free_ram);

    uint64_t below = 0;
    uint64_t above = 0;
    for (size_t i = 0; i < ram.count; i++) {
        if (ram.r[i].base < FOUR_GIB) {
            range_list_add(&takeable, ram.r[i].base, ram.r[i].end);
            below += ram.r[i].end - ram.r[i].base;
        } else {
            above += ram.r[i].end - ram.r[i].base;
        }
    }
    printf(
        "Guest RAM: %llu MiB from 1 MiB, to %#llx, and %llu MiB from 4 GiB\n",
        (unsigned long long)(below >> 20),
        (unsigned long long)(takeable.count != 0 ? takeable.r[takeable.count - 1].end : 0),
        (unsigned long long)(above >> 20)
    );

    range_list_free(&fill);
    range_list_free(&free_ram);
    range_list_free(&avoid);
}

uintptr_t layout_lfb(void) {
    return lfb;
}

bool layout_mmio_free(uintptr_t base, size_t size) {
    uintptr_t end = base + size;
    if (end < base || base < ONE_MIB) {
        return false;
    }
    if (lfb != 0 && base < lfb + vga_lfb_size() && end > lfb) {
        return false;
    }
    if (stolen.end != 0 && base < stolen.end && end > stolen.base) {
        return false;
    }
    const struct pci_plan *plan = pci_plan();
    if (plan != NULL && plan->ecam != 0) {
        uintptr_t ecam_end = plan->ecam + (((uint64_t)plan->bus_end + 1) << 20);
        if (base < ecam_end && end > plan->ecam) {
            return false;
        }
    }
    const struct range_list *lists[] = { &ram, &pinned, &kept, &holes };
    for (size_t l = 0; l < ARRAY_COUNT(lists); l++) {
        for (size_t i = 0; i < lists[l]->count; i++) {
            if (base < lists[l]->r[i].end && end > lists[l]->r[i].base) {
                return false;
            }
        }
    }
    return true;
}

uintptr_t layout_low_ram_end(void) {
    for (size_t i = 0; i < ram.count; i++) {
        if (ram.r[i].base <= ONE_MIB && ram.r[i].end > ONE_MIB) {
            return ram.r[i].end;
        }
    }
    return ONE_MIB;
}

uintptr_t layout_high_ram_end(void) {
    uintptr_t end = FOUR_GIB;
    for (size_t i = 0; i < ram.count; i++) {
        if (ram.r[i].end > end) {
            end = ram.r[i].end;
        }
    }
    return end;
}

bool layout_host_kept(uintptr_t base, uintptr_t end) {
    return !range_list_overlaps(&host_free, base, end);
}

bool layout_ram(uintptr_t gpa, size_t size) {
    uintptr_t end = gpa + size;
    if (end < gpa) {
        return false;
    }
    if (end <= VGA_BASE) {
        return true;
    }
    for (size_t i = 0; i < ram.count; i++) {
        if (gpa >= ram.r[i].base && end <= ram.r[i].end) {
            return true;
        }
    }
    return false;
}

static bool contiguous(uintptr_t gpa, size_t pages, uintptr_t *hpa) {
    uint32_t prot;
    for (size_t i = 0; i < pages; i++) {
        uint64_t at;
        if (!gpt_translate(gpa + i * PAGE_SIZE, &at, &prot, NULL)
         || (i != 0 && at != *hpa + i * PAGE_SIZE)) {
            return false;
        }
        if (i == 0) {
            *hpa = at;
        }
    }
    return true;
}

bool layout_contiguous(uintptr_t gpa, size_t pages, uintptr_t *hpa) {
    if (contiguous(gpa, pages, hpa)) {
        return true;
    }
    vcpu_pause_others();
    bool ok = contiguous(gpa, pages, hpa);
    if (!ok && moved_pages + pages <= MAX_MOVED_PAGES) {
        uint8_t *block = page_alloc(pages);
        for (size_t i = 0; i < pages; i++) {
            uintptr_t from;
            uint32_t prot;
            gpt_translate(gpa + i * PAGE_SIZE, &from, &prot, NULL);
            memcpy(block + i * PAGE_SIZE, (void *)(uintptr_t)from, PAGE_SIZE);
        }
        // The pages left behind stay unused, as the processor may still have
        // been told their addresses.
        map_ram(gpa, (uintptr_t)block, pages * PAGE_SIZE);
        gpt_flush();
        moved_pages += pages;
        *hpa = (uintptr_t)block;
        ok = true;
    }
    vcpu_resume_others();
    return ok;
}

void *layout_low(uintptr_t gpa) {
    if (gpa >= ONE_MIB || (gpa >= VGA_BASE && gpa < VGA_END)) {
        panic("No guest low memory at %#llx\n", (unsigned long long)gpa);
    }
    return low + gpa;
}

uint32_t layout_take_reserved(size_t size, uint32_t align, uintptr_t *hpa) {
    uint32_t gpa = layout_take(size, align, E820_RESERVED);
    if (gpa == 0) {
        return 0;
    }
    size_t pages = page_count(size);
    if (!contiguous(gpa, pages, hpa)) {
        *hpa = (uintptr_t)page_alloc(pages);
        map_ram(gpa, *hpa, pages * PAGE_SIZE);
    }
    return gpa;
}

uint32_t layout_take(size_t size, uint32_t align, uint32_t e820_type) {
    // Whole pages, as the memory map and the page tables have them.
    size = (uint32_t)page_up(size);
    if (align < PAGE_SIZE) {
        align = PAGE_SIZE;
    }
    for (size_t i = takeable.count; i-- > 0; ) {
        uintptr_t end = takeable.r[i].end;
        if (end - takeable.r[i].base < size) {
            continue;
        }
        uintptr_t base = align_down(end - size, align);
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
        range_list_remove(&takeable, base, end);
        return (uint32_t)base;
    }
    return 0;
}

void layout_absent(uintptr_t gpa, size_t size) {
    if (size == 0) {
        return;
    }
    if (ones_page == 0) {
        uint8_t *page = page_alloc(1);
        memset(page, 0xff, PAGE_SIZE);
        ones_page = (uintptr_t)page;
    }
    // Reads hit the page of ones, writes exit.
    mmio_register(gpa, size, &mmio_absent_ops, NULL);
    for (size_t offset = 0; offset < size; offset += PAGE_SIZE) {
        gpt_map(gpa + offset, ones_page, PAGE_SIZE, GPT_R | GPT_X, MT_WB);
    }
    gpt_flush();
}

static uint64_t rom_read(struct vcpu *v, void *ctx, uint64_t offset, size_t size) {
    (void)v;
    uint64_t value = 0;
    memcpy(&value, low + (uintptr_t)ctx + offset, size);
    return value;
}

static const struct mmio_ops rom_ops = { rom_read, mmio_absent_write, true };

void layout_read_only(uintptr_t gpa, size_t size) {
    if (size == 0 || gpa >= ONE_MIB || size > ONE_MIB - gpa) {
        return;
    }
    // Reads go to the memory, writes exit to be dropped.
    mmio_register(gpa, size, &rom_ops, (void *)(uintptr_t)gpa);
    gpt_map(gpa, (uintptr_t)(low + gpa), size, GPT_R | GPT_X, MT_WB);
    gpt_flush();
}

bool layout_e820(struct e820_map *map) {
    bool ok = true;
    e820_init(map);
    for (size_t i = 0; i < identity_count; i++) {
        const struct identity_range *r = &identity[i];
        EFI_MEMORY_DESCRIPTOR desc = {
            .Type = r->efi_type,
            .PhysicalStart = r->base,
            .NumberOfPages = (r->end - r->base) / PAGE_SIZE,
            .Attribute = r->attribute
        };
        ok &= e820_set_efi_map(
            map, &desc, sizeof(EFI_MEMORY_DESCRIPTOR), sizeof(EFI_MEMORY_DESCRIPTOR)
        );
    }
    for (size_t i = 0; i < ram.count; i++) {
        ok &= e820_set(map, ram.r[i].base, ram.r[i].end - ram.r[i].base, E820_RAM);
    }
    for (size_t i = 0; i < taken_count; i++) {
        ok &= e820_set(map, taken[i].base, taken[i].end - taken[i].base, taken[i].e820_type);
    }
    for (size_t i = 0; i < kept.count; i++) {
        ok &= e820_set(map, kept.r[i].base, kept.r[i].end - kept.r[i].base, E820_RESERVED);
    }
    if (stolen.end != 0) {
        ok &= e820_set(map, stolen.base, stolen.end - stolen.base, E820_RESERVED);
    }
    for (size_t i = 0; i < holes.count; i++) {
        ok &= e820_set(map, holes.r[i].base, holes.r[i].end - holes.r[i].base, E820_RESERVED);
    }
    const struct pci_plan *plan = pci_plan();
    if (plan != NULL && plan->ecam != 0) {
        ok &= e820_set(map, plan->ecam, ((uint64_t)plan->bus_end + 1) << 20, E820_RESERVED);
    }
    ok &= e820_set_low_memory(map);
    return ok;
}
