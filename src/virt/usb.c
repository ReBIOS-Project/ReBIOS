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
#include <tendril/glue.h>
#include <tendril/hid.h>
#include <tendril/tendril.h>
#include <virt/usb.h>
#include <hw/pci_cfg.h>
#include <hw/pci_enum.h>
#include <lib/heap.h>
#include <lib/log.h>
#include <lib/time.h>
#include <memory.h>
#include <sys/mm.h>
#include <sys/smp.h>
#include <sys/task.h>
#include <sys/x86.h>
#include <virt/hv_timer.h>
#include <virt/kbc.h>
#include <virt/layout.h>
#include <virt/usb_controller.h>
#include <virt/usb_disk.h>
#include <virt/vcpu.h>

// USB legacy support, as a BIOS's is to a PC: the hypervisor has the machine's
// controllers until the OS takes them, with their keyboards and mice feeding
// the 8042 and their disks the legacy BIOS. All of Tendril runs in one task on
// the boot processor, whose waits leave the guest its time.

#define MAX_CONTROLLERS 16
#define MAX_KEYBOARDS 8

#define PCI_CLASS_SERIAL_BUS 0x0c
#define PCI_SUBCLASS_USB 0x03
#define UHCI_BAR 4

// Enough for a controller's rings and scratchpad, and a transfer's bounce
// buffer, each; devices reach it where the guest's memory map keeps it for
// them.
#define POOL_EACH ((size_t)4 << 20)
#define POOL_ALIGN ((uint32_t)1 << 21)

// Without interrupts it all goes by polling, seldom with nothing to look
// after.
#define POLL_NS ((uint64_t)4000000)
#define POLL_IDLE_NS ((uint64_t)50000000)
// What the guest gets between slices of work under way, a slice being
// CPU_SLICE_NS long. The library's waits go by the clock, so that they take no
// longer for it.
#define WORK_GAP_NS ((uint64_t)3000000)

// The span of a PS/2 mouse's counts an absolute pointer's range is made into.
#define ABSOLUTE_SPAN 2048

// Linux 7.3's drivers/usb/host/pci-quirks.c, usb_disable_xhci_ports().
#define INTEL_XUSB2PR 0xd0
#define INTEL_USB3_PSSEN 0xd8

static struct usb_controller controllers[MAX_CONTROLLERS];
static size_t controller_count;
static struct task task;
static bool started;
static struct hv_timer tick;
static uint64_t tick_wake;
static bool all_ready;
// The task did not get to the end of its work in its last slice.
static bool mid_work;

static uint8_t *pool;
static uintptr_t pool_bus;
static size_t pool_pages;
static uint8_t *pool_used;

struct usb_hid {
    struct tendril_hid *hid;
    uint8_t held[32];
    bool placed;
    int32_t x;
    int32_t y;
};

static struct tendril_hid *keyboards[MAX_KEYBOARDS];
static uint8_t leds;
static size_t hid_devices;

void *usb_dma_alloc(size_t size, size_t align, uintptr_t *bus) {
    size_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    size_t step = align > PAGE_SIZE ? align / PAGE_SIZE : 1;
    // The pool itself is aligned as far as anyone asks.
    for (size_t first = 0; first + pages <= pool_pages; first += step) {
        size_t n = 0;
        while (n < pages && pool_used[first + n] == 0) {
            n++;
        }
        if (n == pages) {
            memset(pool_used + first, 1, pages);
            *bus = pool_bus + first * PAGE_SIZE;
            return pool + first * PAGE_SIZE;
        }
    }
    return NULL;
}

void usb_dma_free(void *ptr, size_t size) {
    size_t first = (size_t)((uint8_t *)ptr - pool) / PAGE_SIZE;
    memset(pool_used + first, 0, (size + PAGE_SIZE - 1) / PAGE_SIZE);
}

void *tendril_glue_alloc(size_t size) {
    return kmalloc(size);
}

void tendril_glue_free(void *ptr, size_t size) {
    (void)size;
    kfree(ptr);
}

