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

#ifndef VIRT__HYPERCALL_H
#define VIRT__HYPERCALL_H

// Hypercalls are VMCALL or VMMCALL with EAX holding HYPERCALL_MAGIC in its top
// half and the function in its bottom half. Anything else gets the #UD the
// instruction raises without a hypervisor.
#define HYPERCALL_MAGIC 0x52420000
#define HYPERCALL_FUNCTION_MASK 0x0000ffff

// Writes the character in BL to the hypervisor's log.
#define HYPERCALL_DEBUG_PUTC 0x0000
// Returns from a CSM16 call, with its AX in SI and its BX in BX.
#define HYPERCALL_CSM_RETURN 0x0001
// Resets the machine or powers it off, as EBX's HYPERCALL_RESET_* says.
#define HYPERCALL_RESET 0x0002
// Records the processor state, in 32-bit flat protected mode, that
// HYPERCALL_CALL32_ENTER switches to.
#define HYPERCALL_CALL32_INIT 0x0003
// Saves the processor state and switches to the recorded one, at EBX.
#define HYPERCALL_CALL32_ENTER 0x0004
// Switches back to the state the last HYPERCALL_CALL32_ENTER saved, at EBX.
#define HYPERCALL_CALL32_RETURN 0x0005

#define HYPERCALL_RESET_COLD 0
#define HYPERCALL_RESET_WARM 1
#define HYPERCALL_RESET_SHUTDOWN 2

// The CSM16 table's HypervisorInterface field, telling the legacy BIOS it may
// make these hypercalls.
#define HYPERCALL_INTERFACE 0x52420001

#endif
