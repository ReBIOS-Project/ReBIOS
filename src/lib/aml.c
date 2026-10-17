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
#include <memory.h>
#include <lib/aml.h>
#include <lib/heap.h>
#include <lib/log.h>

// Room for the longest PkgLength while a construct is open.
#define PKG_LENGTH_MAX 4

void aml_init(struct aml *a) {
    *a = (struct aml){0};
}

void aml_free(struct aml *a) {
    kfree(a->data);
    *a = (struct aml){0};
}

static void reserve(struct aml *a, uint32_t more) {
    if (a->capacity - a->size >= more) {
        return;
    }
    uint32_t capacity = a->capacity != 0 ? a->capacity : 4096;
    while (capacity - a->size < more) {
        capacity *= 2;
    }
    uint8_t *data = kmalloc(capacity);
    if (a->size != 0) {
        memcpy(data, a->data, a->size);
    }
    kfree(a->data);
    a->data = data;
    a->capacity = capacity;
}

void aml_bytes(struct aml *a, const void *data, uint32_t size) {
    reserve(a, size);
    memcpy(a->data + a->size, data, size);
    a->size += size;
}

void aml_byte(struct aml *a, uint8_t value) {
    aml_bytes(a, &value, 1);
}

void aml_word(struct aml *a, uint16_t value) {
    uint8_t b[2] = {(uint8_t)value, (uint8_t)(value >> 8)};
    aml_bytes(a, b, sizeof(b));
}

void aml_dword(struct aml *a, uint32_t value) {
    uint8_t b[4] = {(uint8_t)value, (uint8_t)(value >> 8), (uint8_t)(value >> 16),
                    (uint8_t)(value >> 24)};
    aml_bytes(a, b, sizeof(b));
}

void aml_op(struct aml *a, uint16_t op) {
    if (op > 0xff) {
        aml_byte(a, (uint8_t)(op >> 8));
    }
    aml_byte(a, (uint8_t)op);
}

// The PkgLength encoding of value in n bytes (ACPI 1.0b, 16.2.4): the count of
// bytes that follow the first in bits 6-7, then four bits, then whole bytes.
static void pkg_encode(uint8_t *out, uint32_t value, unsigned n) {
    if (n == 1) {
        out[0] = (uint8_t)value;
        return;
    }
    out[0] = (uint8_t)((n - 1) << 6 | (value & 0x0f));
    for (unsigned i = 1; i < n; i++) {
        out[i] = (uint8_t)(value >> (4 + 8 * (i - 1)));
    }
}

void aml_begin(struct aml *a, uint16_t op) {
    if (a->depth == AML_MAX_DEPTH) {
        panic("AML nested too deeply\n");
    }
    aml_op(a, op);
    a->open[a->depth++] = a->size;
    reserve(a, PKG_LENGTH_MAX);
    a->size += PKG_LENGTH_MAX;
}

void aml_end(struct aml *a) {
    uint32_t at = a->open[--a->depth];
    uint32_t content = a->size - at - PKG_LENGTH_MAX;
    // A package's length counts its own encoding.
    unsigned n = content + 1 < 0x40 ? 1 : content + 2 < 0x1000 ? 2 : content + 3 < 0x100000 ? 3 : 4;
    memmove(a->data + at + n, a->data + at + PKG_LENGTH_MAX, content);
    pkg_encode(a->data + at, content + n, n);
    a->size -= PKG_LENGTH_MAX - n;
}

static void seg(struct aml *a, const char *s, size_t length) {
    char out[4] = {'_', '_', '_', '_'};
    for (size_t i = 0; i < length && i < 4; i++) {
        out[i] = s[i];
    }
    aml_bytes(a, out, 4);
}

static void seg_count(struct aml *a, unsigned count) {
    if (count == 0) {
        aml_byte(a, 0);
    } else if (count == 2) {
        aml_byte(a, AML_DUAL_NAME_PREFIX);
    } else if (count > 2) {
        aml_byte(a, AML_MULTI_NAME_PREFIX);
        aml_byte(a, (uint8_t)count);
    }
}