void *tendril_glue_dma_alloc(
    void *ctx, size_t size, size_t align, bool dma32, uint64_t *dma
) {
    // All of it is below 4 GiB.
    (void)ctx;
    (void)dma32;
    uintptr_t bus;
    void *ptr = usb_dma_alloc(size, align, &bus);
    if (ptr != NULL) {
        *dma = bus;
    }
    return ptr;
}

void tendril_glue_dma_free(void *ctx, void *ptr, size_t size) {
    (void)ctx;
    usb_dma_free(ptr, size);
}

// Anything else the library bounces: the IOMMU has device addresses only for
// the guest's memory, which the guest's addresses are.
size_t tendril_glue_dma_map(
    void *ctx, void *ptr, size_t size, int dir, uint64_t *dma
) {
    (void)ctx;
    (void)dir;
    uint8_t *p = ptr;
    if (p < pool || p >= pool + pool_pages * PAGE_SIZE
     || size > (size_t)(pool + pool_pages * PAGE_SIZE - p)) {
        return 0;
    }
    *dma = pool_bus + (size_t)(p - pool);
    return size;
}

void tendril_glue_dma_unmap(
    void *ctx, void *ptr, size_t size, int dir, uint64_t dma
) {
    (void)ctx;
    (void)ptr;
    (void)size;
    (void)dir;
    (void)dma;
}

void tendril_glue_dma_barrier(void) {
    asm volatile ("mfence" :: : "memory");
}

// Registers can be slow, and the library has nothing under way that minds the
// guest going on in between.
uint16_t tendril_glue_read16(void *ctx, uint32_t offset) {
    struct usb_controller *c = ctx;
    task_yield_if_over();
    if (c->mmio == NULL) {
        return inw((uint16_t)(c->port + offset));
    }
    return *(volatile uint16_t *)(c->mmio + offset);
}

uint32_t tendril_glue_read32(void *ctx, uint32_t offset) {
    struct usb_controller *c = ctx;
    task_yield_if_over();
    if (c->mmio == NULL) {
        return inl((uint16_t)(c->port + offset));
    }
    return *(volatile uint32_t *)(c->mmio + offset);
}

void tendril_glue_write16(void *ctx, uint32_t offset, uint16_t value) {
    struct usb_controller *c = ctx;
    asm volatile ("" :: : "memory");
    if (c->mmio == NULL) {
        outw((uint16_t)(c->port + offset), value);
    } else {
        *(volatile uint16_t *)(c->mmio + offset) = value;
    }
}

void tendril_glue_write32(void *ctx, uint32_t offset, uint32_t value) {
    struct usb_controller *c = ctx;
    asm volatile ("" :: : "memory");
    if (c->mmio == NULL) {
        outl((uint16_t)(c->port + offset), value);
    } else {
        *(volatile uint32_t *)(c->mmio + offset) = value;
    }
}

uint8_t tendril_glue_pci_read8(void *ctx, uint16_t offset) {
    struct usb_controller *c = ctx;
    return (uint8_t)pci_cfg_read(c->f->addr, offset, 1);
}

uint16_t tendril_glue_pci_read16(void *ctx, uint16_t offset) {
    struct usb_controller *c = ctx;
    return (uint16_t)pci_cfg_read(c->f->addr, offset, 2);
}

uint32_t tendril_glue_pci_read32(void *ctx, uint16_t offset) {
    struct usb_controller *c = ctx;
    return pci_cfg_read(c->f->addr, offset, 4);
}

void tendril_glue_pci_write8(void *ctx, uint16_t offset, uint8_t value) {
    struct usb_controller *c = ctx;
    pci_cfg_write(c->f->addr, offset, 1, value);
}

void tendril_glue_pci_write16(void *ctx, uint16_t offset, uint16_t value) {
    struct usb_controller *c = ctx;
    pci_cfg_write(c->f->addr, offset, 2, value);
}

void tendril_glue_pci_write32(void *ctx, uint16_t offset, uint32_t value) {
    struct usb_controller *c = ctx;
    pci_cfg_write(c->f->addr, offset, 4, value);
}

