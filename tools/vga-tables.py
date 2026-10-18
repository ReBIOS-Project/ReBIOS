#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
#
# This file is part of ReBIOS.
# Copyright (C) 2026 Mintsuki and contributors
#
# ReBIOS is free software: you can redistribute it and/or modify it under the
# terms of the GNU General Public License version 2 as published by the Free
# Software Foundation.
#
# ReBIOS is distributed in the hope that it will be useful, but WITHOUT ANY
# WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
# FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
# details.
#
# You should have received a copy of the GNU General Public License along
# with ReBIOS. If not, see <https://www.gnu.org/licenses/>.

# Usage: vga-tables.py <QEMU VGA BIOS binary> <vgadump log> <out.c>
#
# Writes the emulated VGA's video BIOS tables: the 8x8, 8x14, and 8x16 fonts,
# taken from QEMU's VGA BIOS, and each standard mode's registers and DAC
# palette, as vgadump.asm recorded QEMU's VGA BIOS programming them, with the
# 350- and 200-line text modes made from those (QEMU's BIOS has none).
import re
import sys

# The notice each generated file starts with.
NOTICE = [
    '// SPDX-License-Identifier: GPL-2.0-only',
    '//',
    '// This file is part of ReBIOS.',
    '// Copyright (C) 2026 Mintsuki and contributors',
    '//',
    '// ReBIOS is free software: you can redistribute it and/or modify it under the',
    '// terms of the GNU General Public License version 2 as published by the Free',
    '// Software Foundation.',
    '//',
    '// ReBIOS is distributed in the hope that it will be useful, but WITHOUT ANY',
    '// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS',
    '// FOR A PARTICULAR PURPOSE. See the GNU General Public License for more',
    '// details.',
    '//',
    '// You should have received a copy of the GNU General Public License along',
    '// with ReBIOS. If not, see <https://www.gnu.org/licenses/>.',
]

rom_path, log_path, out_path = sys.argv[1:4]
rom = open(rom_path, 'rb').read()
log = open(log_path, encoding='latin-1').read()

# Located by the glyph of 'A', checked against the blank glyph 0 and the
# smiley glyph 1.
def find_font(height, a_glyph, smiley):
    i = rom.find(a_glyph)
    while i >= 0:
        base = i - 0x41 * height
        if base >= 0 and rom[base:base + height] == bytes(height) \
                and rom[base + height:base + 2 * height] == smiley:
            return rom[base:base + 256 * height]
        i = rom.find(a_glyph, i + 1)
    sys.exit('no %d-line font found' % height)

font16 = find_font(16, bytes.fromhex('00001038 6cc6c6fe c6c6c6c6 00000000'),
                   bytes.fromhex('00007e81 a58181bd 9981817e 00000000'))
font14 = find_font(14, bytes.fromhex('00001038 6cc6c6fe c6c6c600 0000'),
                   bytes.fromhex('00007e81 a58181bd 99817e00 0000'))
font8 = find_font(8, bytes.fromhex('3078cccc fccccc00'), bytes.fromhex('7e81a581 bd99817e'))

blocks = re.findall(r'MODE (\w\w)\s+MISC (\w\w)\s+SEQ ([\w ]+?)\s*\n\s*CRTC ([\w ]+?)\s*\n'
                    r'\s*GC ([\w ]+?)\s*\n\s*AC ([\w ]+?)\s*\n\s*BDA ([\w ]+?)\s*\n'
                    r'\s*DAC (\w\w) (\w+)', log)
if len(blocks) != 15:
    sys.exit('expected 15 modes, found %d' % len(blocks))

