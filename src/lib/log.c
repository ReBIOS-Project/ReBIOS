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

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <efi.h>
#include <flanterm.h>
#include <lib/log.h>
#include <hw/serial.h>
#include <lib/printf.h>
#include <lib/spinlock.h>
#include <loader/efi_glue.h>
#include <sys/x86.h>

static uint16_t log_serial_port;
static bool log_console;
static bool log_started;
static struct flanterm_context *log_screen;
static bool log_screen_handed_over;
static bool log_panicking;

static struct spinlock log_lock;
static char early_log[8192];
static size_t early_log_len;
static bool early_log_lost;

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
    if (buf->count == sizeof(buf->chars) / sizeof(EFI_CHAR16) - 1) {
        con_flush(buf);
    }
    buf->chars[buf->count++] = c;
}

static bool screen_shown(void) {
    return log_console && (!log_screen_handed_over || log_panicking);
}

static void screen_put(struct con_buffer *buf, char c) {
    if (log_screen != NULL) {
        flanterm_write(log_screen, &c, 1);
    } else {
        con_put(buf, (EFI_CHAR16)(unsigned char)c);
    }
}

// Flanterm's first flush paints the whole framebuffer, so there is none until
// something is shown.
static void screen_flush(struct con_buffer *buf) {
    if (log_screen != NULL) {
        if (screen_shown()) {
            flanterm_flush(log_screen);
        }
    } else {
        con_flush(buf);
    }
}

static void log_emit(struct con_buffer *buf, char c) {
    bool screen = screen_shown();
    if (c == '\n') {
        if (screen) {
            screen_put(buf, '\r');
        }
        if (log_serial_port != 0) {
            serial_putc(log_serial_port, '\r');
        }
    }
    if (screen) {
        screen_put(buf, c);
    }
    if (log_serial_port != 0) {
        serial_putc(log_serial_port, c);
    }
    // Firmware may mirror its console on the same serial port, so the console
    // gets whole lines, which keeps the two copies from interleaving.
    if (c == '\n') {
        screen_flush(buf);
    }
}

static void log_putc(int c, void *ctx) {
    if (!log_started) {
        if (early_log_len < sizeof(early_log)) {
            early_log[early_log_len++] = (char)c;
        } else {
            early_log_lost = true;
        }
        return;
    }
    log_emit(ctx, (char)c);
}

static void vprintf_unlocked(const char *fmt, va_list args) {
    struct con_buffer buf;
    buf.count = 0;
    format(log_putc, &buf, fmt, args);
    screen_flush(&buf);
}

static void printf_unlocked(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vprintf_unlocked(fmt, args);
    va_end(args);
}

static void log_start(void) {
    log_started = true;
    struct con_buffer buf;
    buf.count = 0;
    for (size_t i = 0; i < early_log_len; i++) {
        log_emit(&buf, early_log[i]);
    }
    screen_flush(&buf);
    if (early_log_lost) {
        printf_unlocked("(log truncated)\n");
    }
}

void log_init(uint16_t serial_port, uint32_t serial_baud, bool console) {
    if (serial_port != 0 && serial_init(serial_port, serial_baud)) {
        log_serial_port = serial_port;
    }
    log_console = console;
    log_start();
}

void log_set_screen(struct flanterm_context *screen) {
    log_screen = screen;
}

void log_hand_over_screen(void) {
    __atomic_store_n(&log_screen_handed_over, true, __ATOMIC_RELEASE);
}

void vprintf(const char *fmt, va_list args) {
    spinlock_acquire(&log_lock);
    vprintf_unlocked(fmt, args);
    spinlock_release(&log_lock);
}

void printf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
}

void log_notice(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    spinlock_acquire(&log_lock);
    bool console = log_console;
    log_console = true;
    vprintf_unlocked(fmt, args);
    log_console = console;
    spinlock_release(&log_lock);
    va_end(args);
}

void panic(const char *fmt, ...) {
    // Whatever the configuration, a fatal error is shown, over anything the
    // guest drew.
    __atomic_store_n(&log_panicking, true, __ATOMIC_RELEASE);
    log_console = true;
    if (!log_started) {
        log_start();
    }
    if (log_screen != NULL && log_screen_handed_over) {
        flanterm_full_refresh(log_screen);
    }
    // Without the lock, which the panicking processor may hold.
    va_list args;
    printf_unlocked("PANIC: ");
    va_start(args, fmt);
    vprintf_unlocked(fmt, args);
    va_end(args);
    halt_forever();
}

bool log_panicked(void) {
    return __atomic_load_n(&log_panicking, __ATOMIC_ACQUIRE);
}

bool log_take_screen(void) {
    if (!log_console || log_screen == NULL) {
        return false;
    }
    spinlock_acquire(&log_lock);
    __atomic_store_n(&log_panicking, true, __ATOMIC_RELEASE);
    if (log_screen != NULL && log_screen_handed_over) {
        flanterm_full_refresh(log_screen);
    }
    spinlock_release(&log_lock);
    return true;
}
