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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <efi.h>
#include <loader/last_log.h>
#include <handoff.h>
#include <lib/log.h>
#include <lib/printf.h>
#include <lib/reed_solomon.h>
#include <lib/util.h>
#include <loader/efi_glue.h>
#include <loader/files.h>
#include <loader/kept.h>
#include <memory.h>
#include <sys/cache.h>

#define FILE_NAME "rebios-last.log"
// The most logs that one search tells apart: a start without save_log leaves
// the last one with it where it was.
#define FOUND_MAX 8
// ReBIOS 0.7.0 to 0.10.0 kept the log's start in OLD_VARIABLE itself, and its
// latest text in OLD_END_VARIABLE, and 0.11.0 kept it in parts, from 0 on,
// named after OLD_VARIABLE and the part's number in 4 decimal digits.
#define OLD_PARTS 10000
#define OLD_PART_DIGITS 4
#define ATTRIBUTES \
    (EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS)

static uint16_t old_variable[] = u"ReBIOSLog";
static uint16_t old_end_variable[] = u"ReBIOSLogEnd";
static struct efi_guid guid = HANDOFF_VARIABLE_GUID;

struct found {
    uintptr_t page;
    struct efi_time started;
    uint64_t count;
};

static struct found found[FOUND_MAX];
static size_t found_count;

// Whether a start began after b's, by their clocks.
static bool later(const struct efi_time *a, const struct efi_time *b) {
    uint64_t date_a = (uint64_t)a->year << 40 | (uint64_t)a->month << 32 | (uint64_t)a->day << 24
                    | (uint64_t)a->hour << 16 | (uint64_t)a->minute << 8 | a->second;
    uint64_t date_b = (uint64_t)b->year << 40 | (uint64_t)b->month << 32 | (uint64_t)b->day << 24
                    | (uint64_t)b->hour << 16 | (uint64_t)b->minute << 8 | b->second;
    return date_a != date_b ? date_a > date_b : a->nanosecond > b->nanosecond;
}

// Notes a log kept at page, whole in free memory up to end.
static bool note(uintptr_t page, uint64_t end, void *ctx) {
    (void)ctx;
    struct handoff_log header;
    size_t parity_at = offsetof(struct handoff_log, parity);
    if (end - page >= HANDOFF_KEPT_SIZE && found_count < FOUND_MAX
     && kept_header(page, HANDOFF_LOG_MAGIC, &header, parity_at)) {
        found[found_count++] = (struct found){
            .page = page, .started = header.started, .count = header.count
        };
    }
    return false;
}

// Corrects the blocks of the log kept at page that hold its count bytes, and
// adds the number of bytes corrected to *corrected, and that of blocks past
// correcting to *failed.
static void correct(uintptr_t page, uint64_t count, size_t *corrected, size_t *failed) {
    uint8_t *text = (uint8_t *)page + HANDOFF_KEPT_DATA;
    uint8_t *parity = (uint8_t *)page + HANDOFF_KEPT_PARITY;
    uint64_t head = count < HANDOFF_LOG_HEAD_SIZE ? count : HANDOFF_LOG_HEAD_SIZE;
    uint64_t ring = count - head < HANDOFF_LOG_TAIL_SIZE ? count - head : HANDOFF_LOG_TAIL_SIZE;
    size_t head_blocks = (size_t)(head + REED_SOLOMON_BLOCK - 1) / REED_SOLOMON_BLOCK;
    size_t ring_blocks = (size_t)(ring + REED_SOLOMON_BLOCK - 1) / REED_SOLOMON_BLOCK;
    size_t ring_first = HANDOFF_LOG_HEAD_SIZE / REED_SOLOMON_BLOCK;
    reed_solomon_decode_blocks(text, parity, head_blocks, corrected, failed);
    reed_solomon_decode_blocks(
        text + HANDOFF_LOG_HEAD_SIZE, parity + ring_first * REED_SOLOMON_PARITY, ring_blocks,
        corrected, failed
    );
}

// The text of the log kept at page, of count bytes, in a pool allocation of
// *size bytes: its start, then the latest bytes of its ring, oldest first,
// with a mark where the bytes between them are lost, then end_len bytes of
// end.
static char *log_text(
    uintptr_t page,
    uint64_t count,
    const char *end,
    size_t end_len,
    size_t *size
) {
    static const char gap[] = "[...]\n";
    const char *text = (const char *)page + HANDOFF_KEPT_DATA;
    uint64_t head = count < HANDOFF_LOG_HEAD_SIZE ? count : HANDOFF_LOG_HEAD_SIZE;
    uint64_t ring = count - head;
    uint64_t kept = ring < HANDOFF_LOG_TAIL_SIZE ? ring : HANDOFF_LOG_TAIL_SIZE;
    size_t gap_len = ring > HANDOFF_LOG_TAIL_SIZE ? sizeof(gap) - 1 : 0;
    *size = (size_t)(head + gap_len + kept) + end_len;
    char *out = efi_glue_alloc_pool(*size != 0 ? *size : 1);
    if (out == NULL) {
        return NULL;
    }
    memcpy(out, text, head);
    memcpy(out + head, gap, gap_len);
    char *at = out + head + gap_len;
    uint64_t first = ring - kept;
    for (size_t i = 0; i < kept; i++) {
        at[i] = text[HANDOFF_LOG_HEAD_SIZE + (first + i) % HANDOFF_LOG_TAIL_SIZE];
    }
    // The file holds text alone, whatever memory held.
    for (size_t i = 0; i < *size - end_len; i++) {
        if ((out[i] < 0x20 || out[i] > 0x7e) && out[i] != '\n' && out[i] != '\t') {
            out[i] = '?';
        }
    }
    memcpy(out + *size - end_len, end, end_len);
    return out;
}