# Kind, width and height in pixels (graphics), and segment of each mode.
MODES = {
    0x00: ('MODE_TEXT', 0, 0, 0xb800), 0x01: ('MODE_TEXT', 0, 0, 0xb800),
    0x02: ('MODE_TEXT', 0, 0, 0xb800), 0x03: ('MODE_TEXT', 0, 0, 0xb800),
    0x04: ('MODE_CGA', 320, 200, 0xb800), 0x05: ('MODE_CGA', 320, 200, 0xb800),
    0x06: ('MODE_CGA1', 640, 200, 0xb800), 0x07: ('MODE_TEXT', 0, 0, 0xb000),
    0x0d: ('MODE_PLANAR', 320, 200, 0xa000), 0x0e: ('MODE_PLANAR', 640, 200, 0xa000),
    0x0f: ('MODE_PLANAR', 640, 350, 0xa000), 0x10: ('MODE_PLANAR', 640, 350, 0xa000),
    0x11: ('MODE_PLANAR', 640, 480, 0xa000), 0x12: ('MODE_PLANAR', 640, 480, 0xa000),
    0x13: ('MODE_LINEAR', 320, 200, 0xa000),
}

# QEMU's VGA BIOS leaves mode 7's palette registers 0 and 8 at values that
# break the monochrome palette's pattern, which the others follow: black for
# 0, 10h for 8.
def fix_mode7(ac):
    ac[0] = 0x00
    ac[8] = 0x10

palettes = []
modes = []
raw = {}
for b in blocks:
    mode = int(b[0], 16)
    misc = int(b[1], 16)
    seq = [int(x, 16) for x in b[2].split()]
    crtc = [int(x, 16) for x in b[3].split()]
    gc = [int(x, 16) for x in b[4].split()]
    ac = [int(x, 16) for x in b[5].split()]
    bda = [int(x, 16) for x in b[6].split()]
    dac = bytes.fromhex(b[8])
    assert len(seq) == 5 and len(crtc) == 25 and len(gc) == 9 and len(ac) == 21
    if mode == 0x07:
        fix_mode7(ac)
    assert len(dac) == 768
    field = lambda off: bda[off - 0x49]
    columns = field(0x4a) | field(0x4b) << 8
    page_size = field(0x4c) | field(0x4d) << 8
    rows = field(0x84) + 1
    char_height = field(0x85)
    # Each palette's entries past the last one set are left out, as zeros.
    used = len(dac)
    while used > 0 and dac[used - 1] == 0:
        used -= 1
    entries = (used + 2) // 3
    entries = 64 if entries <= 64 else 256
    key = dac[:entries * 3]
    if key not in palettes:
        palettes.append(key)
    kind, width, height, segment = MODES[mode]
    lines = 400 if kind == 'MODE_TEXT' else height
    modes.append((mode, kind, columns, rows, char_height, palettes.index(key), width, height,
                  segment, page_size, misc, seq[1:5], crtc, ac[:20], gc, lines))
    raw[mode] = (misc, seq, crtc)

# The CRT controller's vertical timing registers, and its maximum scan line
# register's bits beyond the cell height.
VERTICAL = (0x06, 0x07, 0x10, 0x11, 0x12, 0x15, 0x16)
HORIZONTAL = (0x00, 0x01, 0x02, 0x03, 0x04, 0x05)

# A text mode at another line count: the vertical timing of a graphics mode
# with that many lines, the horizontal timing and dot clock of the 8-dot
# graphics mode with as many columns (or the text mode's own, for mode 7's
# 9-dot cells), and the text mode's other registers.
def variant(text, lines, vertical_mode, horizontal_mode, height, cursor, underline):
    (mode, kind, columns, rows, _, palette, width, h, segment, page_size, misc, seq, crtc, ac,
     gc, _) = text
    vmisc, _, vcrtc = raw[vertical_mode]
    hmisc, hseq, hcrtc = raw[horizontal_mode]
    crtc = list(crtc)
    for r in VERTICAL:
        crtc[r] = vcrtc[r]
    for r in HORIZONTAL:
        crtc[r] = hcrtc[r]
    crtc[0x09] = (vcrtc[0x09] & 0xe0) | (height - 1)
    crtc[0x0a], crtc[0x0b] = cursor
    crtc[0x14] = underline
    seq = list(seq)
    seq[0] = hseq[1]
    misc = (vmisc & 0xc0) | (hmisc & 0x3f)
    return (mode, kind, columns, rows, height, palette, width, h, segment, page_size, misc, seq,
            crtc, ac, gc, lines)

