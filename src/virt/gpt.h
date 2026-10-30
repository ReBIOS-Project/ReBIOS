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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Guest-physical address translation, through EPT on VMX or nested paging on
// SVM. Access rights only: an access a mapping does not allow causes an exit.
#define GPT_R ((uint32_t)1 << 0)
#define GPT_W ((uint32_t)1 << 1)
#define GPT_X ((uint32_t)1 << 2)
#define GPT_RWX (GPT_R | GPT_W | GPT_X)
// Mapped with 4 KiB pages, whose dirty flags gpt_dirty() reads.
#define GPT_DIRTY ((uint32_t)1 << 3)

// Memory types, with the encodings EPT and the PAT use.
#define MT_UC 0
#define MT_WC 1
#define MT_WT 4
#define MT_WP 5
#define MT_WB 6

// The value the host's IA32_PAT gets, so that nested paging can name every
// memory type above. This is the layout Linux 7.3 uses (pat_bp_init()).
#define HOST_PAT ((uint64_t)0x0407050600070106)

// dirty_flags tells whether the processor can keep dirty flags in the tables'
// leaves, which EPT does only once enabled.
void gpt_init(bool ept, bool huge_pages, bool dirty_flags);
// The tables the processor walks now, which gpt_mask_a20() switches.
uintptr_t gpt_root(void);

// size and both addresses must be page aligned. prot 0 unmaps. The IOMMU
// mirrors the mapping, so that device DMA sees the same memory.
void gpt_map(
    uintptr_t gpa, uintptr_t hpa, size_t size, uint32_t prot, uint8_t memtype
);

// Has the processor, but not device DMA, see guest-physical memory below 4 GiB
// with address bit 20 clear, as a closed A20 gate makes it, or as it is.
void gpt_mask_a20(bool on);
// The guest-physical address the processor reaches for gpa, A20 included.
uintptr_t gpt_cpu_gpa(uintptr_t gpa);

// Returns false if gpa is unmapped, ignoring A20. memtype may be NULL.
bool gpt_translate(
    uintptr_t gpa, uint64_t *hpa, uint32_t *prot, uint8_t *memtype
);
// Also the level of the leaf that maps it: 0 for 4 KiB, 1 for 2 MiB, 2 for 1
// GiB.
bool gpt_translate_leaf(
    uintptr_t gpa, uint64_t *hpa, uint32_t *prot, uint8_t *memtype,
    size_t *level
);

// Drops cached translations, on every CPU that will next enter the guest.
void gpt_flush(void);

// Has the processor keep dirty flags where it can, from before the guest first
// runs; whether it does.
bool gpt_track_dirty(void);
bool gpt_dirty_tracked(void);
// Sets bit first + i of bits for each page i of [gpa, gpa + size), mapped with
// GPT_DIRTY, that the guest wrote to since its dirty flag was cleared,
// clearing the flag where clear; whether any page was written to. bits may be
// NULL. As TLBs may keep translations that are dirty already, the other vCPUs
// must stay out of the guest from clearing flags until gpt_flush().
bool gpt_dirty(
    uintptr_t gpa, size_t size, uint64_t *bits, uintptr_t first, bool clear
);

// Bumped by gpt_flush(), for the vCPUs to catch up with as they enter the
// guest.
extern uint64_t gpt_generation;

#endif
