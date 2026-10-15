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

#ifndef IOMMU__DMA_H
#define IOMMU__DMA_H

#include <stdint.h>

// Device DMA sees the guest's physical address space, through the IOMMU,
// whose tables mirror every mapping gpt_map() makes. Needs acpi_init() and
// pci_cfg_init(), and comes before any gpt_map().
void dma_init(void);

// gpt_map()'s mirror: prot as there, 0 to unmap.
void dma_map(uint64_t gpa, uint64_t hpa, uint64_t size, uint32_t prot);

// Turns translation on, once the guest's memory is mapped and before it runs.
void dma_enable(void);

// Logs, and clears, the faults the IOMMUs recorded.
void dma_report_faults(void);

#endif