bool tendril_glue_pci_find_read8(
    void *ctx, uint16_t vendor, uint16_t device, uint16_t offset,
    uint8_t *value
) {
    (void)ctx;
    const struct pci_topology *t = pci_enum_topology();
    for (size_t i = 0; t != NULL && i < t->function_count; i++) {
        const struct pci_function *f = &t->functions[i];
        if (f->vendor_id == vendor && f->device_id == device) {
            *value = (uint8_t)pci_cfg_read(f->addr, offset, 1);
            return true;
        }
    }
    return false;
}

// The library's waits read the time as they go, which is where they let the
// guest go on.
uint64_t tendril_glue_time_ns(void) {
    task_yield_if_over();
    return time_ns();
}

// One task does all of it.
void tendril_glue_lock(void *ctx) {
    (void)ctx;
}

void tendril_glue_unlock(void *ctx) {
    (void)ctx;
}

void tendril_glue_log(int level, const char *message) {
    (void)level;
    printf("USB: %s\n", message);
}

static void hid_attach(struct tendril_hid *hid) {
    struct usb_hid *h = kmalloc(sizeof(struct usb_hid));
    h->hid = hid;
    tendril_hid_set_user(hid, h);
    hid_devices++;
    if ((tendril_hid_kinds(hid) & TENDRIL_HID_KEYBOARD) == 0) {
        return;
    }
    for (size_t i = 0; i < MAX_KEYBOARDS; i++) {
        if (keyboards[i] == NULL) {
            keyboards[i] = hid;
            tendril_hid_set_leds(hid, leds);
            break;
        }
    }
}

// The keys it still holds come up, as they would not for the guest otherwise.
static void hid_detach(struct tendril_hid *hid) {
    struct usb_hid *h = tendril_hid_user(hid);
    for (size_t i = 0; i < MAX_KEYBOARDS; i++) {
        if (keyboards[i] == hid) {
            keyboards[i] = NULL;
        }
    }
    for (size_t key = 0; key < sizeof(h->held) * 8; key++) {
        if ((h->held[key / 8] & (1 << (key % 8))) != 0) {
            kbc_key((uint16_t)key, false, 0);
        }
    }
    kbc_pointer(0, 0, 0, 0);
    kfree(h);
    hid_devices--;
}

static void hid_key(
    struct tendril_hid *hid, uint16_t key, bool pressed, uint8_t modifiers
) {
    struct usb_hid *h = tendril_hid_user(hid);
    if (key / 8 < sizeof(h->held)) {
        uint8_t bit = (uint8_t)(1 << (key % 8));
        if (pressed) {
            h->held[key / 8] |= bit;
        } else {
            h->held[key / 8] &= (uint8_t)~bit;
        }
    }
    kbc_key(key, pressed, modifiers);
}

static int32_t absolute_counts(int32_t value, int32_t min, int32_t max) {
    int64_t span = (int64_t)max - min + 1;
    if (span <= 0) {
        return 0;
    }
    return (int32_t)(((int64_t)value - min) * ABSOLUTE_SPAN / span);
}

static void hid_pointer(
    struct tendril_hid *hid, const struct tendril_hid_pointer_event *event
) {
    int32_t dx = event->dx;
    int32_t dy = event->dy;
    if (event->absolute) {
        struct usb_hid *h = tendril_hid_user(hid);
        int32_t x = absolute_counts(event->x, event->x_min, event->x_max);
        int32_t y = absolute_counts(event->y, event->y_min, event->y_max);
        dx = h->placed ? x - h->x : 0;
        dy = h->placed ? y - h->y : 0;
        h->placed = true;
        h->x = x;
        h->y = y;
    }
    kbc_pointer(dx, dy, event->wheel, event->buttons);
}

static const struct tendril_hid_callbacks hid_callbacks = {
    .attach = hid_attach,
    .detach = hid_detach,
    .key = hid_key,
    .pointer = hid_pointer
};

static void leds_follow(void) {
    uint8_t now = kbc_leds();
    if (now == leds) {
        return;
    }
    leds = now;
    for (size_t i = 0; i < MAX_KEYBOARDS; i++) {
        if (keyboards[i] != NULL) {
            tendril_hid_set_leds(keyboards[i], now);
        }
    }
}

