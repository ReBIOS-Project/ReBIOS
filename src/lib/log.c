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
// You should have received a copy of the GNU General Public License along with
// ReBIOS. If not, see <https://www.gnu.org/licenses/>.

#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <efi.h>
#include <flanterm.h>
#include <flanterm_backends/fb.h>
#include <lib/log.h>
#include <handoff.h>
#include <hw/serial.h>
#include <lib/printf.h>
#include <lib/reed_solomon.h>
#include <lib/spinlock.h>
#include <lib/util.h>
#include <loader/efi_glue.h>
#include <memory.h>
#include <sys/cache.h>
#include <sys/x86.h>

static uint16_t log_serial_port;
static bool log_console;
static bool log_started;
static struct flanterm_context *log_screen;
static _Atomic(bool) log_screen_handed_over;
static _Atomic(bool) log_panicking;

// The log's latest bytes after its first LOG_SAVED_START, for
// log_saved_text().
#define SAVED_TAIL 32768

static char saved_head[LOG_SAVED_START];
static size_t saved_head_len;
static char saved_tail[SAVED_TAIL];
static size_t saved_tail_count;
static _Atomic(log_keeper) keeper;

// The log's start, until log_keep_in() puts it in the area it is kept in.
#define START_TEXT 16384

static struct spinlock log_lock;
static char early_log[8192];
static size_t early_log_len;
static bool early_log_lost;
static char start_text[START_TEXT];
static size_t start_text_len;
static struct handoff_log *kept;
// How many bytes the log has had, and how many of them memory has.
static uint64_t kept_count;
static uint64_t kept_flushed;

struct con_buffer {
    uint16_t chars[128];
    size_t count;
};

static void con_flush(struct con_buffer *buf) {
    if (buf->count == 0) {
        return;
    }
    buf->chars[buf->count] = 0;
    if (!efi_glue_boot_services_exited) {
        efi_st->con_out->output_string(efi_st->con_out, buf->chars);
    }
    buf->count = 0;
}

static void con_put(struct con_buffer *buf, uint16_t c) {
    if (buf->count == ARRAY_COUNT(buf->chars) - 1) {
        con_flush(buf);
    }
    buf->chars[buf->count++] = c;
}

static bool screen_shown(void) {
    return log_console
        && (!atomic_load_explicit(&log_screen_handed_over, memory_order_relaxed)
         || atomic_load_explicit(&log_panicking, memory_order_relaxed));
}

