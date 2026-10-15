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
#include <core.h>
#include <handoff.h>
#include <lib/log.h>
#include <sys/x86.h>

void core_main(struct handoff *handoff) {
    printf("ReBIOS core running, heap %#llx-%#llx\n",
           (unsigned long long)handoff->heap_base,
           (unsigned long long)(handoff->heap_base + handoff->heap_size));

    halt_forever();
}