static void controller_let_go(struct usb_controller *c) {
    if (c->hc != NULL) {
        bool routes = c->type == TENDRIL_HC_XHCI && c->ports_to_ehci;
        tendril_hc_remove(c->hc);
        c->hc = NULL;
        if (routes) {
            pci_cfg_write(c->f->addr, INTEL_USB3_PSSEN, 4, 0);
            pci_cfg_write(c->f->addr, INTEL_XUSB2PR, 4, 0);
        }
    }
    __atomic_store_n(&c->state, USB_STATE_RELEASED, __ATOMIC_RELEASE);
    usb_legacy_disown(c);
    // An OS without xHCI's driver wants the ports its EHCI driver can reach,
    // as a BIOS's setting that leaves them to the OS gives it.
    if (c->type != TENDRIL_HC_EHCI) {
        return;
    }
    for (size_t i = 0; i < controller_count; i++) {
        struct usb_controller *x = &controllers[i];
        if (x->hc != NULL
         && (tendril_hc_quirks(x->hc)
           & TENDRIL_HC_QUIRK_INTEL_PORT_ROUTING) != 0) {
            x->ports_to_ehci = true;
            usb_release(x);
        }
    }
}

static void controller_take(struct usb_controller *c) {
    struct tendril_hc_config config = {
        .type = c->type, .ctx = c, .pci = true, .irq = false, .quirks = 0
    };
    int status = tendril_hc_add(&config, &c->hc);
    if (status != TENDRIL_OK) {
        printf(
            "USB: controller %02x:%02x.%x stays the guest's: %s\n",
            c->f->addr.bus, c->f->addr.device, c->f->addr.function,
            tendril_status_str(status)
        );
        controller_let_go(c);
        return;
    }
    __atomic_store_n(&c->state, USB_STATE_OWNED, __ATOMIC_RELEASE);
}

static void usb_main(void *arg) {
    (void)arg;
    tendril_hid_register(&hid_callbacks, 0);
    usb_disk_register();
    for (;;) {
        uint64_t next = UINT64_MAX;
        bool ready = true;
        for (size_t i = 0; i < controller_count; i++) {
            struct usb_controller *c = &controllers[i];
            int state = __atomic_load_n(&c->state, __ATOMIC_ACQUIRE);
            if (state == USB_STATE_RELEASED) {
                continue;
            }
            if (__atomic_load_n(&c->release_wanted, __ATOMIC_ACQUIRE)) {
                controller_let_go(c);
                continue;
            }
            if (state == USB_STATE_WAITING) {
                controller_take(c);
            }
            if (c->hc != NULL) {
                uint64_t deadline = tendril_hc_poll(c->hc);
                next = deadline < next ? deadline : next;
                ready = ready && tendril_hc_ready(c->hc);
            }
        }
        usb_disk_work();
        leds_follow();
        __atomic_store_n(&all_ready, ready, __ATOMIC_RELEASE);
        uint64_t poll = time_ns()
            + (hid_devices != 0 || !ready ? POLL_NS : POLL_IDLE_NS);
        task_sleep(next < poll ? next : poll);
    }
}

bool usb_ready(void) {
    return !started || __atomic_load_n(&all_ready, __ATOMIC_ACQUIRE);
}

void usb_kick(void) {
    task_wake(&task);
    vcpu_kick(smp_cpu(0));
}

void usb_release(struct usb_controller *c) {
    if (!__atomic_exchange_n(&c->release_wanted, true, __ATOMIC_ACQ_REL)) {
        usb_kick();
    }
}

struct usb_controller *usb_controller_of(const struct tendril_hc *hc) {
    for (size_t i = 0; i < controller_count; i++) {
        if (controllers[i].hc == hc) {
            return &controllers[i];
        }
    }
    return NULL;
}

