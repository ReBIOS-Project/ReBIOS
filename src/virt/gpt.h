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

#ifndef VIRT__GPT_H
#define VIRT__GPT_H

#include <stdint.h>
#include <stdbool.h>

// Guest-physical address translation, through EPT on VMX or nested paging on
// SVM. Access rights only: an access a mapping does not allow causes an exit.
#define GPT_R (1u << 0)
#define GPT_W (1u << 1)
#define GPT_X (1u << 2)
#define GPT_RWX (GPT_R | GPT_W | GPT_X)

// Memory types, with the encodings EPT and the PAT use.
#define MT_UC 0
#define MT_WC 1
#define MT_WT 4
#define MT_WP 5
#define MT_WB 6

// The value the host's IA32_PAT gets, so that nested paging can name every
// memory type above. This is the layout Linux 7.3 uses (pat_bp_init()).
#define HOST_PAT 0x0407050600070106ull

void gpt_init(bool ept, bool huge_pages);
// The tables the processor walks now, which gpt_mask_a20() switches.
uint64_t gpt_root(void);

// size and both addresses must be page aligned. prot 0 unmaps. The IOMMU
// mirrors the mapping, so that device DMA sees the same memory.
void gpt_map(uint64_t gpa, uint64_t hpa, uint64_t size, uint32_t prot, uint8_t memtype);

// Has the processor, but not device DMA, see guest-physical memory below 4 GiB
// with address bit 20 clear, as a closed A20 gate makes it, or as it is.
void gpt_mask_a20(bool on);
// The guest-physical address the processor reaches for gpa, A20 included.
uint64_t gpt_cpu_gpa(uint64_t gpa);

// Returns false if gpa is unmapped, ignoring A20. memtype may be NULL.
bool gpt_translate(uint64_t gpa, uint64_t *hpa, uint32_t *prot, uint8_t *memtype);
// Also the level of the leaf that maps it: 0 for 4 KiB, 1 for 2 MiB, 2 for
// 1 GiB.
bool gpt_translate_leaf(uint64_t gpa, uint64_t *hpa, uint32_t *prot, uint8_t *memtype,
                        int *level);

// Drops cached translations, on every CPU that will next enter the guest.
void gpt_flush(void);

// Bumped by gpt_flush(), for the vCPUs to catch up with as they enter the
// guest.
extern uint64_t gpt_generation;

#endif