for text in [m for m in modes if m[0] in (0x00, 0x01, 0x02, 0x03)]:
    narrow = text[2] == 40
    modes.append(variant(text, 350, 0x10, 0x0d if narrow else 0x10, 14, (0x0b, 0x0c), 0x1f))
    modes.append(variant(text, 200, 0x0e, 0x0d if narrow else 0x0e, 8, (0x06, 0x07), 0x1f))
mode7 = [m for m in modes if m[0] == 0x07][0]
modes.append(variant(mode7, 350, 0x10, 0x07, 14, (0x0b, 0x0c), 0x0d))

def hexrows(data, per_row=16, indent='    '):
    lines = []
    for i in range(0, len(data), per_row):
        lines.append(indent + ', '.join('0x%02x' % x for x in data[i:i + per_row]) + ',')
    return '\n'.join(lines)

o = []
o.extend(NOTICE + [''])
o.append('// Generated by tools/vga-tables.py from QEMU\'s VGA BIOS: its fonts, and the')
o.append('// registers and DAC palettes it programs for each standard mode.')
o.append('#include <stdint.h>')
o.append('#include <vgabios.h>')
o.append('')
for name, data in (('font8', font8), ('font14', font14), ('font16', font16)):
    o.append('const __seg_fs uint8_t vgabios_%s[%d] = {' % (name, len(data)))
    o.append(hexrows(data))
    o.append('};')
    o.append('')
for i, p in enumerate(palettes):
    o.append('static const __seg_fs uint8_t palette%d[%d] = {' % (i, len(p)))
    o.append(hexrows(p, 12))
    o.append('};')
    o.append('')
o.append('const __seg_fs struct vgabios_palette vgabios_palettes[%d] = {' % len(palettes))
for i, p in enumerate(palettes):
    o.append('    {palette%d, %d},' % (i, len(p) // 3))
o.append('};')
o.append('')
o.append('const __seg_fs uint8_t vgabios_mode_count = %d;' % len(modes))
o.append('')
o.append('const __seg_fs struct vgabios_mode vgabios_modes[%d] = {' % len(modes))
for m in modes:
    (mode, kind, columns, rows, char_height, palette, width, height, segment, page_size, misc,
     seq, crtc, ac, gc, lines) = m
    o.append('    {')
    o.append('        .mode = 0x%02x,' % mode)
    o.append('        .kind = %s,' % kind)
    o.append('        .columns = %d,' % columns)
    o.append('        .rows = %d,' % rows)
    o.append('        .char_height = %d,' % char_height)
    o.append('        .palette = %d,' % palette)
    o.append('        .width = %d,' % width)
    o.append('        .height = %d,' % height)
    o.append('        .segment = 0x%04x,' % segment)
    o.append('        .page_size = 0x%04x,' % page_size)
    o.append('        .scan_lines = %d,' % lines)
    o.append('        .misc = 0x%02x,' % misc)
    o.append('        .seq = {%s},' % ', '.join('0x%02x' % x for x in seq))
    o.append('        .crtc = {')
    o.append(hexrows(crtc, 13, '            '))
    o.append('        },')
    o.append('        .ac = {')
    o.append(hexrows(ac, 10, '            '))
    o.append('        },')
    o.append('        .gc = {%s},' % ', '.join('0x%02x' % x for x in gc))
    o.append('    },')
o.append('};')
open(out_path, 'w').write('\n'.join(o) + '\n')
print('%d modes, %d palettes' % (len(modes), len(palettes)))