static void controller_add(const struct pci_function *f, int type) {
    if (controller_count == MAX_CONTROLLERS) {
        return;
    }
    struct usb_controller *c = &controllers[controller_count];
    c->f = f;
    c->type = type;
    size_t index = type == TENDRIL_HC_UHCI ? UHCI_BAR : 0;
    const struct pci_bar *bar = &f->bars[index];
    if (bar->size == 0 || bar->base == 0) {
        return;
    }
    if (type == TENDRIL_HC_UHCI) {
        if (bar->kind != PCI_BAR_IO) {
            return;
        }
        c->port = (uint16_t)bar->base;
        c->port_count = bar->size;
    } else {
        if (bar->kind == PCI_BAR_IO) {
            return;
        }
        mm_map_device(bar->base, bar->size);
        c->mmio = (volatile uint8_t *)bar->base;
        c->mmio_size = bar->size;
    }
    controller_count++;
}

static void tick_fired(void *ctx) {
    (void)ctx;
}

void usb_init(void) {
    const struct pci_topology *t = pci_enum_topology();
    for (size_t i = 0; t != NULL && i < t->function_count; i++) {
        const struct pci_function *f = &t->functions[i];
        int type;
        uint32_t class_code = (uint32_t)f->base_class << 16
            | (uint32_t)f->subclass << 8 | f->prog_if;
        if (f->base_class == PCI_CLASS_SERIAL_BUS
         && f->subclass == PCI_SUBCLASS_USB
         && tendril_pci_hc_type(class_code, &type)) {
            controller_add(f, type);
        }
    }
    if (controller_count == 0) {
        return;
    }

    size_t size = (controller_count + 1) * POOL_EACH;
    uintptr_t hpa;
    pool_bus = layout_take_reserved(size, POOL_ALIGN, &hpa);
    if (pool_bus == 0) {
        printf("USB: no memory for its devices to reach\n");
        controller_count = 0;
        return;
    }
    pool = (uint8_t *)hpa;
    pool_pages = size / PAGE_SIZE;
    pool_used = kmalloc(pool_pages);

    for (size_t i = 0; i < controller_count; i++) {
        struct usb_controller *c = &controllers[i];
        if (!usb_legacy_own(c)) {
            printf(
                "USB: controller %02x:%02x.%x stays the guest's\n",
                c->f->addr.bus, c->f->addr.device, c->f->addr.function
            );
            c->state = USB_STATE_RELEASED;
        }
    }
    tick.fired = tick_fired;
    hv_timer_add(&tick);
}

void usb_start(void) {
    if (controller_count == 0) {
        return;
    }
    task_init(&task, usb_main, NULL);
    __atomic_store_n(&started, true, __ATOMIC_RELEASE);
}

void usb_slice(bool guest_waiting) {
    if (!__atomic_load_n(&started, __ATOMIC_ACQUIRE)) {
        return;
    }
    if (guest_waiting && mid_work) {
        task_wake(&task);
    }
    uint64_t before = time_ns();
    if (__atomic_load_n(&task.wake, __ATOMIC_ACQUIRE) <= before) {
        task_run(&task);
        mid_work = __atomic_load_n(&task.wake, __ATOMIC_ACQUIRE) <= time_ns();
        if (mid_work) {
            __atomic_store_n(
                &task.wake, time_ns() + WORK_GAP_NS, __ATOMIC_RELEASE
            );
        }
    }
    // A timer brings the next one, as the guest may not.
    uint64_t now = time_ns();
    uint64_t wake = __atomic_load_n(&task.wake, __ATOMIC_ACQUIRE);
    uint64_t armed = __atomic_load_n(&tick.deadline, __ATOMIC_ACQUIRE);
    if (wake > now && wake == tick_wake && armed != 0) {
        return;
    }
    tick_wake = wake;
    uint64_t delay = wake > now ? wake - now : 0;
    hv_timer_set(&tick, rdtsc() + delay * time_tsc_hz() / 1000000000);
}

bool usb_working(void) {
    return __atomic_load_n(&started, __ATOMIC_ACQUIRE)
        && (mid_work
         || __atomic_load_n(&task.wake, __ATOMIC_ACQUIRE) <= time_ns());
}
