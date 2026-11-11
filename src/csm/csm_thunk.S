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

#include <virt/hypercall.h>

// The real-mode code through which the guest calls the CSM16, run with CS at
// its start. The hypervisor sets up each call's registers and far pointer; the
// call's AX and BX come back to it through a hypercall, which on SVM is
// patched to VMMCALL.
.section .rodata
.globl csm_thunk_start
.type csm_thunk_start, %object
.globl csm_thunk_hypercall
.type csm_thunk_hypercall, %object
.globl csm_thunk_halt
.type csm_thunk_halt, %object
.globl csm_thunk_entry
.type csm_thunk_entry, %object
.globl csm_thunk_end
.type csm_thunk_end, %object
.code16
csm_thunk_start:
    lcallw *%cs:(csm_thunk_entry - csm_thunk_start)
    movl %eax, %esi
    movl $(HYPERCALL_MAGIC | HYPERCALL_CSM_RETURN), %eax
csm_thunk_hypercall:
    vmcall
.size csm_thunk_hypercall, . - csm_thunk_hypercall
    jmp csm_thunk_start
csm_thunk_halt:
    cli
    hlt
    jmp csm_thunk_halt
.size csm_thunk_halt, . - csm_thunk_halt
.balign 4
csm_thunk_entry:
.word 0
.word 0
.size csm_thunk_entry, . - csm_thunk_entry
.size csm_thunk_start, . - csm_thunk_start
csm_thunk_end:
.size csm_thunk_end, . - csm_thunk_end
.code64
