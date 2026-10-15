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

#ifndef LOADER__IOMMU_CHECK_H
#define LOADER__IOMMU_CHECK_H

#include <stdbool.h>

// Whether the firmware describes, in a DMAR or IVRS, an IOMMU the hypervisor
// can use, which it cannot do without: if not, it says so on the console,
// whatever the log's configuration, and leaves time to read it.
bool iommu_check(void);

#endif
