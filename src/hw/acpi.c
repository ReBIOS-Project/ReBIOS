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
#include <uacpi/acpi.h>
#include <uacpi/event.h>
#include <uacpi/io.h>
#include <uacpi/kernel_api.h>
#include <uacpi/log.h>
#include <uacpi/platform/arch_helpers.h>
#include <uacpi/platform/types.h>
#include <uacpi/status.h>
#include <uacpi/tables.h>
#include <uacpi/types.h>
#include <uacpi/uacpi.h>
#include <uacpi/utilities.h>
#include <hw/acpi.h>
#include <core.h>
#include <handoff.h>
#include <hw/ec.h>
#include <hw/pci_cfg.h>
#include <lib/heap.h>
#include <lib/log.h>
#include <lib/spinlock.h>
#include <lib/time.h>
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
    uint64_t units;
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
static bool aml_started;
static bool aml_wanted;
static bool aml_running;
static bool aml_bsp;

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

uacpi_status uacpi_kernel_pci_device_open(
    uacpi_pci_address address, uacpi_handle *out_handle
) {
    struct pci_addr *addr = kmalloc(sizeof(struct pci_addr));
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
    if (offset >= 4096 || offset % size != 0) {
        return UACPI_STATUS_INVALID_ARGUMENT;
    }
    const struct pci_addr *addr = device;
    *value = pci_cfg_read(*addr, (uint32_t)offset, size);
    return UACPI_STATUS_OK;
}