void aml_name(struct aml *a, const char *path) {
    if (*path == '\\') {
        aml_byte(a, AML_ROOT_CHAR);
        path++;
    }
    while (*path == '^') {
        aml_byte(a, AML_PARENT_PREFIX);
        path++;
    }
    unsigned count = 0;
    for (const char *p = path; *p != '\0'; p++) {
        if (p == path || p[-1] == '.') {
            count++;
        }
    }
    seg_count(a, count);
    while (*path != '\0') {
        const char *dot = path;
        while (*dot != '\0' && *dot != '.') {
            dot++;
        }
        seg(a, path, (size_t)(dot - path));
        path = *dot == '.' ? dot + 1 : dot;
    }
}

void aml_name_segs(struct aml *a, bool root, const char (*segs)[4], unsigned count) {
    if (root) {
        aml_byte(a, AML_ROOT_CHAR);
    }
    seg_count(a, count);
    for (unsigned i = 0; i < count; i++) {
        aml_bytes(a, segs[i], 4);
    }
}

void aml_int(struct aml *a, uint32_t value) {
    if (value == 0) {
        aml_byte(a, AML_ZERO);
    } else if (value == 1) {
        aml_byte(a, AML_ONE);
    } else if (value <= 0xff) {
        aml_byte(a, AML_BYTE_PREFIX);
        aml_byte(a, (uint8_t)value);
    } else if (value <= 0xffff) {
        aml_byte(a, AML_WORD_PREFIX);
        aml_word(a, (uint16_t)value);
    } else {
        aml_byte(a, AML_DWORD_PREFIX);
        aml_dword(a, value);
    }
}

void aml_string(struct aml *a, const char *s, uint32_t length) {
    aml_byte(a, AML_STRING_PREFIX);
    for (uint32_t i = 0; i < length && s[i] != '\0'; i++) {
        aml_byte(a, (uint8_t)s[i]);
    }
    aml_byte(a, 0);
}

void aml_buffer(struct aml *a, const void *data, uint32_t size) {
    aml_begin(a, AML_BUFFER);
    aml_int(a, size);
    aml_bytes(a, data, size);
    aml_end(a);
}

void aml_local(struct aml *a, unsigned n) {
    aml_byte(a, (uint8_t)(AML_LOCAL0 + n));
}

void aml_arg(struct aml *a, unsigned n) {
    aml_byte(a, (uint8_t)(AML_ARG0 + n));
}

void aml_name_int(struct aml *a, const char *path, uint32_t value) {
    aml_op(a, AML_NAME);
    aml_name(a, path);
    aml_int(a, value);
}

void aml_name_string(struct aml *a, const char *path, const char *s) {
    aml_op(a, AML_NAME);
    aml_name(a, path);
    uint32_t length = 0;
    while (s[length] != '\0') {
        length++;
    }
    aml_string(a, s, length);
}

void aml_method(struct aml *a, const char *path, unsigned args, uint8_t flags) {
    aml_begin(a, AML_METHOD);
    aml_name(a, path);
    aml_byte(a, (uint8_t)(args | flags));
}

void aml_return_int(struct aml *a, uint32_t value) {
    aml_op(a, AML_RETURN);
    aml_int(a, value);
}

// A field's width is a PkgLength that does not count itself.
void aml_field_unit(struct aml *a, const char *name, uint32_t bits) {
    if (name != NULL) {
        size_t length = 0;
        while (name[length] != '\0') {
            length++;
        }
        seg(a, name, length);
    } else {
        aml_byte(a, 0);
    }
    unsigned n = bits < 0x40 ? 1 : bits < 0x1000 ? 2 : bits < 0x100000 ? 3 : 4;
    uint8_t out[4];
    pkg_encode(out, bits, n);
    aml_bytes(a, out, n);
}

void aml_table_header(uint8_t *table, uint32_t length, const char signature[4], uint8_t revision,
                      const char oem_id[6], const char oem_table_id[8], uint32_t oem_revision) {
    memcpy(table, signature, 4);
    memcpy(table + 4, &length, 4);
    table[8] = revision;
    table[9] = 0;
    memcpy(table + 10, oem_id, 6);
    memcpy(table + 16, oem_table_id, 8);
    memcpy(table + 24, &oem_revision, 4);
    memcpy(table + 28, "RBIO", 4);
    uint32_t creator_revision = 1;
    memcpy(table + 32, &creator_revision, 4);
    uint8_t sum = 0;
    for (uint32_t i = 0; i < length; i++) {
        sum += table[i];
    }
    table[9] = (uint8_t)-sum;
}
