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

#ifndef VIRT__HYPERCALL_H
#define VIRT__HYPERCALL_H

// Hypercalls are VMCALL or VMMCALL with EAX holding HYPERCALL_MAGIC in its top
// half and the function in its bottom half. Anything else gets the #UD the
// instruction raises without a hypervisor.
#define HYPERCALL_MAGIC 0x52420000
#define HYPERCALL_FUNCTION_MASK 0x0000ffff

// Writes the character in BL to the hypervisor's log.
#define HYPERCALL_DEBUG_PUTC 0x0000
// Reports a self-test result in EBX.
#define HYPERCALL_TEST_DONE 0xfffe

#endif