// Writes the log that f found out, with write, and marks it written either
// way.
static void take(const struct found *f, bool write) {
    size_t pages = page_count(HANDOFF_KEPT_SIZE);
    efi_physical_address taken = f->page;
    // The pages are the loader's while it reads them, so that writing the file
    // cannot take them.
    if (efi_bs->allocate_pages(EFI_ALLOCATE_ADDRESS, EFI_LOADER_DATA, pages, &taken)
     != EFI_SUCCESS) {
        return;
    }
    if (write) {
        size_t corrected = 0;
        size_t failed = 0;
        correct(f->page, f->count, &corrected, &failed);
        // What the restart did to the log ends it, as Linux 7.3's ramoops ends
        // its records (fs/pstore/ram_core.c).
        char end[160];
        size_t end_len = 0;
        if (corrected != 0 || failed != 0) {
            int written = snprintf(
                end, sizeof(end),
                "[Memory changed this log over the restart: %zu bytes corrected, %zu blocks of %d "
                "bytes past correcting]\n",
                corrected, failed, REED_SOLOMON_BLOCK
            );
            if (written > 0 && (size_t)written < sizeof(end)) {
                end_len = (size_t)written;
            }
        }
        size_t size;
        char *text = log_text(f->page, f->count, end, end_len, &size);
        if (text != NULL) {
            const struct efi_time *t = &f->started;
            char where[256] = FILE_NAME;
            efi_status status;
            const char *why = files_write_beside(
                FILE_NAME, text, size, where, sizeof(where), &status
            );
            if (why != NULL) {
                log_notice(
                    "Cannot write the log that ReBIOS kept last time to %s: %s, status %#llx\n",
                    where, why, (unsigned long long)status
                );
            } else {
                log_notice(
                    "Wrote the log that ReBIOS kept at its start of %04u-%02u-%02u "
                    "%02u:%02u:%02u to %s\n",
                    t->year, t->month, t->day, t->hour, t->minute, t->second, where
                );
            }
            efi_bs->free_pool(text);
        } else {
            log_notice("No memory to write the log that ReBIOS kept last time to %s\n", FILE_NAME);
        }
        if (corrected != 0 || failed != 0) {
            log_notice(
                "Memory changed that log over the restart: %zu bytes corrected, %zu blocks of %d "
                "bytes past correcting\n",
                corrected, failed, REED_SOLOMON_BLOCK
            );
        }
    }
    // Once is enough.
    struct handoff_log *log = (struct handoff_log *)f->page;
    memset(log->magic, 0, sizeof(log->magic));
    cache_write_back(log->magic, sizeof(log->magic));
    efi_bs->free_pages(f->page, pages);
}

void last_log_write(void) {
    found_count = 0;
    kept_find(note, NULL);
    if (found_count == 0) {
        log_notice(
            "No log from ReBIOS's last start for %s: save_log was off then, or the machine was "
            "turned off since\n",
            FILE_NAME
        );
        return;
    }
    size_t latest = 0;
    for (size_t i = 1; i < found_count; i++) {
        if (later(&found[i].started, &found[latest].started)) {
            latest = i;
        }
    }
    for (size_t i = 0; i < found_count; i++) {
        take(&found[i], i == latest);
    }
}

void last_log_keep(void) {
    uint64_t area = kept_reserve(HANDOFF_KEPT_SIZE);
    if (area == 0) {
        log_notice("No memory to keep the log in for the next start\n");
        return;
    }
    struct handoff_log *log = (struct handoff_log *)(uintptr_t)area;
    memset(log, 0, sizeof(struct handoff_log));
    if (efi_rt->get_time(&log->started, NULL) != EFI_SUCCESS) {
        memset(&log->started, 0, sizeof(struct efi_time));
    }
    log_keep_in(log);
    printf(
        "The log is kept in memory at %#llx, %llu MiB, for the next start to write out\n",
        (unsigned long long)area, (unsigned long long)(HANDOFF_KEPT_SIZE >> 20)
    );
}

// Whether the variable exists, which reads nothing of it.
static bool variable_exists(uint16_t *name) {
    size_t size = 0;
    return efi_rt->get_variable(name, &guid, NULL, &size, NULL) == EFI_BUFFER_TOO_SMALL;
}

static bool variable_delete(uint16_t *name) {
    return variable_exists(name)
        && efi_rt->set_variable(name, &guid, ATTRIBUTES, 0, NULL) == EFI_SUCCESS;
}

void last_log_delete_variables(void) {
    size_t deleted = 0;
    deleted += variable_delete(old_variable) ? 1 : 0;
    deleted += variable_delete(old_end_variable) ? 1 : 0;
    // The parts follow one another up to the first that is missing.
    size_t base_len = ARRAY_COUNT(old_variable) - 1;
    uint16_t name[ARRAY_COUNT(old_variable) + OLD_PART_DIGITS];
    memcpy(name, old_variable, base_len * sizeof(uint16_t));
    name[base_len + OLD_PART_DIGITS] = 0;
    for (size_t part = 0; part < OLD_PARTS; part++) {
        size_t n = part;
        for (size_t i = OLD_PART_DIGITS; i-- > 0; ) {
            name[base_len + i] = (uint16_t)(u'0' + n % 10);
            n /= 10;
        }
        if (!variable_delete(name)) {
            break;
        }
        deleted++;
    }
    if (deleted != 0) {
        printf(
            "Deleted %zu EFI variables that ReBIOS 0.7.0 to 0.11.0 kept their log in\n",
            deleted
        );
    }
}
