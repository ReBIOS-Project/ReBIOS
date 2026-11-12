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

#ifndef VIRT__DEVMEM_H
#define VIRT__DEVMEM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The memory of the firmware's ACPI devices, at addresses of the guest's own:
// a block below the I/O APICs' area holds the devices' memory, in the host's
// order, and a TPM's is where the TCG's PC Client specs put it.

// Lays the devices' memory out in the guest, with room to spare for memory
// that the firmware's AML gives later. Needs pci_host_scan().
void devmem_plan(void);
// The guest's ranges that devmem_plan() took, [base, end): the block, then a
// TPM's range, false past the last.
bool devmem_range(size_t i, uintptr_t *base, uintptr_t *end);
// The block's room that devmem_plan() gave no device's memory, [base, end).
bool devmem_spare(uintptr_t *base, uintptr_t *end);
// Maps what devmem_plan() laid out onto the host's memory.
void devmem_map(void);
// The guest's address for the host's [base, end): its interrupt controllers'
// keep theirs. With late, memory a device has that has none yet gets one in
// the block's spare room, where it fits. False where it has none.
bool devmem_guest(uint64_t base, uint64_t end, bool late, uint64_t *guest);
// The host's address for the guest's [base, end), false where it has none.
bool devmem_host(uint64_t base, uint64_t end, uint64_t *host);
// Whether the guest's [base, end) is all at an interrupt controller's
// addresses, which the guest keeps.
bool devmem_interrupts(uint64_t base, uint64_t end);

#endif
