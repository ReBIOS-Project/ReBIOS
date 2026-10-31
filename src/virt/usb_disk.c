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
#include <tendril/msc.h>
#include <tendril/tendril.h>
#include <virt/usb_disk.h>
#include <hw/pci_enum.h>
#include <lib/printf.h>
#include <lib/spinlock.h>
#include <memory.h>
#include <virt/guest_mem.h>
#include <virt/hypercall.h>
#include <virt/usb.h>
#include <virt/usb_controller.h>

// USB's disks for the legacy BIOS, whose driver asks for a transfer and then
// looks until it is done, going about its business meanwhile.

#define MAX_DISKS 16
#define MAX_HUBS 7

// As much as the legacy BIOS's disk calls transfer at once.
#define BUFFER_SIZE ((size_t)64 << 10)

#define SCSI_TYPE_CDROM 0x05

// What a disk is doing, of the only transfer it has at once.
#define DISK_IDLE 0
#define DISK_QUEUED 1
#define DISK_RUNNING 2
#define DISK_DONE 3

struct usb_disk {
    struct tendril_msc_lun *lun;
    uint8_t *buffer;
    int state;
    int status;
    bool read;
    uint32_t guest_buffer;
    size_t bytes;
    struct tendril_msc_request request;
};

static struct usb_disk disks[MAX_DISKS];
static struct spinlock lock;

static void msc_attach(struct tendril_msc_lun *lun) {
    spinlock_acquire(&lock);
    for (size_t i = 0; i < MAX_DISKS; i++) {
        struct usb_disk *d = &disks[i];
        if (d->lun == NULL) {
            uintptr_t bus;
            d->buffer = usb_dma_alloc(BUFFER_SIZE, 0, &bus);
            if (d->buffer != NULL) {
                d->lun = lun;
                d->state = DISK_IDLE;
                tendril_msc_set_user(lun, d);
            }
            break;
        }
    }
    spinlock_release(&lock);
}

static void msc_detach(struct tendril_msc_lun *lun) {
    struct usb_disk *d = tendril_msc_user(lun);
    if (d == NULL) {
        return;
    }
    spinlock_acquire(&lock);
    usb_dma_free(d->buffer, BUFFER_SIZE);
    d->lun = NULL;
    spinlock_release(&lock);
}

static const struct tendril_msc_callbacks msc_callbacks = {
    .attach = msc_attach, .detach = msc_detach
};

void usb_disk_register(void) {
    tendril_msc_register(&msc_callbacks);
}

static void request_done(struct tendril_msc_request *request) {
    struct usb_disk *d = request->context;
    spinlock_acquire(&lock);
    d->status = request->status;
    d->state = DISK_DONE;
    spinlock_release(&lock);
}

void usb_disk_work(void) {
    for (size_t i = 0; i < MAX_DISKS; i++) {
        struct usb_disk *d = &disks[i];
        spinlock_acquire(&lock);
        bool queued = d->lun != NULL && d->state == DISK_QUEUED;
        if (queued) {
            d->state = DISK_RUNNING;
        }
        spinlock_release(&lock);
        if (queued) {
            int status = tendril_msc_submit(d->lun, &d->request);
            if (status != TENDRIL_OK) {
                d->request.status = status;
                request_done(&d->request);
            }
        }
    }
}

static uint32_t status_of(int status) {
    switch (status) {
        case TENDRIL_OK: {
            return HYPERCALL_USB_DISK_STATUS_OK;
        }
        case TENDRIL_ERR_NO_MEDIUM:
        case TENDRIL_ERR_MEDIUM_CHANGED: {
            return HYPERCALL_USB_DISK_STATUS_NO_MEDIUM;
        }
        case TENDRIL_ERR_WRITE_PROTECTED: {
            return HYPERCALL_USB_DISK_STATUS_WRITE_PROTECTED;
        }
        default: {
            return HYPERCALL_USB_DISK_STATUS_FAILED;
        }
    }
}

// The path below the controller, as loader/drives.c writes it.
static void disk_path(struct tendril_msc_lun *lun, char *out, size_t size) {
    struct tendril_device *dev = tendril_msc_device(lun);
    uint8_t ports[MAX_HUBS];
    size_t count = 0;
    for (struct tendril_device *up = tendril_device_parent(dev);
         up != NULL && count < MAX_HUBS; up = tendril_device_parent(up)) {
        ports[count++] = tendril_device_port(up);
    }
    size_t used = 0;
    for (size_t i = count; i-- > 0; ) {
        used += (size_t)snprintf(out + used, size - used, "/hub@%x", ports[i]);
    }
    snprintf(
        out + used, size - used, "/storage@%x/*@0/*@0,%x",
        tendril_device_port(dev), tendril_msc_lun_number(lun)
    );
}

