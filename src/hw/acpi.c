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
#include <uacpi/event.h>
#include <uacpi/io.h>
#include <uacpi/kernel_api.h>
#include <uacpi/log.h>
#include <uacpi/namespace.h>
#include <uacpi/platform/arch_helpers.h>
#include <uacpi/platform/types.h>
#include <uacpi/resources.h>
#include <uacpi/status.h>
#include <uacpi/tables.h>
#include <uacpi/types.h>
#include <uacpi/uacpi.h>
#include <uacpi/utilities.h>
#include <hw/acpi.h>
#include <core.h>
#include <handoff.h>
#include <hw/cmos.h>
#include <hw/ec.h>
#include <hw/pci_cfg.h>
#include <hw/pci_regs.h>
#include <lib/heap.h>
#include <lib/log.h>
#include <lib/spinlock.h>
#include <lib/time.h>
#include <memory.h>
#include <sys/cpu.h>
#include <sys/task.h>
#include <sys/x86.h>

// Holds the table list until uacpi_initialize() moves it to the heap, at about
// 56 bytes for each table beyond the 16 uACPI has room for itself.
#define EARLY_TABLE_BUFFER_SIZE 8192

// Larger requests only come from broken AML, and uACPI copes with them
// failing, where kmalloc() would panic.
#define MAX_ALLOCATION ((uint32_t)16 << 20)

#define WAIT_FOREVER 0xffff

// Events, semaphores created with no units, are only signalled by the SCI
// handler (the global lock's release) or AML, neither of which runs while the
// hypervisor waits outside the AML task with interrupts off. uACPI tries the
// global lock again after each wait, so waits forever on events poll instead.
#define FOREVER_POLL_MS 1

struct io_range {
    uint16_t base;
    size_t length;
};

struct semaphore {
    _Atomic(uint64_t) units;
    bool event;
};

