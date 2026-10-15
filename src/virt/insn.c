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
#include <stdbool.h>
#include <sys/x86.h>
#include <virt/guest_mem.h>
#include <virt/insn.h>
#include <virt/vcpu.h>

static bool is_legacy_prefix(uint8_t b) {
    switch (b) {
        case 0x26:
        case 0x2e:
        case 0x36:
        case 0x3e:
        case 0x64:
        case 0x65:
        case 0x66:
        case 0x67:
        case 0xf0:
        case 0xf2:
        case 0xf3: {
            return true;
        }
        default: {
            return false;
        }
    }
}

unsigned insn_length_intercepted(struct vcpu *v) {
    struct segment cs;
    vcpu_get_seg(v, SEG_CS, &cs);
    bool long_mode = (vcpu_efer(v) & EFER_LMA) && (cs.attrib & 0x200);
    uint64_t linear = vcpu_rip(v);
    if (!long_mode) {
        linear = (cs.base + linear) & 0xffffffff;
    }

    uint8_t bytes[15];
    if (!guest_linear_read(v, linear, bytes, sizeof(bytes), true)) {
        // The instruction may end before a page that is not mapped.
        unsigned avail = 4096 - (linear & 4095);
        if (avail >= sizeof(bytes) || !guest_linear_read(v, linear, bytes, avail, true)) {
            return 0;
        }
    }

    unsigned i = 0;
    while (i < sizeof(bytes) && is_legacy_prefix(bytes[i])) {
        i++;
    }
    if (long_mode && i < sizeof(bytes) && (bytes[i] & 0xf0) == 0x40) {
        i++;
    }
    if (i + 3 > sizeof(bytes)) {
        return 0;
    }

    if (bytes[i] == 0xf4) {
        return i + 1;
    }
    if (bytes[i] != 0x0f) {
        return 0;
    }
    switch (bytes[i + 1]) {
        case 0x08:
        case 0x09:
        case 0x30:
        case 0x32:
        case 0xa2: {
            return i + 2;
        }
        case 0x01: {
            uint8_t modrm = bytes[i + 2];
            if (modrm == 0xc1 || modrm == 0xd1 || (modrm >= 0xd8 && modrm <= 0xdf)) {
                return i + 3;
            }
            return 0;
        }
        default: {
            return 0;
        }
    }
}
