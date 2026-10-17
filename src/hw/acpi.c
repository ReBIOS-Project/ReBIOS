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
#include <uacpi/acpi.h>
#include <uacpi/io.h>
#include <uacpi/kernel_api.h>
#include <uacpi/tables.h>
#include <uacpi/uacpi.h>
#include <uacpi/event.h>
#include <uacpi/utilities.h>
#include <core.h>
#include <handoff.h>
#include <hw/acpi.h>
#include <hw/ec.h>
#include <hw/pci_cfg.h>
#include <lib/heap.h>
#include <lib/log.h>
#include <lib/spinlock.h>
#include <lib/time.h>
#include <sys/cpu.h>
#include <sys/x86.h>

// Holds the table list until uacpi_initialize() moves it to the heap, at
// about 56 bytes for each table beyond the 16 uACPI has room for itself.
#define EARLY_TABLE_BUFFER_SIZE 8192

// Larger requests only come from broken AML, and uACPI copes with them
// failing, where kmalloc() would panic.
#define MAX_ALLOCATION (16u << 20)

#define WAIT_FOREVER 0xffff

// The global lock's event is signalled by the SCI handler, which does not run
// while the hypervisor waits with interrupts off. uACPI tries the lock again
// after each wait, so waits forever poll it instead.
#define FOREVER_POLL_MS 1

struct io_range {
    uint16_t base;
    uint32_t length;
};

struct event {
    uint64_t counter;
};

struct work {
    struct work *next;
    uacpi_work_handler handler;
    uacpi_handle ctx;
};

static bool init_done;
static bool init_ok;
static bool tables_ok;

static uint8_t interrupt_handle;
static uacpi_interrupt_handler sci_handler;
static uacpi_handle sci_ctx;
static uint32_t sci_irq;

static struct spinlock lock;
static struct spinlock work_lock;
static struct work *work_head;
static struct work *work_tail;

uacpi_status uacpi_kernel_get_rsdp(uacpi_phys_addr *out_rsdp_address) {
    if (core_handoff->acpi_rsdp == 0) {
        return UACPI_STATUS_NOT_FOUND;
    }
    *out_rsdp_address = core_handoff->acpi_rsdp;
    return UACPI_STATUS_OK;
}

void *uacpi_kernel_map(uacpi_phys_addr addr, uacpi_size len) {
    uint64_t top = core_handoff->map_top;
    if (len > top || addr > top - len) {
        return UACPI_MAP_FAILED;
    }
    return (void *)(uintptr_t)addr;
}

void uacpi_kernel_unmap(void *addr, uacpi_size len) {
    (void)addr;
    (void)len;
}

void uacpi_kernel_log(uacpi_log_level level, const uacpi_char *message) {
    const char *name;
    switch (level) {
        case UACPI_LOG_ERROR: {
            name = "error";
            break;
        }
        case UACPI_LOG_WARN: {
            name = "warning";
            break;
        }
        case UACPI_LOG_INFO: {
            name = "info";
            break;
        }
        default: {
            return;
        }
    }

    // Messages cut short to fit uACPI's buffer lose their newline.
    size_t length = 0;
    while (message[length] != '\0') {
        length++;
    }
    bool newline = length > 0 && message[length - 1] == '\n';
    printf("uACPI %s: %s%s", name, message, newline ? "" : "\n");
}

uacpi_status uacpi_kernel_pci_device_open(uacpi_pci_address address, uacpi_handle *out_handle) {
    struct pci_addr *addr = kmalloc(sizeof(*addr));
    addr->segment = address.segment;
    addr->bus = address.bus;
    addr->device = address.device;
    addr->function = address.function;
    *out_handle = addr;
    return UACPI_STATUS_OK;
}

void uacpi_kernel_pci_device_close(uacpi_handle handle) {
    kfree(handle);
}

static uacpi_status pci_read(uacpi_handle device, uacpi_size offset, unsigned size,
                             uint32_t *value) {
    if (offset >= 4096 || offset % size != 0) {
        return UACPI_STATUS_INVALID_ARGUMENT;
    }
    const struct pci_addr *addr = device;
    *value = pci_cfg_read(*addr, (uint32_t)offset, size);
    return UACPI_STATUS_OK;
}