struct work {
    struct work *next;
    uacpi_work_handler handler;
    uacpi_handle ctx;
    bool bsp;
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

// The work uACPI defers runs as a task, so that what the firmware's AML waits
// for keeps no CPU from the guest for long: once the time slice of the exit
// that runs the task is over, the task carries on at a later exit, on
// whichever CPU. Whether it has work, which lasts until the task ran out of
// it, whether a CPU runs it, and whether only the boot processor may.
static struct task aml_task;
static _Atomic(bool) aml_started;
static _Atomic(bool) aml_wanted;
static _Atomic(bool) aml_running;
static _Atomic(bool) aml_bsp;

uacpi_status uacpi_kernel_get_rsdp(uacpi_phys_addr *out_rsdp_address) {
    if (core_handoff->acpi_rsdp == 0) {
        return UACPI_STATUS_NOT_FOUND;
    }
    *out_rsdp_address = core_handoff->acpi_rsdp;
    return UACPI_STATUS_OK;
}

void *uacpi_kernel_map(uacpi_phys_addr addr, uacpi_size len) {
    uintptr_t top = core_handoff->map_top;
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
    struct pci_cfg_addr *addr = kmalloc(sizeof(struct pci_cfg_addr));
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

static uacpi_status pci_read(
    uacpi_handle device, uacpi_size offset, size_t size, uint32_t *value
) {
    if (offset >= PCI_CFG_SPACE_EXP_SIZE || offset % size != 0) {
        return UACPI_STATUS_INVALID_ARGUMENT;
    }
    const struct pci_cfg_addr *addr = device;
    *value = pci_cfg_read(*addr, (uint32_t)offset, size);
    return UACPI_STATUS_OK;
}

static uacpi_status pci_write(
    uacpi_handle device, uacpi_size offset, size_t size, uint32_t value
) {
    if (offset >= PCI_CFG_SPACE_EXP_SIZE || offset % size != 0) {
        return UACPI_STATUS_INVALID_ARGUMENT;
    }
    const struct pci_cfg_addr *addr = device;
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
    struct io_range *range = kmalloc(sizeof(struct io_range));
    range->base = (uint16_t)base;
    range->length = (uint32_t)len;
    *out_handle = range;
    return UACPI_STATUS_OK;
}

void uacpi_kernel_io_unmap(uacpi_handle handle) {
    kfree(handle);
}

static bool io_port(uacpi_handle handle, uacpi_size offset, size_t size, uint16_t *port) {
    const struct io_range *range = handle;
    if (offset > range->length || range->length - offset < size) {
        return false;
    }
    *port = (uint16_t)(range->base + offset);
    return true;
}

// The RTC's ports go through hw/cmos, which the guest's RTC shares, a byte at
// a time, as the chipset runs them.
#define CMOS_FIRST 0x70
#define CMOS_LAST 0x77

static uint32_t io_access(uint16_t port, size_t size, bool in, uint32_t value) {
    if (port + size <= CMOS_FIRST || port > CMOS_LAST) {
        switch (size) {
            case 1: {
                if (in) {
                    return inb(port);
                }
                outb(port, (uint8_t)value);
                break;
            }
            case 2: {
                if (in) {
                    return inw(port);
                }
                outw(port, (uint16_t)value);
                break;
            }
            default: {
                if (in) {
                    return inl(port);
                }
                outl(port, value);
                break;
            }
        }
        return value;
    }
    for (size_t i = 0; i < size; i++) {
        uint16_t p = (uint16_t)(port + i);
        uint8_t byte = (uint8_t)(value >> (8 * i));
        if (p >= CMOS_FIRST && p <= CMOS_LAST) {
            cmos_host_io(p, in, &byte);
        } else if (in) {
            byte = inb(p);
        } else {
            outb(p, byte);
        }
        if (in) {
            value = (value & ~((uint32_t)0xff << (8 * i))) | (uint32_t)byte << (8 * i);
        }
    }
    return value;
}

uacpi_status uacpi_kernel_io_read8(uacpi_handle handle, uacpi_size offset, uacpi_u8 *out_value) {
    uint16_t port;
    if (!io_port(handle, offset, 1, &port)) {
        return UACPI_STATUS_INVALID_ARGUMENT;
    }
    *out_value = (uacpi_u8)io_access(port, 1, true, 0);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_read16(uacpi_handle handle, uacpi_size offset, uacpi_u16 *out_value) {
    uint16_t port;
    if (!io_port(handle, offset, 2, &port)) {
        return UACPI_STATUS_INVALID_ARGUMENT;
    }
    *out_value = (uacpi_u16)io_access(port, 2, true, 0);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_read32(uacpi_handle handle, uacpi_size offset, uacpi_u32 *out_value) {
    uint16_t port;
    if (!io_port(handle, offset, 4, &port)) {
        return UACPI_STATUS_INVALID_ARGUMENT;
    }
    *out_value = io_access(port, 4, true, 0);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_write8(uacpi_handle handle, uacpi_size offset, uacpi_u8 in_value) {
    uint16_t port;
    if (!io_port(handle, offset, 1, &port)) {
        return UACPI_STATUS_INVALID_ARGUMENT;
    }
    io_access(port, 1, false, in_value);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_write16(uacpi_handle handle, uacpi_size offset, uacpi_u16 in_value) {
    uint16_t port;
    if (!io_port(handle, offset, 2, &port)) {
        return UACPI_STATUS_INVALID_ARGUMENT;
    }
    io_access(port, 2, false, in_value);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_write32(uacpi_handle handle, uacpi_size offset, uacpi_u32 in_value) {
    uint16_t port;
    if (!io_port(handle, offset, 4, &port)) {
        return UACPI_STATUS_INVALID_ARGUMENT;
    }
    io_access(port, 4, false, in_value);
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

// uACPI looks at the time at each turn of an AML While loop, where the AML
// task yields too.
uacpi_u64 uacpi_kernel_get_nanoseconds_since_boot(void) {
    task_yield_if_over();
    return time_ns();
}

void uacpi_kernel_stall(uacpi_u8 usec) {
    time_udelay(usec);
}

void uacpi_kernel_sleep(uacpi_u64 msec) {
    uintptr_t end = time_ns() + msec * 1000000;
    if (task_current() != NULL) {
        task_sleep(end);
        return;
    }
    while (time_ns() < end) {
        cpu_relax();
    }
}

static bool timed_out(uint64_t start_ns, uacpi_u16 timeout_ms) {
    return timeout_ms != WAIT_FOREVER && time_ns() - start_ns >= (uint64_t)timeout_ms * 1000000;
}

uacpi_handle uacpi_kernel_create_semaphore(uacpi_u32 initial_units) {
    struct semaphore *semaphore = kmalloc(sizeof(struct semaphore));
    atomic_init(&semaphore->units, initial_units);
    semaphore->event = initial_units == 0;
    return semaphore;
}

void uacpi_kernel_free_semaphore(uacpi_handle handle) {
    kfree(handle);
}

uacpi_status uacpi_kernel_wait_for_semaphore(uacpi_handle handle, uacpi_u16 timeout) {
    struct semaphore *semaphore = handle;
    if (timeout == WAIT_FOREVER && semaphore->event) {
        timeout = FOREVER_POLL_MS;
    }
    uintptr_t start = time_ns();
    for (;;) {
        uint64_t units = atomic_load_explicit(&semaphore->units, memory_order_relaxed);
        while (units != 0) {
            bool taken = atomic_compare_exchange_strong_explicit(
                &semaphore->units, &units, units - 1, memory_order_acquire, memory_order_relaxed
            );
            if (taken) {
                return UACPI_STATUS_OK;
            }
        }
        if (timed_out(start, timeout)) {
            return UACPI_STATUS_TIMEOUT;
        }
        if (task_current() != NULL) {
            task_yield();
        } else {
            // What holds it may be the AML task, which this CPU then runs on.
            acpi_slice();
            cpu_relax();
        }
    }
}

void uacpi_kernel_signal_semaphore(uacpi_handle handle) {
    struct semaphore *semaphore = handle;
    atomic_fetch_add_explicit(&semaphore->units, 1, memory_order_release);
}

// The AML task's mutexes stay its own on whichever CPU it carries on.
uacpi_thread_id uacpi_kernel_get_thread_id(void) {
    struct task *t = task_current();
    if (t != NULL) {
        return t;
    }
    return this_cpu();
}

static uint64_t read_rflags(void) {
    uint64_t rflags;
    asm volatile (
        "pushfq\n\t"
        "popq %0"
        : "=r"(rflags)
        :
        : "memory"
    );
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
            printf(
                "AML fatal error, type %#x, code %#x, argument %#llx\n", request->fatal.type,
                request->fatal.code, (unsigned long long)request->fatal.arg
            );
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
uacpi_status uacpi_kernel_install_interrupt_handler(
    uacpi_u32 irq, uacpi_interrupt_handler handler, uacpi_handle ctx, uacpi_handle *out_irq_handle
) {
    sci_irq = irq;
    sci_handler = handler;
    sci_ctx = ctx;
    *out_irq_handle = &interrupt_handle;
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_uninstall_interrupt_handler(
    uacpi_interrupt_handler handler, uacpi_handle irq_handle
) {
    (void)handler;
    (void)irq_handle;
    sci_handler = NULL;
    return UACPI_STATUS_OK;
}

uacpi_handle uacpi_kernel_create_work_item(void) {
    return kmalloc(sizeof(struct work));
}

void uacpi_kernel_free_work_item(uacpi_handle handle) {
    kfree(handle);
}

// Work runs in the order it came in, GPE methods and Notify() handlers alike:
// in the AML task, or before it started, when the CPU that queued it releases
// acpi_lock(). The task runs GPE methods on the boot processor only, as Linux
// 7.3's acpi_os_execute() does, since SMIs that AML sets off from other
// processors corrupt memory on some machines.
void uacpi_kernel_schedule_work(
    uacpi_work_type type, uacpi_handle work_item, uacpi_work_handler handler, uacpi_handle ctx
) {
    struct work *w = work_item;
    w->next = NULL;
    w->handler = handler;
    w->ctx = ctx;
    w->bsp = type == UACPI_WORK_GPE_EXECUTION;
    spinlock_acquire(&work_lock);
    if (work_tail != NULL) {
        work_tail->next = w;
    } else {
        work_head = w;
    }
    work_tail = w;
    spinlock_release(&work_lock);
    atomic_store_explicit(&aml_wanted, true, memory_order_seq_cst);
}

// Takes the oldest work off the queue, as a copy, since its handler may
// schedule its item again or free it: false if there is none.
static bool work_take(struct work *w) {
    spinlock_acquire(&work_lock);
    struct work *head = work_head;
    if (head != NULL) {
        *w = *head;
        work_head = head->next;
        if (work_head == NULL) {
            work_tail = NULL;
        }
    }
    spinlock_release(&work_lock);
    return head != NULL;
}

// In the AML task, a slice at a time, and GPE methods on the boot processor,
// which alone carries the task on meanwhile.
static void work_run(void) {
    bool in_task = task_current() != NULL;
    bool outer = atomic_load_explicit(&aml_bsp, memory_order_relaxed);
    struct work w;
    while (work_take(&w)) {
        if (in_task) {
            atomic_store_explicit(&aml_bsp, outer || w.bsp, memory_order_release);
            while (w.bsp && !this_cpu()->bsp) {
                task_yield();
            }
        }
        w.handler(w.ctx);
        if (in_task) {
            atomic_store_explicit(&aml_bsp, outer, memory_order_release);
            task_yield_if_over();
        }
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
    if (!atomic_load_explicit(&aml_started, memory_order_acquire)) {
        work_run();
    }
    spinlock_release(&lock);
}

static void aml_main(void *arg) {
    (void)arg;
    for (;;) {
        work_run();
        // Work that comes meanwhile makes aml_wanted true again.
        atomic_store_explicit(&aml_wanted, false, memory_order_seq_cst);
        spinlock_acquire(&work_lock);
        bool more = work_head != NULL;
        spinlock_release(&work_lock);
        if (more) {
            atomic_store_explicit(&aml_wanted, true, memory_order_seq_cst);
        } else {
            task_yield();
        }
    }
}

void acpi_slice(void) {
    bool elsewhere = atomic_load_explicit(&aml_bsp, memory_order_acquire) && !this_cpu()->bsp;
    if (!acpi_working() || elsewhere
     || atomic_exchange_explicit(&aml_running, true, memory_order_acquire)) {
        return;
    }
    task_run(&aml_task);
    atomic_store_explicit(&aml_running, false, memory_order_release);
}

bool acpi_working(void) {
    return atomic_load_explicit(&aml_started, memory_order_acquire)
        && atomic_load_explicit(&aml_wanted, memory_order_acquire);
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
    task_init(&aml_task, aml_main, NULL);
    atomic_store_explicit(&aml_started, true, memory_order_release);
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

static uacpi_iteration_decision device_found(
    void *user, uacpi_namespace_node *node, uacpi_u32 depth
) {
    (void)node;
    (void)depth;
    *(bool *)user = true;
    return UACPI_ITERATION_DECISION_BREAK;
}

bool acpi_has_device(const char *const *ids) {
    if (!init_ok) {
        return false;
    }
    bool found = false;
    acpi_lock();
    uacpi_find_devices_at(uacpi_namespace_root(), ids, device_found, &found);
    acpi_unlock();
    return found;
}

struct port_claim {
    uint16_t port;
    bool claimed;
};

static uacpi_iteration_decision resource_claims(void *user, uacpi_resource *resource) {
    struct port_claim *claim = user;
    uint32_t base;
    uint32_t length;
    switch (resource->type) {
        case UACPI_RESOURCE_TYPE_IO: {
            base = resource->io.minimum;
            length = resource->io.length;
            break;
        }
        case UACPI_RESOURCE_TYPE_FIXED_IO: {
            base = resource->fixed_io.address;
            length = resource->fixed_io.length;
            break;
        }
        default: {
            return UACPI_ITERATION_DECISION_CONTINUE;
        }
    }
    if (claim->port >= base && claim->port - base < length) {
        claim->claimed = true;
        return UACPI_ITERATION_DECISION_BREAK;
    }
    return UACPI_ITERATION_DECISION_CONTINUE;
}

static uacpi_iteration_decision device_claims(
    void *user, uacpi_namespace_node *node, uacpi_u32 depth
) {
    (void)depth;
    struct port_claim *claim = user;
    uacpi_u32 sta;
    if (uacpi_eval_sta(node, &sta) != UACPI_STATUS_OK
     || (sta & (ACPI_STA_RESULT_DEVICE_PRESENT | ACPI_STA_RESULT_DEVICE_FUNCTIONING)) == 0) {
        return UACPI_ITERATION_DECISION_NEXT_PEER;
    }
    uacpi_for_each_device_resource(node, "_CRS", resource_claims, claim);
    return claim->claimed ? UACPI_ITERATION_DECISION_BREAK : UACPI_ITERATION_DECISION_CONTINUE;
}

struct memory_claim {
    uint64_t base;
    uint64_t end;
    bool claimed;
};

// The memory descriptors Linux 7.3's check_mcfg_resource() reads.
static uacpi_iteration_decision memory_claims(void *user, uacpi_resource *resource) {
    struct memory_claim *claim = user;
    uint64_t base;
    uint64_t length;
    switch (resource->type) {
        case UACPI_RESOURCE_TYPE_FIXED_MEMORY32: {
            base = resource->fixed_memory32.address;
            length = resource->fixed_memory32.length;
            break;
        }
        case UACPI_RESOURCE_TYPE_ADDRESS32: {
            if (resource->address32.common.type != UACPI_RANGE_MEMORY) {
                return UACPI_ITERATION_DECISION_CONTINUE;
            }
            base = resource->address32.minimum;
            length = resource->address32.address_length;
            break;
        }
        case UACPI_RESOURCE_TYPE_ADDRESS64: {
            if (resource->address64.common.type != UACPI_RANGE_MEMORY) {
                return UACPI_ITERATION_DECISION_CONTINUE;
            }
            base = resource->address64.minimum;
            length = resource->address64.address_length;
            break;
        }
        default: {
            return UACPI_ITERATION_DECISION_CONTINUE;
        }
    }
    if (claim->base >= base && claim->end - base <= length) {
        claim->claimed = true;
        return UACPI_ITERATION_DECISION_BREAK;
    }
    return UACPI_ITERATION_DECISION_CONTINUE;
}

static uacpi_iteration_decision board_claims(
    void *user, uacpi_namespace_node *node, uacpi_u32 depth
) {
    (void)depth;
    struct memory_claim *claim = user;
    uacpi_for_each_device_resource(node, "_CRS", memory_claims, claim);
    return claim->claimed ? UACPI_ITERATION_DECISION_BREAK : UACPI_ITERATION_DECISION_CONTINUE;
}

bool acpi_motherboard_reserves(uint64_t base, uint64_t end) {
    if (!init_ok) {
        return false;
    }
    static const char *const system_board[] = { "PNP0C01", NULL };
    static const char *const motherboard[] = { "PNP0C02", NULL };
    struct memory_claim claim = { base, end, false };
    acpi_lock();
    uacpi_find_devices_at(uacpi_namespace_root(), system_board, board_claims, &claim);
    if (!claim.claimed) {
        uacpi_find_devices_at(uacpi_namespace_root(), motherboard, board_claims, &claim);
    }
    acpi_unlock();
    return claim.claimed;
}

bool acpi_claims_port(uint16_t port) {
    if (!init_ok) {
        return false;
    }
    struct port_claim claim = { port, false };
    acpi_lock();
    uacpi_namespace_for_each_child(
        uacpi_namespace_root(), device_claims, NULL, UACPI_OBJECT_DEVICE_BIT, UACPI_MAX_DEPTH_ANY,
        &claim
    );
    acpi_unlock();
    return claim.claimed;
}

static const char unknown_path[] = "?";

const char *acpi_path(uacpi_namespace_node *node) {
    const char *path = uacpi_namespace_node_generate_absolute_path(node);
    return path != NULL ? path : unknown_path;
}

void acpi_path_free(const char *path) {
    if (path != unknown_path) {
        uacpi_free_absolute_path(path);
    }
}

const char *const acpi_host_bridge_ids[] = { "PNP0A03", "PNP0A08", NULL };
const char *const acpi_link_ids[] = { "PNP0C0F", NULL };

void acpi_log_failure(uacpi_namespace_node *node, const char *name, uacpi_status status) {
    const char *path = acpi_path(node);
    printf("Cannot evaluate %s.%s: %s\n", path, name, uacpi_status_to_string(status));
    acpi_path_free(path);
}

bool acpi_eval_optional(
    uacpi_namespace_node *node, const char *name, uint64_t *value, bool *found
) {
    uacpi_status status = uacpi_eval_simple_integer(node, name, value);
    if (found != NULL) {
        *found = status == UACPI_STATUS_OK;
    }
    if (status == UACPI_STATUS_NOT_FOUND) {
        *value = 0;
        return true;
    }
    if (status != UACPI_STATUS_OK) {
        acpi_log_failure(node, name, status);
        return false;
    }
    return true;
}

const struct acpi_entry_hdr *acpi_madt_next(const void *madt, size_t length, size_t *offset) {
    if (*offset > length || length - *offset < sizeof(struct acpi_entry_hdr)) {
        return NULL;
    }
    const struct acpi_entry_hdr *entry = (const void *)((const uint8_t *)madt + *offset);
    if (entry->length < sizeof(struct acpi_entry_hdr) || entry->length > length - *offset) {
        return NULL;
    }
    *offset += entry->length;
    return entry;
}

bool acpi_madt_processor(
    const struct acpi_entry_hdr *entry, uint32_t *apic_id, uint32_t *uid, uint32_t *flags
) {
    if (entry->type == ACPI_MADT_ENTRY_TYPE_LAPIC
     && entry->length >= sizeof(struct acpi_madt_lapic)) {
        struct acpi_madt_lapic lapic;
        memcpy(&lapic, entry, sizeof(struct acpi_madt_lapic));
        *apic_id = lapic.id;
        *uid = lapic.uid;
        *flags = lapic.flags;
        return true;
    }
    if (entry->type == ACPI_MADT_ENTRY_TYPE_LOCAL_X2APIC
     && entry->length >= sizeof(struct acpi_madt_x2apic)) {
        struct acpi_madt_x2apic x2apic;
        memcpy(&x2apic, entry, sizeof(struct acpi_madt_x2apic));
        *apic_id = x2apic.id;
        *uid = x2apic.uid;
        *flags = x2apic.flags;
        return true;
    }
    return false;
}

bool acpi_madt_lapic_nmi(
    const struct acpi_entry_hdr *entry, uint32_t *uid, bool *all, uint8_t *lint, uint16_t *flags
) {
    if (entry->type == ACPI_MADT_ENTRY_TYPE_LAPIC_NMI
     && entry->length >= sizeof(struct acpi_madt_lapic_nmi)) {
        struct acpi_madt_lapic_nmi nmi;
        memcpy(&nmi, entry, sizeof(struct acpi_madt_lapic_nmi));
        *uid = nmi.uid;
        *all = nmi.uid == 0xff;
        *lint = nmi.lint;
        *flags = nmi.flags;
        return true;
    }
    if (entry->type == ACPI_MADT_ENTRY_TYPE_LOCAL_X2APIC_NMI
     && entry->length >= sizeof(struct acpi_madt_x2apic_nmi)) {
        struct acpi_madt_x2apic_nmi nmi;
        memcpy(&nmi, entry, sizeof(struct acpi_madt_x2apic_nmi));
        *uid = nmi.uid;
        *all = nmi.uid == 0xffffffff;
        *lint = nmi.lint;
        *flags = nmi.flags;
        return true;
    }
    return false;
}