static void screen_put(struct con_buffer *buf, char c) {
    if (log_screen != NULL) {
        flanterm_write(log_screen, &c, 1);
    } else {
        con_put(buf, (uint16_t)(unsigned char)c);
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

// Where the log's byte at position at goes in the area it is kept in.
static char *kept_byte(uint64_t at) {
    char *text = (char *)kept + HANDOFF_KEPT_DATA;
    if (at < HANDOFF_LOG_HEAD_SIZE) {
        return text + at;
    }
    return text + HANDOFF_LOG_HEAD_SIZE + (at - HANDOFF_LOG_HEAD_SIZE) % HANDOFF_LOG_TAIL_SIZE;
}

// The header's parity, after any change to it.
static void kept_seal(void) {
    reed_solomon_encode((const uint8_t *)kept, offsetof(struct handoff_log, parity), kept->parity);
    cache_write_back(kept, sizeof(struct handoff_log));
}

// The bytes since the last flush go to memory with the parity of their blocks,
// then their count, so that the count is true of memory whenever a reset
// comes.
static void kept_flush(void) {
    uint8_t *text = (uint8_t *)kept + HANDOFF_KEPT_DATA;
    uint8_t *parity = (uint8_t *)kept + HANDOFF_KEPT_PARITY;
    uint64_t at = kept_flushed;
    while (at < kept_count) {
        // A run ends at the end of the head, or of the ring.
        uint64_t end;
        if (at < HANDOFF_LOG_HEAD_SIZE) {
            end = HANDOFF_LOG_HEAD_SIZE;
        } else {
            uint64_t in_ring = (at - HANDOFF_LOG_HEAD_SIZE) % HANDOFF_LOG_TAIL_SIZE;
            end = at + HANDOFF_LOG_TAIL_SIZE - in_ring;
        }
        end = end < kept_count ? end : kept_count;
        size_t from = (size_t)((uint8_t *)kept_byte(at) - text);
        size_t to = from + (size_t)(end - at);
        reed_solomon_encode_blocks(text, parity, from, to);
        cache_write_back(text + from, to - from);
        size_t first = from / REED_SOLOMON_BLOCK;
        size_t blocks = (to - 1) / REED_SOLOMON_BLOCK - first + 1;
        cache_write_back(parity + first * REED_SOLOMON_PARITY, blocks * REED_SOLOMON_PARITY);
        at = end;
    }
    *(volatile uint64_t *)&kept->count = kept_count;
    kept_seal();
    kept_flushed = kept_count;
}

static void keep(char c) {
    if (kept == NULL) {
        if (start_text_len < START_TEXT) {
            start_text[start_text_len++] = c;
        }
        return;
    }
    *kept_byte(kept_count++) = c;
    if (c == '\n') {
        kept_flush();
    }
}

static void log_emit(struct con_buffer *buf, char c) {
    if (saved_head_len < LOG_SAVED_START) {
        saved_head[saved_head_len++] = c;
    } else {
        saved_tail[saved_tail_count++ % SAVED_TAIL] = c;
    }
    keep(c);
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
    printf_format(log_putc, &buf, fmt, args);
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

void log_init(uint16_t serial_port, uint32_t serial_baud, uint32_t serial_clock, bool console) {
    if (serial_port != 0 && serial_init(serial_port, serial_baud, serial_clock)) {
        log_serial_port = serial_port;
    }
    log_console = console;
    log_start();
}

void log_set_screen(struct flanterm_context *screen) {
    log_screen = screen;
}

void log_move_screen(void *framebuffer) {
    spinlock_acquire(&log_lock);
    if (log_screen != NULL) {
        flanterm_fb_set_framebuffer(log_screen, framebuffer);
    }
    spinlock_release(&log_lock);
}

void log_hand_over_screen(void) {
    atomic_store_explicit(&log_screen_handed_over, true, memory_order_release);
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
    atomic_store_explicit(&log_panicking, true, memory_order_release);
    log_console = true;
    if (!log_started) {
        log_start();
    }
    if (log_screen != NULL
     && atomic_load_explicit(&log_screen_handed_over, memory_order_relaxed)) {
        flanterm_full_refresh(log_screen);
    }
    // Without the lock, which the panicking processor may hold.
    va_list args;
    printf_unlocked("PANIC: ");
    va_start(args, fmt);
    vprintf_unlocked(fmt, args);
    va_end(args);
    log_keeper keep_log = atomic_exchange_explicit(&keeper, NULL, memory_order_acq_rel);
    if (keep_log != NULL) {
        keep_log(false);
    }
    x86_halt_forever();
}

bool log_panicked(void) {
    return atomic_load_explicit(&log_panicking, memory_order_acquire);
}

bool log_take_screen(void) {
    if (!log_console || log_screen == NULL) {
        return false;
    }
    spinlock_acquire(&log_lock);
    atomic_store_explicit(&log_panicking, true, memory_order_release);
    if (log_screen != NULL
     && atomic_load_explicit(&log_screen_handed_over, memory_order_relaxed)) {
        flanterm_full_refresh(log_screen);
    }
    spinlock_release(&log_lock);
    return true;
}

void log_keep_in(struct handoff_log *area) {
    static const char cut[] = "[...]\n";
    spinlock_acquire(&log_lock);
    kept = area;
    kept_count = 0;
    kept_flushed = 0;
    for (size_t i = 0; i < start_text_len; i++) {
        *kept_byte(kept_count++) = start_text[i];
    }
    if (start_text_len == START_TEXT) {
        for (size_t i = 0; i < sizeof(cut) - 1; i++) {
            *kept_byte(kept_count++) = cut[i];
        }
    }
    kept_flush();
    memcpy(area->magic, HANDOFF_LOG_MAGIC, sizeof(area->magic));
    kept_seal();
    spinlock_release(&log_lock);
}

size_t log_saved_text(size_t since, char *out, size_t size, size_t *first, bool lock) {
    if (lock) {
        spinlock_acquire(&log_lock);
    }
    size_t end = saved_head_len + saved_tail_count;
    // The head stays whole, while the tail keeps only its latest SAVED_TAIL
    // bytes, those from kept on.
    size_t kept_from = LOG_SAVED_START;
    if (saved_tail_count > SAVED_TAIL) {
        kept_from = LOG_SAVED_START + saved_tail_count - SAVED_TAIL;
    }
    size_t start = since;
    size_t stop = end;
    if (start >= LOG_SAVED_START && start < kept_from) {
        start = kept_from;
    } else if (start < LOG_SAVED_START && kept_from > LOG_SAVED_START) {
        stop = LOG_SAVED_START;
    }
    size_t len = stop - start < size ? stop - start : size;
    for (size_t p = start; p < start + len; p++) {
        out[p - start] = p < LOG_SAVED_START
                       ? saved_head[p]
                       : saved_tail[(p - LOG_SAVED_START) % SAVED_TAIL];
    }
    if (lock) {
        spinlock_release(&log_lock);
    }
    *first = start;
    return len;
}

size_t log_saved_count(void) {
    return saved_head_len + saved_tail_count;
}

void log_set_keeper(log_keeper keep_log) {
    atomic_store_explicit(&keeper, keep_log, memory_order_release);
}

void log_keep(void) {
    log_keeper keep_log = atomic_load_explicit(&keeper, memory_order_acquire);
    if (keep_log != NULL) {
        keep_log(true);
    }
}