static uacpi_status pci_write(
    uacpi_handle device, uacpi_size offset, size_t size, uint32_t value
) {
    if (offset >= 4096 || offset % size != 0) {
        return UACPI_STATUS_INVALID_ARGUMENT;
    }
    const struct pci_addr *addr = device;
    pci_cfg_write(*addr, (uint32_t)offset, size, value);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_pci_read8(
    uacpi_handle device, uacpi_size offset, uacpi_u8 *value
) {
    uint32_t v = 0;
    uacpi_status status = pci_read(device, offset, 1, &v);
    *value = (uacpi_u8)v;
    return status;
}

uacpi_status uacpi_kernel_pci_read16(
    uacpi_handle device, uacpi_size offset, uacpi_u16 *value
) {
    uint32_t v = 0;
    uacpi_status status = pci_read(device, offset, 2, &v);
    *value = (uacpi_u16)v;
    return status;
}

uacpi_status uacpi_kernel_pci_read32(
    uacpi_handle device, uacpi_size offset, uacpi_u32 *value
) {
    uint32_t v = 0;
    uacpi_status status = pci_read(device, offset, 4, &v);
    *value = v;
    return status;
}

uacpi_status uacpi_kernel_pci_write8(
    uacpi_handle device, uacpi_size offset, uacpi_u8 value
) {
    return pci_write(device, offset, 1, value);
}

uacpi_status uacpi_kernel_pci_write16(
    uacpi_handle device, uacpi_size offset, uacpi_u16 value
) {
    return pci_write(device, offset, 2, value);
}

uacpi_status uacpi_kernel_pci_write32(
    uacpi_handle device, uacpi_size offset, uacpi_u32 value
) {
    return pci_write(device, offset, 4, value);
}

uacpi_status uacpi_kernel_io_map(
    uacpi_io_addr base, uacpi_size len, uacpi_handle *out_handle
) {
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

static bool io_port(
    uacpi_handle handle, uacpi_size offset, size_t size, uint16_t *port
) {
    const struct io_range *range = handle;
    if (offset > range->length || range->length - offset < size) {
        return false;
    }
    *port = (uint16_t)(range->base + offset);
    return true;
}

uacpi_status uacpi_kernel_io_read8(
    uacpi_handle handle, uacpi_size offset, uacpi_u8 *out_value
) {
    uint16_t port;
    if (!io_port(handle, offset, 1, &port)) {
        return UACPI_STATUS_INVALID_ARGUMENT;
    }
    *out_value = inb(port);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_read16(
    uacpi_handle handle, uacpi_size offset, uacpi_u16 *out_value
) {
    uint16_t port;
    if (!io_port(handle, offset, 2, &port)) {
        return UACPI_STATUS_INVALID_ARGUMENT;
    }
    *out_value = inw(port);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_read32(
    uacpi_handle handle, uacpi_size offset, uacpi_u32 *out_value
) {
    uint16_t port;
    if (!io_port(handle, offset, 4, &port)) {
        return UACPI_STATUS_INVALID_ARGUMENT;
    }
    *out_value = inl(port);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_write8(
    uacpi_handle handle, uacpi_size offset, uacpi_u8 in_value
) {
    uint16_t port;
    if (!io_port(handle, offset, 1, &port)) {
        return UACPI_STATUS_INVALID_ARGUMENT;
    }
    outb(port, in_value);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_write16(
    uacpi_handle handle, uacpi_size offset, uacpi_u16 in_value
) {
    uint16_t port;
    if (!io_port(handle, offset, 2, &port)) {
        return UACPI_STATUS_INVALID_ARGUMENT;
    }
    outw(port, in_value);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_write32(
    uacpi_handle handle, uacpi_size offset, uacpi_u32 in_value
) {
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
    return timeout_ms != WAIT_FOREVER
        && time_ns() - start_ns >= (uint64_t)timeout_ms * 1000000;
}

uacpi_handle uacpi_kernel_create_semaphore(uacpi_u32 initial_units) {
    struct semaphore *semaphore = kmalloc(sizeof(struct semaphore));
    semaphore->units = initial_units;
    semaphore->event = initial_units == 0;
    return semaphore;
}

void uacpi_kernel_free_semaphore(uacpi_handle handle) {
    kfree(handle);
}

uacpi_status uacpi_kernel_wait_for_semaphore(
    uacpi_handle handle, uacpi_u16 timeout
) {
    struct semaphore *semaphore = handle;
    if (timeout == WAIT_FOREVER && semaphore->event) {
        timeout = FOREVER_POLL_MS;
    }
    uintptr_t start = time_ns();
    for (;;) {
        uint64_t units = __atomic_load_n(&semaphore->units, __ATOMIC_RELAXED);
        while (units != 0) {
            bool taken = __atomic_compare_exchange_n(
                &semaphore->units, &units, units - 1, false, __ATOMIC_ACQUIRE,
                __ATOMIC_RELAXED
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
    __atomic_fetch_add(&semaphore->units, 1, __ATOMIC_RELEASE);
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

uacpi_status uacpi_kernel_handle_firmware_request(
    uacpi_firmware_request *request
) {
    switch (request->type) {
        case UACPI_FIRMWARE_REQUEST_TYPE_BREAKPOINT: {
            printf("AML breakpoint\n");
            break;
        }
        case UACPI_FIRMWARE_REQUEST_TYPE_FATAL: {
            printf(
                "AML fatal error, type %#x, code %#x, argument %#llx\n",
                request->fatal.type, request->fatal.code,
                (unsigned long long)request->fatal.arg
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
    uacpi_u32 irq, uacpi_interrupt_handler handler, uacpi_handle ctx,
    uacpi_handle *out_irq_handle
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
    uacpi_work_type type, uacpi_handle work_item, uacpi_work_handler handler,
    uacpi_handle ctx
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
    __atomic_store_n(&aml_wanted, true, __ATOMIC_SEQ_CST);
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
    bool outer = __atomic_load_n(&aml_bsp, __ATOMIC_RELAXED);
    struct work w;
    while (work_take(&w)) {
        if (in_task) {
            __atomic_store_n(&aml_bsp, outer || w.bsp, __ATOMIC_RELEASE);
            while (w.bsp && !this_cpu()->bsp) {
                task_yield();
            }
        }
        w.handler(w.ctx);
        if (in_task) {
            __atomic_store_n(&aml_bsp, outer, __ATOMIC_RELEASE);
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
    if (!__atomic_load_n(&aml_started, __ATOMIC_ACQUIRE)) {
        work_run();
    }
    spinlock_release(&lock);
}

static void aml_main(void *arg) {
    (void)arg;
    for (;;) {
        work_run();
        // Work that comes meanwhile makes aml_wanted true again.
        __atomic_store_n(&aml_wanted, false, __ATOMIC_SEQ_CST);
        spinlock_acquire(&work_lock);
        bool more = work_head != NULL;
        spinlock_release(&work_lock);
        if (more) {
            __atomic_store_n(&aml_wanted, true, __ATOMIC_SEQ_CST);
        } else {
            task_yield();
        }
    }
}

void acpi_slice(void) {
    if (!acpi_working()
     || (__atomic_load_n(&aml_bsp, __ATOMIC_ACQUIRE) && !this_cpu()->bsp)
     || __atomic_exchange_n(&aml_running, true, __ATOMIC_ACQUIRE)) {
        return;
    }
    task_run(&aml_task);
    __atomic_store_n(&aml_running, false, __ATOMIC_RELEASE);
}

bool acpi_working(void) {
    return __atomic_load_n(&aml_started, __ATOMIC_ACQUIRE)
        && __atomic_load_n(&aml_wanted, __ATOMIC_ACQUIRE);
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
        printf(
            "Cannot initialise the ACPI namespace: %s\n",
            uacpi_status_to_string(status)
        );
    }
    // Again, should _INI have undone it.
    status = uacpi_set_interrupt_model(UACPI_INTERRUPT_MODEL_IOAPIC);
    if (status != UACPI_STATUS_OK) {
        printf(
            "Cannot put the firmware in APIC mode: %s\n",
            uacpi_status_to_string(status)
        );
    }
    status = uacpi_finalize_gpe_initialization();
    if (status != UACPI_STATUS_OK) {
        printf(
            "Cannot enable the ACPI GPEs: %s\n", uacpi_status_to_string(status)
        );
    }
    acpi_unlock();
    task_init(&aml_task, aml_main, NULL);
    __atomic_store_n(&aml_started, true, __ATOMIC_RELEASE);
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
    uacpi_status status = uacpi_setup_early_table_access(
        buffer, EARLY_TABLE_BUFFER_SIZE
    );
    if (status != UACPI_STATUS_OK) {
        printf(
            "Cannot access the ACPI tables: %s\n",
            uacpi_status_to_string(status)
        );
        kfree(buffer);
        return false;
    }
    tables_ok = true;

    status = uacpi_initialize(0);
    if (status != UACPI_STATUS_OK) {
        printf(
            "Cannot initialise uACPI: %s\n", uacpi_status_to_string(status)
        );
    } else {
        status = uacpi_namespace_load();
        if (status != UACPI_STATUS_OK) {
            printf(
                "Cannot load the ACPI namespace: %s\n",
                uacpi_status_to_string(status)
            );
        }
    }

    if (status != UACPI_STATUS_OK) {
        // uACPI drops all of its state on failure, its tables included.
        status = uacpi_setup_early_table_access(
            buffer, EARLY_TABLE_BUFFER_SIZE
        );
        if (status != UACPI_STATUS_OK) {
            printf(
                "Cannot access the ACPI tables again: %s\n",
                uacpi_status_to_string(status)
            );
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