static uint32_t disk_info(const struct usb_disk *d, uint32_t gpa) {
    struct tendril_msc_lun *lun = d->lun;
    struct hypercall_usb_disk_info info = { 0 };
    const struct usb_controller *c = usb_controller_of(
        tendril_device_hc(tendril_msc_device(lun))
    );
    if (c == NULL) {
        return HYPERCALL_USB_DISK_STATUS_NO_DISK;
    }
    info.blocks = tendril_msc_blocks(lun);
    info.block_size = (uint32_t)tendril_msc_block_size(lun);
    if (tendril_msc_type(lun) == SCSI_TYPE_CDROM) {
        info.flags |= HYPERCALL_USB_DISK_CD;
    }
    if (tendril_msc_removable(lun)) {
        info.flags |= HYPERCALL_USB_DISK_REMOVABLE;
    }
    info.bdf = (uint32_t)c->f->addr.bus << 8
        | (uint32_t)c->f->addr.device << 3 | c->f->addr.function;
    disk_path(lun, info.path, sizeof(info.path));
    snprintf(
        info.description, sizeof(info.description), "USB %s %s",
        tendril_msc_vendor(lun), tendril_msc_product(lun)
    );
    bool written = guest_phys_write(
        gpa, &info, sizeof(struct hypercall_usb_disk_info)
    );
    return written ? HYPERCALL_USB_DISK_STATUS_OK
        : HYPERCALL_USB_DISK_STATUS_FAILED;
}

static uint32_t disk_start(
    struct usb_disk *d, const struct hypercall_usb_disk *call, bool read
) {
    size_t block_size = tendril_msc_block_size(d->lun);
    size_t bytes = call->count * block_size;
    if (d->state != DISK_IDLE || call->count == 0 || bytes > BUFFER_SIZE) {
        return HYPERCALL_USB_DISK_STATUS_FAILED;
    }
    if (!read && !guest_phys_read(call->buffer, d->buffer, bytes)) {
        return HYPERCALL_USB_DISK_STATUS_FAILED;
    }
    d->read = read;
    d->guest_buffer = call->buffer;
    d->bytes = bytes;
    d->request = (struct tendril_msc_request){
        .op = read ? TENDRIL_MSC_READ : TENDRIL_MSC_WRITE, .lba = call->lba,
        .count = call->count, .buffer = d->buffer, .done = request_done,
        .context = d
    };
    d->state = DISK_QUEUED;
    return HYPERCALL_USB_DISK_STATUS_PENDING;
}

static uint32_t disk_poll(struct usb_disk *d) {
    if (d->state == DISK_IDLE) {
        return HYPERCALL_USB_DISK_STATUS_FAILED;
    }
    if (d->state != DISK_DONE) {
        return HYPERCALL_USB_DISK_STATUS_PENDING;
    }
    d->state = DISK_IDLE;
    uint32_t status = status_of(d->status);
    if (status == HYPERCALL_USB_DISK_STATUS_OK && d->read
     && !guest_phys_write(d->guest_buffer, d->buffer, d->bytes)) {
        return HYPERCALL_USB_DISK_STATUS_FAILED;
    }
    return status;
}

void usb_disk_call(uint32_t gpa) {
    struct hypercall_usb_disk call;
    if (!guest_phys_read(gpa, &call, sizeof(struct hypercall_usb_disk))) {
        return;
    }
    if (call.function == HYPERCALL_USB_DISK_READY) {
        call.status = usb_ready() ? 1 : 0;
        guest_phys_write(gpa, &call, sizeof(struct hypercall_usb_disk));
        return;
    }
    spinlock_acquire(&lock);
    struct usb_disk *d = call.disk < MAX_DISKS ? &disks[call.disk] : NULL;
    if (d == NULL || d->lun == NULL) {
        call.status = HYPERCALL_USB_DISK_STATUS_NO_DISK;
    } else {
        switch (call.function) {
            case HYPERCALL_USB_DISK_INFO: {
                call.status = disk_info(d, call.buffer);
                break;
            }
            case HYPERCALL_USB_DISK_READ: {
                call.status = disk_start(d, &call, true);
                break;
            }
            case HYPERCALL_USB_DISK_WRITE: {
                call.status = disk_start(d, &call, false);
                break;
            }
            case HYPERCALL_USB_DISK_POLL: {
                call.status = disk_poll(d);
                break;
            }
            default: {
                call.status = HYPERCALL_USB_DISK_STATUS_FAILED;
                break;
            }
        }
    }
    spinlock_release(&lock);
    guest_phys_write(gpa, &call, sizeof(struct hypercall_usb_disk));
    // The transfer's next step, whichever that is, comes sooner.
    usb_kick();
}
