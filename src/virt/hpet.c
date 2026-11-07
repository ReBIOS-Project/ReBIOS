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

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <uacpi/acpi.h>
#include <uacpi/platform/types.h>
#include <uacpi/status.h>
#include <uacpi/tables.h>
#include <virt/hpet.h>
#include <lib/log.h>
#include <lib/spinlock.h>
#include <lib/util.h>
#include <memory.h>
#include <sys/mm.h>
#include <virt/gpt.h>
#include <virt/irq.h>
#include <virt/layout.h>
#include <virt/mmio.h>
#include <virt/vcpu.h>

// IA-PC HPET Specification 1.0a, 2.3: the capabilities and configuration
// registers, and each timer's configuration register.
#define HPET_SIZE ((uint32_t)0x1000)
#define CAP_TIMERS_SHIFT 8
#define CAP_TIMERS_MASK ((uint32_t)0x1f)
#define CONFIG ((uint32_t)0x10)
#define CONFIG_ENABLE ((uint32_t)1 << 0)
#define CONFIG_LEGACY_ROUTE ((uint32_t)1 << 1)
#define TIMER_BASE ((uint32_t)0x100)
#define TIMER_STRIDE ((uint32_t)0x20)
#define TIMER_INT_ENABLE ((uint32_t)1 << 2)
#define TIMER_FSB_ENABLE ((uint32_t)1 << 14)
#define TIMER_FSB_CAP ((uint32_t)1 << 15)
// The upper half of a timer's configuration register: the I/O APIC inputs it
// can interrupt on (2.3.8).
#define TIMER_ROUTE_CAP ((uint32_t)4)
#define ROUTE_LINES 32
// What legacy replacement routes timers 0 and 1 to (2.3.9.1), which the
// guest's interrupts follow on their own (irq.c's reaches_guest()).
#define LEGACY_TIMER_IRQ 0
#define LEGACY_RTC_IRQ 8
#define MAX_HIDDEN 8

static uint64_t hidden[MAX_HIDDEN];
static size_t hidden_count;
// Where the guest's is: the first it gets, the only one a PC has.
static uintptr_t guest_base;
// Whether its legacy replacement route is on, as the guest's writes to its
// configuration register, which pass through the hypervisor, leave it.
static _Atomic(bool) legacy_route;
static struct spinlock lock;

static uint32_t read32(uintptr_t address) {
    return *(volatile uint32_t *)(uintptr_t)address;
}

static void write32(uintptr_t address, uint32_t value) {
    *(volatile uint32_t *)(uintptr_t)address = value;
}

static unsigned int timers(uintptr_t base) {
    return ((read32(base) >> CAP_TIMERS_SHIFT) & CAP_TIMERS_MASK) + 1;
}

static bool fsb_capable(uintptr_t base) {
    for (size_t n = 0; n < timers(base); n++) {
        if ((read32(base + TIMER_BASE + n * TIMER_STRIDE) & TIMER_FSB_CAP) != 0) {
            return true;
        }
    }
    return false;
}

// Its timers interrupt no more, and the PIT and the RTC interrupt as without
// it; the counter runs on, as firmware may time its own delays with it.
static void hide(uintptr_t base) {
    for (size_t n = 0; n < timers(base); n++) {
        uintptr_t config = base + TIMER_BASE + n * TIMER_STRIDE;
        write32(config, read32(config) & ~(TIMER_INT_ENABLE | TIMER_FSB_ENABLE));
    }
    write32(base + CONFIG, read32(base + CONFIG) & ~CONFIG_LEGACY_ROUTE);
    layout_absent(base, HPET_SIZE);
    hidden[hidden_count++] = base;
    printf(
        "HPET at %#llx hidden from the guest, as its timers can deliver through the FSB\n",
        (unsigned long long)base
    );
}

