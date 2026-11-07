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
#include <sys/fixup.h>

struct fixup {
    int32_t insn;
    int32_t resume;
};

extern const struct fixup __start_fixup_table[];
extern const struct fixup __stop_fixup_table[];

uintptr_t fixup_find(uintptr_t rip) {
    for (const struct fixup *f = __start_fixup_table; f < __stop_fixup_table; f++) {
        if ((uintptr_t)&f->insn + f->insn == rip) {
            return (uintptr_t)&f->resume + f->resume;
        }
    }
    return 0;
}
