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

#ifndef HANDOFF_H
#define HANDOFF_H

#include <stdbool.h>
#include <stdint.h>

// The page tables the processor runs the guest's guests with, which the
// hypervisor builds from the guest's and its own.
#define NESTED_HEAP_SIZE ((uint64_t)128 << 20)

#define HANDOFF_MAX_RESERVED 16
#define HANDOFF_MAX_PCI_ROMS 16
#define HANDOFF_MAX_BLOCK_DEVICES 32
#define HANDOFF_MAX_CPU_RANGES 64
#define HANDOFF_MAX_BOOT_ORDER 4096

struct handoff_range {
    uint64_t base;
    uint64_t size;
};

struct handoff_pci_address {
    uint16_t segment;
    uint8_t bus;
    // Device in bits 7:3, function in bits 2:0.
    uint8_t devfn;
};

// How a picture is turned on a framebuffer whose panel is mounted turned in
// its casing, so as to stand upright: a quarter turn clockwise or
// anticlockwise.
#define HANDOFF_ROTATE_0 0
#define HANDOFF_ROTATE_90 1
#define HANDOFF_ROTATE_270 3

// A linear framebuffer as the firmware's graphics output left it. Each colour
// component is size bits wide, starting at bit shift of a pixel.
struct handoff_framebuffer {
    uint64_t base;
    uint64_t size;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint8_t bpp;
    uint8_t red_size;
    uint8_t red_shift;
    uint8_t green_size;
    uint8_t green_shift;
    uint8_t blue_size;
    uint8_t blue_shift;
    uint8_t reserved_size;
    uint8_t reserved_shift;
    bool present;
    // One of the HANDOFF_ROTATE_* values.
    uint8_t rotation;
};

// An x86 legacy option ROM image, copied to hypervisor memory.
struct handoff_pci_rom {
    struct handoff_pci_address address;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t base_class;
    uint8_t subclass;
    uint8_t prog_if;
    uint32_t size;
    uint64_t image;
};

#define HANDOFF_MAX_EDID 256

struct handoff_video {
    struct handoff_framebuffer framebuffer;
    // The PCI device that owns the legacy VGA resources, if any.
    bool vga_present;
    // Whether that device's legacy video BIOS is in vbios.
    bool vbios_present;
    uint16_t vga_vendor_id;
    uint16_t vga_device_id;
    struct handoff_pci_address vga_device;
    struct handoff_pci_rom vbios;
    // The emulated VGA's video memory, if the VGA is to be emulated.
    uint64_t vram_base;
    uint64_t vram_size;
    // The framebuffer's display's EDID, its first blocks, as the firmware has
    // it.
    uint32_t edid_size;
    uint8_t edid[HANDOFF_MAX_EDID];
};

// Block devices the firmware knew of, as BIOS Boot Specification 1.01 device
// types.
struct handoff_block_device {
    struct handoff_pci_address address;
    uint8_t bbs_type;
    uint8_t base_class;
    uint8_t subclass;
    // ReBIOS was loaded from this device.
    bool boot;
};

#define HANDOFF_CPU_FILTER_NONE 0
#define HANDOFF_CPU_FILTER_ALLOW 1
#define HANDOFF_CPU_FILTER_BLOCK 2

// Whether the hypervisor emulates a VGA: when the firmware hands over no
// usable video BIOS, always, or never.
#define HANDOFF_VGA_EMULATION_AUTO 0
#define HANDOFF_VGA_EMULATION_ALWAYS 1
#define HANDOFF_VGA_EMULATION_NEVER 2

// How an emulated VGA's display fills the framebuffer: all of it, as much of
// it as 4:3 allows, at one framebuffer pixel per VGA pixel, at vga_scale
// pixels per pixel, or at the most whole pixels per pixel that fit; centered
// unless stretched.
#define HANDOFF_VGA_SCALING_STRETCH 0
#define HANDOFF_VGA_SCALING_ASPECT 1
#define HANDOFF_VGA_SCALING_CENTER 2
#define HANDOFF_VGA_SCALING_MANUAL 3
#define HANDOFF_VGA_SCALING_INTEGER 4

// When AMD's AVIC runs the guest's local APICs: on the processors it serves by
// default (virt/svm.c), on all that have it but those AMD advises against it
// on, or never.
#define HANDOFF_AVIC_AUTO 0
#define HANDOFF_AVIC_ALWAYS 1
#define HANDOFF_AVIC_NEVER 2

struct handoff_config {
    uint8_t serial;
    uint8_t verbose;
    uint8_t vga_specified;
    uint16_t serial_port;
    uint32_t serial_baud;
    struct handoff_pci_address vga_device;
    uint8_t vga_emulation;
    uint8_t vga_scaling;
    uint8_t vga_scale;
    uint8_t profile;
    // Whether the guest gets the processor's virtualisation of its own.
    uint8_t nested;
    // One of the HANDOFF_AVIC_* values.
    uint8_t avic;
    // Whether the guest starts with CPUID's basic leaves limited to 2, as
    // firmware setting IA32_MISC_ENABLE's Limit CPUID Maxval would have it.
    uint8_t cpuid_limit;
    // Whether Intel's integrated graphics get their stolen memory at an
    // address of the guest's own (virt/igfx.c), else at the host's.
    uint8_t igpu_stolen_remap;
    // Whether the hypervisor has the USB controllers until the OS takes them
    // (virt/usb.c), else the OS has them from the start.
    uint8_t legacy_usb;
    // Which APIC IDs the guest gets, as inclusive ranges.
    uint32_t cpu_filter;
    uint32_t cpu_range_count;
    struct {
        uint32_t first;
        uint32_t last;
    } cpu_ranges[HANDOFF_MAX_CPU_RANGES];
};

// What the loader passes to the core.
struct handoff {
    // Memory the hypervisor keeps, which the guest must never reach: ReBIOS's
    // own image, the core's heap, page tables, and what the loader hands over.
    struct handoff_range reserved[HANDOFF_MAX_RESERVED];
    uint32_t reserved_count;

    // The page tables the core starts on, identity mapping the bottom of the
    // physical address space up to map_top.
    uint64_t pml4;
    uint64_t map_top;

    // A page below 1 MiB, where the other processors start, or 0.
    uint64_t ap_boot;

    // Memory the core manages itself, NESTED_HEAP_SIZE more of it where the
    // guest gets VMX or SVM of its own.
    uint64_t heap_base;
    uint64_t heap_size;

    // The TSC's rate, measured against the firmware's delay service.
    uint64_t tsc_hz;

    // The memory map current at ExitBootServices().
    uint64_t efi_mmap;
    uint64_t efi_mmap_size;
    uint64_t efi_mmap_desc_size;
    uint32_t efi_mmap_desc_version;

    // The firmware's tables, which stay where they are.
    uint64_t efi_system_table;
    uint64_t efi_rt;
    uint64_t acpi_rsdp;
    // An SMBIOS 2.x entry point below 4GiB, with the structure table below
    // 4GiB, or 0.
    uint64_t smbios_entry;

    struct handoff_config config;
    struct handoff_video video;

    uint32_t pci_rom_count;
    uint32_t block_device_count;
    struct handoff_pci_rom pci_roms[HANDOFF_MAX_PCI_ROMS];
    struct handoff_block_device block_devices[HANDOFF_MAX_BLOCK_DEVICES];

    // The legacy BIOS's boot order: the drives as the lines of SeaBIOS's
    // bootorder file name them, the first to be tried first; NUL-terminated.
    char boot_order[HANDOFF_MAX_BOOT_ORDER];
};

#endif
