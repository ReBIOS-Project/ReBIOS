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

#ifndef LIB__AML_H
#define LIB__AML_H

#include <stdint.h>
#include <stdbool.h>

// The opcodes of ACPI 1.0b, Table 16-2, which every AML interpreter knows.
// Two-byte ones carry the extended prefix in their high byte.
#define AML_ZERO 0x00
#define AML_ONE 0x01
#define AML_ALIAS 0x06
#define AML_NAME 0x08
#define AML_BYTE_PREFIX 0x0a
#define AML_WORD_PREFIX 0x0b
#define AML_DWORD_PREFIX 0x0c
#define AML_STRING_PREFIX 0x0d
#define AML_SCOPE 0x10
#define AML_BUFFER 0x11
#define AML_PACKAGE 0x12
#define AML_METHOD 0x14
#define AML_DUAL_NAME_PREFIX 0x2e
#define AML_MULTI_NAME_PREFIX 0x2f
#define AML_EXT_PREFIX 0x5b
#define AML_ROOT_CHAR 0x5c
#define AML_PARENT_PREFIX 0x5e
#define AML_LOCAL0 0x60
#define AML_ARG0 0x68
#define AML_STORE 0x70
#define AML_REF_OF 0x71
#define AML_ADD 0x72
#define AML_CONCAT 0x73
#define AML_SUBTRACT 0x74
#define AML_INCREMENT 0x75
#define AML_DECREMENT 0x76
#define AML_MULTIPLY 0x77
#define AML_DIVIDE 0x78
#define AML_SHIFT_LEFT 0x79
#define AML_SHIFT_RIGHT 0x7a
#define AML_AND 0x7b
#define AML_OR 0x7d
#define AML_XOR 0x7f
#define AML_NOT 0x80
#define AML_DEREF_OF 0x83
#define AML_NOTIFY 0x86
#define AML_SIZE_OF 0x87
#define AML_INDEX 0x88
#define AML_CREATE_DWORD_FIELD 0x8a
#define AML_CREATE_WORD_FIELD 0x8b
#define AML_CREATE_BYTE_FIELD 0x8c
#define AML_OBJECT_TYPE 0x8e
#define AML_LAND 0x90
#define AML_LOR 0x91
#define AML_LNOT 0x92
#define AML_LEQUAL 0x93
#define AML_LGREATER 0x94
#define AML_LLESS 0x95
#define AML_IF 0xa0
#define AML_ELSE 0xa1
#define AML_WHILE 0xa2
#define AML_NOOP 0xa3
#define AML_RETURN 0xa4
#define AML_BREAK 0xa5
#define AML_ONES 0xff
#define AML_MUTEX 0x5b01
#define AML_EVENT 0x5b02
#define AML_COND_REF_OF 0x5b12
#define AML_CREATE_FIELD 0x5b13
#define AML_STALL 0x5b21
#define AML_SLEEP 0x5b22
#define AML_ACQUIRE 0x5b23
#define AML_RELEASE 0x5b27
#define AML_DEBUG 0x5b31
#define AML_OPERATION_REGION 0x5b80
#define AML_FIELD 0x5b81
#define AML_DEVICE 0x5b82
#define AML_PROCESSOR 0x5b83
#define AML_POWER_RESOURCE 0x5b84
#define AML_THERMAL_ZONE 0x5b85

#define AML_REGION_SYSTEM_MEMORY 0
#define AML_REGION_SYSTEM_IO 1

// Field flags: access type in bits 0-3, lock rule in bit 4, update rule in
// bits 5-6.
#define AML_FIELD_ANY_ACC 0
#define AML_FIELD_BYTE_ACC 1
#define AML_FIELD_WORD_ACC 2
#define AML_FIELD_DWORD_ACC 3

#define AML_METHOD_SERIALIZED 0x08

// ObjectType() results.
#define AML_TYPE_INTEGER 1
#define AML_TYPE_STRING 2
#define AML_TYPE_BUFFER 3
#define AML_TYPE_PACKAGE 4

#define AML_MAX_DEPTH 32

// AML being built, growing as needed. Constructs that carry a PkgLength open
// with aml_begin() and close with aml_end().
struct aml {
    uint8_t *data;
    uint32_t size;
    uint32_t capacity;
    uint32_t open[AML_MAX_DEPTH];
    unsigned depth;
};

void aml_init(struct aml *a);
void aml_free(struct aml *a);

void aml_byte(struct aml *a, uint8_t value);
void aml_bytes(struct aml *a, const void *data, uint32_t size);
void aml_word(struct aml *a, uint16_t value);
void aml_dword(struct aml *a, uint32_t value);
// An opcode, with the extended prefix if it has one.
void aml_op(struct aml *a, uint16_t op);
// An opcode with a PkgLength after it, up to the matching aml_end().
void aml_begin(struct aml *a, uint16_t op);
void aml_end(struct aml *a);

// A NameString from a path in ASL's notation ("\\_SB.PCI0", "^^XYZ", "ABC"),
// each segment padded with underscores to four characters.
void aml_name(struct aml *a, const char *path);
// A NameString from four-character segments, rooted or relative.
void aml_name_segs(struct aml *a, bool root, const char (*segs)[4], unsigned count);
// The shortest constant for a value, which ACPI 1.0 keeps to 32 bits.
void aml_int(struct aml *a, uint32_t value);
void aml_string(struct aml *a, const char *s, uint32_t length);
void aml_buffer(struct aml *a, const void *data, uint32_t size);
void aml_local(struct aml *a, unsigned n);
void aml_arg(struct aml *a, unsigned n);

// Name(path, Integer); Method(path, args, flags) {, closed by aml_end().
void aml_name_int(struct aml *a, const char *path, uint32_t value);
void aml_name_string(struct aml *a, const char *path, const char *s);
void aml_method(struct aml *a, const char *path, unsigned args, uint8_t flags);
// Return (Integer).
void aml_return_int(struct aml *a, uint32_t value);
// A NamedField of a Field: four characters and a width in bits.
void aml_field_unit(struct aml *a, const char *name, uint32_t bits);

// Fills a definition block's header, checksum included, in front of the AML
// at table: SDT_HEADER_SIZE bytes, then the AML.
void aml_table_header(uint8_t *table, uint32_t length, const char signature[4], uint8_t revision,
                      const char oem_id[6], const char oem_table_id[8], uint32_t oem_revision);

#define AML_SDT_HEADER_SIZE 36

#endif
