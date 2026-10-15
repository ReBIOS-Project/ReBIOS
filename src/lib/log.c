// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of ReBIOS.
// Copyright (C) 2026 Mintsuki and contributors
//
// ReBIOS is free software: you can redistribute it and/or modify it under the
// terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version.
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
#include <stdarg.h>
#include <efi.h>
#include <hw/serial.h>
#include <lib/log.h>
#include <lib/printf.h>
#include <loader/efi_glue.h>
#include <sys/x86.h>

static uint16_t log_serial_port;

void log_init(uint16_t serial_port, uint32_t serial_baud) {
    if (serial_port != 0 && serial_init(serial_port, serial_baud)) {
        log_serial_port = serial_port;
    }
}

struct con_buffer {
    EFI_CHAR16 chars[128];
    size_t count;
};

static void con_flush(struct con_buffer *buf) {
    if (buf->count == 0) {
        return;
    }
    buf->chars[buf->count] = 0;
    if (!efi_boot_services_exited) {
        efi_st->ConOut->OutputString(efi_st->ConOut, buf->chars);
    }
    buf->count = 0;
}

static void con_put(struct con_buffer *buf, EFI_CHAR16 c) {
    if (buf->count == sizeof(buf->chars) / sizeof(buf->chars[0]) - 1) {
        con_flush(buf);
    }
    buf->chars[buf->count++] = c;
}

static void log_putc(int c, void *ctx) {
    struct con_buffer *buf = ctx;
    if (c == '\n') {
        con_put(buf, u'\r');
        if (log_serial_port != 0) {
            serial_putc(log_serial_port, '\r');
        }
    }
    con_put(buf, (EFI_CHAR16)(unsigned char)c);
    if (log_serial_port != 0) {
        serial_putc(log_serial_port, (char)c);
    }
    // Firmware may mirror its console on the same serial port, so the console
    // gets whole lines, which keeps the two copies from interleaving.
    if (c == '\n') {
        con_flush(buf);
    }
}

void vprintf(const char *fmt, va_list args) {
    struct con_buffer buf;
    buf.count = 0;
    format(log_putc, &buf, fmt, args);
    con_flush(&buf);
}

void printf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
}

void panic(const char *fmt, ...) {
    va_list args;
    printf("PANIC: ");
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    halt_forever();
}
