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

#ifndef VIRT__GUEST_MEM_H
#define VIRT__GUEST_MEM_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <virt/vcpu.h>

#define PF_ERROR_P (1u << 0)
#define PF_ERROR_W (1u << 1)
#define PF_ERROR_U (1u << 2)
#define PF_ERROR_RSVD (1u << 3)
#define PF_ERROR_I (1u << 4)
#define PF_ERROR_PK (1u << 5)
// Not a page fault: the access made an exit of the guest's guest instead,
// which waits for the next entry.
#define PF_ERROR_EXIT (1u << 31)

// Accesses guest-physical memory that is mapped to RAM. Returns false if any
// of it is not.
bool guest_phys_read(uint64_t gpa, void *buf, size_t len);
bool guest_phys_write(uint64_t gpa, const void *buf, size_t len);

// Whether PAE paging may load the PDPTE: not present, or setting no bit it
// reserves.
bool guest_pdpte_valid(uint64_t pdpte);

// Translates through the guest's own paging, as the processor would for the
// access. On failure, *pf_error holds the page fault error code to raise.
bool guest_linear_to_phys(struct vcpu *v, uint64_t linear, bool write, bool fetch,
                          uint64_t *gpa, uint32_t *pf_error);

// Reads at a linear address, crossing pages as needed.
bool guest_linear_read(struct vcpu *v, uint64_t linear, void *buf, size_t len, bool fetch);
// As the processor accesses descriptor tables and the TSS: with supervisor
// rights, crossing pages, reaching emulated devices and absent memory as
// instructions do. On failure, *pf_error holds the page fault's error code, to
// raise at *pf_linear.
bool guest_system_read(struct vcpu *v, uint64_t linear, void *buf, size_t len, uint32_t *pf_error,
                       uint64_t *pf_linear);
bool guest_system_write(struct vcpu *v, uint64_t linear, const void *buf, size_t len,
                        uint32_t *pf_error, uint64_t *pf_linear);

#endif