static uacpi_status pci_write(uacpi_handle device, uacpi_size offset, unsigned size,
                              uint32_t value) {
    if (offset >= 4096 || offset % size != 0) {
        return UACPI_STATUS_INVALID_ARGUMENT;
    }
    const struct pci_addr *addr = device;
    pci_cfg_write(*addr, (uint32_t)offset, size, value);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_pci_read8(uacpi_handle device, uacpi_size offset, uacpi_u8 *value) {
    uint32_t v = 0;
    uacpi_status status = pci_read(device, offset, 1, &v);
    *value = (uacpi_u8)v;
    return status;
}

uacpi_status uacpi_kernel_pci_read16(uacpi_handle device, uacpi_size offset, uacpi_u16 *value) {
    uint32_t v = 0;
    uacpi_status status = pci_read(device, offset, 2, &v);
    *value = (uacpi_u16)v;
    return status;
}

uacpi_status uacpi_kernel_pci_read32(uacpi_handle device, uacpi_size offset, uacpi_u32 *value) {
    uint32_t v = 0;
    uacpi_status status = pci_read(device, offset, 4, &v);
    *value = v;
    return status;
}

uacpi_status uacpi_kernel_pci_write8(uacpi_handle device, uacpi_size offset, uacpi_u8 value) {
    return pci_write(device, offset, 1, value);
}

uacpi_status uacpi_kernel_pci_write16(uacpi_handle device, uacpi_size offset, uacpi_u16 value) {
    return pci_write(device, offset, 2, value);
}

uacpi_status uacpi_kernel_pci_write32(uacpi_handle device, uacpi_size offset, uacpi_u32 value) {
    return pci_write(device, offset, 4, value);
}

uacpi_status uacpi_kernel_io_map(uacpi_io_addr base, uacpi_size len, uacpi_handle *out_handle) {
    if (base > 0xffff || len > 0x10000 - base) {
        return UACPI_STATUS_INVALID_ARGUMENT;
    }
    struct io_range *range = kmalloc(sizeof(*range));
    range->base = (uint16_t)base;
    range->length = (uint32_t)len;
    *out_handle = range;
    return UACPI_STATUS_OK;
}

void uacpi_kernel_io_unmap(uacpi_handle handle) {
    kfree(handle);
}

static bool io_port(uacpi_handle handle, uacpi_size offset, unsigned size, uint16_t *port) {
    const struct io_range *range = handle;
    if (offset > range->length || range->length - offset < size) {
        return false;
    }
    *port = (uint16_t)(range->base + offset);
    return true;
}

uacpi_status uacpi_kernel_io_read8(uacpi_handle handle, uacpi_size offset, uacpi_u8 *out_value) {
    uint16_t port;
    if (!io_port(handle, offset, 1, &port)) {
        return UACPI_STATUS_INVALID_ARGUMENT;
    }
    *out_value = inb(port);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_read16(uacpi_handle handle, uacpi_size offset, uacpi_u16 *out_value) {
    uint16_t port;
    if (!io_port(handle, offset, 2, &port)) {
        return UACPI_STATUS_INVALID_ARGUMENT;
    }
    *out_value = inw(port);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_read32(uacpi_handle handle, uacpi_size offset, uacpi_u32 *out_value) {
    uint16_t port;
    if (!io_port(handle, offset, 4, &port)) {
        return UACPI_STATUS_INVALID_ARGUMENT;
    }
    *out_value = inl(port);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_write8(uacpi_handle handle, uacpi_size offset, uacpi_u8 in_value) {
    uint16_t port;
    if (!io_port(handle, offset, 1, &port)) {
        return UACPI_STATUS_INVALID_ARGUMENT;
    }
    outb(port, in_value);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_write16(uacpi_handle handle, uacpi_size offset, uacpi_u16 in_value) {
    uint16_t port;
    if (!io_port(handle, offset, 2, &port)) {
        return UACPI_STATUS_INVALID_ARGUMENT;
    }
    outw(port, in_value);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_write32(uacpi_handle handle, uacpi_size offset, uacpi_u32 in_value) {
    uint16_t port;
    if (!io_port(handle, offset, 4, &port)) {
        return UACPI_STATUS_INVALID_ARGUMENT;
    }
    outl(port, in_value);
    return UACPI_STATUS_OK;
}

void *uacpi_kernel_alloc(uacpi_size size) {
    if (size > MAX_ALLOCATION) {
        return NULL;
    }
    return kmalloc(size);
}

void uacpi_kernel_free(void *mem) {
    kfree(mem);
}

uacpi_u64 uacpi_kernel_get_nanoseconds_since_boot(void) {
    return time_ns();
}

void uacpi_kernel_stall(uacpi_u8 usec) {
    time_udelay(usec);
}

void uacpi_kernel_sleep(uacpi_u64 msec) {
    // A second at a time, as time_udelay() overflows on long delays.
    while (msec > 0) {
        uint64_t step = msec < 1000 ? msec : 1000;
        time_udelay(step * 1000);
        msec -= step;
    }
}

static bool lock_try(struct spinlock *lock) {
    return !__atomic_exchange_n(&lock->locked, true, __ATOMIC_ACQUIRE);
}

static bool timed_out(uint64_t start_ns, uacpi_u16 timeout_ms) {
    return timeout_ms != WAIT_FOREVER && time_ns() - start_ns >= (uint64_t)timeout_ms * 1000000;
}

uacpi_handle uacpi_kernel_create_mutex(void) {
    return kmalloc(sizeof(struct spinlock));
}

void uacpi_kernel_free_mutex(uacpi_handle handle) {
    kfree(handle);
}

uacpi_status uacpi_kernel_acquire_mutex(uacpi_handle handle, uacpi_u16 timeout) {
    uint64_t start = time_ns();
    while (!lock_try(handle)) {
        if (timed_out(start, timeout)) {
            return UACPI_STATUS_TIMEOUT;
        }
        cpu_relax();
    }
    return UACPI_STATUS_OK;
}

void uacpi_kernel_release_mutex(uacpi_handle handle) {
    spinlock_release(handle);
}

uacpi_handle uacpi_kernel_create_event(void) {
    return kmalloc(sizeof(struct event));
}

void uacpi_kernel_free_event(uacpi_handle handle) {
    kfree(handle);
}

uacpi_bool uacpi_kernel_wait_for_event(uacpi_handle handle, uacpi_u16 timeout) {
    struct event *event = handle;
    if (timeout == WAIT_FOREVER) {
        timeout = FOREVER_POLL_MS;
    }
    uint64_t start = time_ns();
    for (;;) {
        uint64_t count = __atomic_load_n(&event->counter, __ATOMIC_RELAXED);
        while (count != 0) {
            if (__atomic_compare_exchange_n(&event->counter, &count, count - 1, false,
                                            __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
                return UACPI_TRUE;
            }
        }
        if (timed_out(start, timeout)) {
            return UACPI_FALSE;
        }
        cpu_relax();
    }
}

void uacpi_kernel_signal_event(uacpi_handle handle) {
    struct event *event = handle;
    __atomic_fetch_add(&event->counter, 1, __ATOMIC_RELEASE);
}

void uacpi_kernel_reset_event(uacpi_handle handle) {
    struct event *event = handle;
    __atomic_store_n(&event->counter, 0, __ATOMIC_RELEASE);
}

uacpi_thread_id uacpi_kernel_get_thread_id(void) {
    return this_cpu();
}

static uint64_t read_rflags(void) {
    uint64_t rflags;
    asm volatile ("pushfq\n\tpopq %0" : "=r"(rflags) :: "memory");
    return rflags;
}

// Interrupts are always disabled in the core.
uacpi_interrupt_state uacpi_kernel_disable_interrupts(void) {
    return read_rflags();
}

void uacpi_kernel_restore_interrupts(uacpi_interrupt_state state) {
    (void)state;
}

uacpi_handle uacpi_kernel_create_spinlock(void) {
    return kmalloc(sizeof(struct spinlock));
}

void uacpi_kernel_free_spinlock(uacpi_handle handle) {
    kfree(handle);
}

uacpi_cpu_flags uacpi_kernel_lock_spinlock(uacpi_handle handle) {
    uacpi_cpu_flags flags = read_rflags();
    spinlock_acquire(handle);
    return flags;
}

void uacpi_kernel_unlock_spinlock(uacpi_handle handle, uacpi_cpu_flags flags) {
    (void)flags;
    spinlock_release(handle);
}

uacpi_status uacpi_kernel_handle_firmware_request(uacpi_firmware_request *request) {
    switch (request->type) {
        case UACPI_FIRMWARE_REQUEST_TYPE_BREAKPOINT: {
            printf("AML breakpoint\n");
            break;
        }
        case UACPI_FIRMWARE_REQUEST_TYPE_FATAL: {
            printf("AML fatal error, type %#x, code %#x, argument %#llx\n", request->fatal.type,
                   request->fatal.code, (unsigned long long)request->fatal.arg);
            break;
        }
        default: {
            printf("Unknown AML firmware request %u\n", request->type);
            break;
        }
    }
    return UACPI_STATUS_OK;
}

// Kept for acpi_sci_interrupt(), whose caller connects the SCI.
uacpi_status uacpi_kernel_install_interrupt_handler(uacpi_u32 irq, uacpi_interrupt_handler handler,
                                                    uacpi_handle ctx,
                                                    uacpi_handle *out_irq_handle) {
    sci_irq = irq;
    sci_handler = handler;
    sci_ctx = ctx;
    *out_irq_handle = &interrupt_handle;
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_uninstall_interrupt_handler(uacpi_interrupt_handler handler,
                                                      uacpi_handle irq_handle) {
    (void)handler;
    (void)irq_handle;
    sci_handler = NULL;
    return UACPI_STATUS_OK;
}

// Work runs when the processor that queued it releases acpi_lock(), in the
// order it came in: GPE methods and Notify() handlers alike.
uacpi_status uacpi_kernel_schedule_work(uacpi_work_type type, uacpi_work_handler handler,
                                        uacpi_handle ctx) {
    (void)type;
    struct work *w = kmalloc(sizeof(*w));
    w->handler = handler;
    w->ctx = ctx;
    spinlock_acquire(&work_lock);
    if (work_tail != NULL) {
        work_tail->next = w;
    } else {
        work_head = w;
    }
    work_tail = w;
    spinlock_release(&work_lock);
    return UACPI_STATUS_OK;
}

static void work_run(void) {
    for (;;) {
        spinlock_acquire(&work_lock);
        struct work *w = work_head;
        if (w != NULL) {
            work_head = w->next;
            if (work_head == NULL) {
                work_tail = NULL;
            }
        }
        spinlock_release(&work_lock);
        if (w == NULL) {
            return;
        }
        w->handler(w->ctx);
        kfree(w);
    }
}

uacpi_status uacpi_kernel_wait_for_work_completion(void) {
    work_run();
    return UACPI_STATUS_OK;
}

void acpi_lock(void) {
    spinlock_acquire(&lock);
}

void acpi_unlock(void) {
    work_run();
    spinlock_release(&lock);
}

bool acpi_sci_irq(uint32_t *irq) {
    if (sci_handler == NULL) {
        return false;
    }
    *irq = sci_irq;
    return true;
}

void acpi_sci_interrupt(void *ctx) {
    (void)ctx;
    acpi_lock();
    if (sci_handler != NULL) {
        sci_handler(sci_ctx);
    }
    acpi_unlock();
}

void acpi_namespace_start(void) {
    acpi_lock();
    ec_init();
    uacpi_status status = uacpi_namespace_initialize();
    if (status != UACPI_STATUS_OK) {
        printf("Cannot initialise the ACPI namespace: %s\n", uacpi_status_to_string(status));
    }
    // Again, should _INI have undone it.
    status = uacpi_set_interrupt_model(UACPI_INTERRUPT_MODEL_IOAPIC);
    if (status != UACPI_STATUS_OK) {
        printf("Cannot put the firmware in APIC mode: %s\n", uacpi_status_to_string(status));
    }
    status = uacpi_finalize_gpe_initialization();
    if (status != UACPI_STATUS_OK) {
        printf("Cannot enable the ACPI GPEs: %s\n", uacpi_status_to_string(status));
    }
    acpi_unlock();
}

bool acpi_tables_ready(void) {
    return tables_ok;
}

bool acpi_init(void) {
    if (init_done) {
        return init_ok;
    }
    init_done = true;

    if (core_handoff->acpi_rsdp == 0) {
        printf("No ACPI RSDP, running without ACPI\n");
        return false;
    }

    void *buffer = kmalloc(EARLY_TABLE_BUFFER_SIZE);
    uacpi_status status = uacpi_setup_early_table_access(buffer, EARLY_TABLE_BUFFER_SIZE);
    if (status != UACPI_STATUS_OK) {
        printf("Cannot access the ACPI tables: %s\n", uacpi_status_to_string(status));
        kfree(buffer);
        return false;
    }
    tables_ok = true;

    status = uacpi_initialize(0);
    if (status != UACPI_STATUS_OK) {
        printf("Cannot initialise uACPI: %s\n", uacpi_status_to_string(status));
    } else {
        status = uacpi_namespace_load();
        if (status != UACPI_STATUS_OK) {
            printf("Cannot load the ACPI namespace: %s\n", uacpi_status_to_string(status));
        }
    }

    if (status != UACPI_STATUS_OK) {
        // uACPI drops all of its state on failure, its tables included.
        status = uacpi_setup_early_table_access(buffer, EARLY_TABLE_BUFFER_SIZE);
        if (status != UACPI_STATUS_OK) {
            printf("Cannot access the ACPI tables again: %s\n", uacpi_status_to_string(status));
            tables_ok = false;
            kfree(buffer);
        }
        return false;
    }

    // uacpi_initialize() moved the table list to the heap.
    kfree(buffer);
    init_ok = true;
    return true;
}