// Whether the route changed.
static bool follow_route(void) {
    uint32_t route = CONFIG_ENABLE | CONFIG_LEGACY_ROUTE;
    bool on = (read32(guest_base + CONFIG) & route) == route;
    return atomic_exchange_explicit(&legacy_route, on, memory_order_acq_rel) != on;
}

static uint64_t guest_read(struct vcpu *v, void *ctx, uint64_t offset, size_t size) {
    (void)v;
    return mmio_read_host((uintptr_t)ctx + offset, size);
}

static void guest_write(struct vcpu *v, void *ctx, uint64_t offset, size_t size, uint64_t value) {
    (void)v;
    uintptr_t address = (uintptr_t)ctx + offset;
    spinlock_acquire(&lock);
    mmio_write_host(address, size, value);
    bool changed = address < guest_base + CONFIG + 8 && address + size > guest_base + CONFIG
                && follow_route();
    spinlock_release(&lock);
    if (changed) {
        irq_hpet_route_changed();
    }
}

static const struct mmio_ops guest_ops = { guest_read, guest_write, false };

// The guest reads its registers where they are, but its writes pass through
// the hypervisor, which so knows whether the 8254 and the RTC interrupt or
// timers 0 and 1 in their place (IA-PC HPET Specification 1.0a, 2.4.2.1).
static void trap_writes(uintptr_t base) {
    uintptr_t page = page_down(base);
    follow_route();
    mmio_register(page, PAGE_SIZE, &guest_ops, (void *)page);
    gpt_map(page, page, PAGE_SIZE, GPT_R, MT_UC);
    gpt_flush();
}

// The lines the guest's HPET can raise, which reach the guest then, but for
// those of IRQs 0 and 8, which the 8254 and the RTC may drive.
static void give_lines(uintptr_t base) {
    uint32_t legacy_timer = irq_isa_gsi(LEGACY_TIMER_IRQ);
    uint32_t legacy_rtc = irq_isa_gsi(LEGACY_RTC_IRQ);
    for (size_t n = 0; n < timers(base); n++) {
        uint32_t routes = read32(base + TIMER_BASE + n * TIMER_STRIDE + TIMER_ROUTE_CAP);
        for (size_t gsi = 0; gsi < ROUTE_LINES; gsi++) {
            if ((routes & ((uint32_t)1 << gsi)) != 0 && gsi != legacy_timer
             && gsi != legacy_rtc) {
                irq_give_gsi((uint32_t)gsi);
            }
        }
    }
}

void hpet_init(void) {
    struct uacpi_table table;
    for (size_t n = 0;
         uacpi_table_find_nth_by_signature(ACPI_HPET_SIGNATURE, n, &table) == UACPI_STATUS_OK;
         n++) {
        if (table.hdr->length >= sizeof(struct acpi_hpet)) {
            struct acpi_hpet hpet;
            memcpy(&hpet, table.ptr, sizeof(struct acpi_hpet));
            uintptr_t base = hpet.address.address;
            if (hpet.address.address_space_id == ACPI_AS_ID_SYS_MEM
             && (base & (PAGE_SIZE - 1)) == 0 && hidden_count < MAX_HIDDEN && fsb_capable(base)) {
                hide(base);
            } else if (hpet.address.address_space_id == ACPI_AS_ID_SYS_MEM && guest_base == 0) {
                guest_base = base;
                trap_writes(base);
                give_lines(base);
            }
        }
        uacpi_table_unref(&table);
    }
}

bool hpet_hidden(uintptr_t base) {
    for (size_t i = 0; i < hidden_count; i++) {
        if (hidden[i] == base) {
            return true;
        }
    }
    return false;
}

bool hpet_any_hidden(void) {
    return hidden_count != 0;
}

bool hpet_block_id(uintptr_t base, uint32_t *id) {
    if ((base & (PAGE_SIZE - 1)) != 0) {
        return false;
    }
    *id = read32(base);
    return true;
}

bool hpet_replaces_pit(void) {
    return atomic_load_explicit(&legacy_route, memory_order_acquire);
}
