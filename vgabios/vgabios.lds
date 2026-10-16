/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of ReBIOS.
 * Copyright (C) 2026 Mintsuki and contributors
 *
 * ReBIOS is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * ReBIOS is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along
 * with ReBIOS. If not, see <https://www.gnu.org/licenses/>.
 */

OUTPUT_FORMAT("elf32-i386")
OUTPUT_ARCH(i386)
ENTRY(vgabios_header)

SECTIONS
{
    . = 0;
    .header : {
        KEEP(*(.header))
    }
    .text : {
        *(.text.entry)
        *(.text .text.*)
    }
    .rodata : {
        *(.rodata .rodata.*)
    }
    .data : {
        *(.data .data.*)
        *(.bss .bss.*)
        *(COMMON)
    }
    vgabios_size = ALIGN(., 512);
    vgabios_blocks = vgabios_size / 512;
    /DISCARD/ : {
        *(.comment)
        *(.note .note.*)
        *(.eh_frame .eh_frame_hdr)
    }
}

ASSERT(SIZEOF(.data) == 0, "the video BIOS has data it would write in its ROM")
ASSERT(vgabios_size <= 0x8000, "the video BIOS is larger than 32 KiB")
