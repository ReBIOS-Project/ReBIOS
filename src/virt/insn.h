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

#ifndef VIRT__INSN_H
#define VIRT__INSN_H

#include <stdbool.h>
#include <virt/vcpu.h>

#define INSN_MAX_LENGTH 15
// The opcode of an instruction outside the one-byte map.
#define INSN_OPCODE_OTHER 0x100

struct insn {
    unsigned length;
    // One-byte map opcodes as they are, others as INSN_OPCODE_OTHER.
    unsigned opcode;
    // In bytes.
    unsigned operand_size;
    unsigned address_size;
    // An F2 or F3 prefix.
    bool rep;
};

// The length of the instruction at the guest's CS:RIP, for intercepted
// instructions that only need skipping, on processors that do not report it.
// Returns 0 if it cannot be read or is not one of those instructions.
unsigned insn_length_intercepted(struct vcpu *v);

// Decodes the length and the sizes of any instruction at the guest's CS:RIP.
// Returns false if it cannot be read or is not a valid instruction.
bool insn_decode(struct vcpu *v, struct insn *insn);

#endif
