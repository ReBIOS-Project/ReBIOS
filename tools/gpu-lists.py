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

# gpu-lists.py <pci.ids> <amdgpu.ids> <Linux tree> <output.c> [report]: generates
# the lists of GPUs whose legacy Video BIOS is run or not. Integrated graphics
# and GPUs soldered to the board go by when they came out: those from before
# 2018 are allowed and later ones blocked. Cards for slots are allowed. The
# inputs are hwdata's pci.ids, libdrm's amdgpu.ids (which tell apart by revision
# the cards and soldered parts that share a device ID), and Linux's
# include/drm/intel/pciids.h.
import re
import sys

NOTICE = """// SPDX-License-Identifier: GPL-2.0-only
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
"""

INTEL = 0x8086
AMD = 0x1002
NVIDIA = 0x10de
ANY = -1

# Intel's integrated graphics by Linux's platform macros, from before 2018:
# Kaby Lake's but for Amber Lake's IDs, and the desktop Coffee Lake of October
# 2017. Valley View and Cherry View have no VT-d, so ReBIOS cannot run there.
INTEL_BEFORE = ['ILK', 'SNB', 'IVB', 'HSW', 'BDW', 'SKL', 'BXT', 'GLK', 'KBL',
                'CFL_S_GT1', 'CFL_S_GT2']
INTEL_AMBER_LAKE = ['AML_KBL_GT2', 'AML_CFL_GT2']
INTEL_LATER = ['CFL', 'WHL', 'CML', 'CNL', 'ICL', 'EHL', 'JSL', 'TGL', 'RKL',
               'ADLS', 'ADLP', 'ADLN', 'RPLS', 'RPLU', 'RPLP', 'ARL', 'MTL',
               'LNL', 'PTL', 'WCL', 'NVLS', 'NVLP'] + INTEL_AMBER_LAKE
# Only those pci.ids gives a model: "Intel Graphics" could be any of them.
INTEL_DISCRETE = re.compile(
    r'^(DG1|DG2|Battlemage \w+) \[(Arc( Pro)? [AB]\d|Iris Xe)')

# What pci.ids calls functions of GPUs that are not their graphics.
NOT_GRAPHICS = re.compile(
    r'(?i)audio|usb|ucsi|smbus|bridge|i2c|memory controller|ethernet|nvlink'
    r'|\bpsp\b|\bhost\b|pci express|switch|\broot\b|\bport\b|crypto|sensor'
    r'|thunderbolt')

SOLDERED = re.compile(r'^\w+M\b|Mobile|Max-Q|Laptop|Embedded')

# Pascal's mobile parts that came out from 2018 on. All other NVIDIA GPUs up to
# Pascal came out before.
NVIDIA_PASCAL_LATER = re.compile(
    r'MX[23]\d0|Max-Q|Quadro P(5200|4200|3200|600|620|500|520)\b')
NVIDIA_FIRST_TURING = 0x1e00

# AMD's device IDs here are integrated graphics, and from before 2018 only up
# to Kaveri's. No GPU for a slot has them.
AMD_INTEGRATED_LATER = (0x1320, 0x1fff)

# All of AMD's other GPUs up to Fiji and Vega, the first RDNA device ID being
# 7310h, as older ones are numbered all over.
AMD_BEFORE_RDNA = [(0x0000, 0x72ff), (0x7300, 0x730f), (0x9000, 0x9fff)]
AMD_FIRST_RDNA = 0x7310
AMD_LAST_RDNA = 0x8fff
AMD_SOLDERED_NAME = re.compile(r'\d[A-Z]*M( XT)?\b|\d{4}S\b')
# Polaris and Vega parts that came out from 2018 on, for laptops.
AMD_LATER_NAME = re.compile(
    r'Vega M\b|Radeon (Pro )?Vega (16|20)\b|Radeon (RX )?5[456]0X\b|RX 640\b')

# Graphics that hypervisors emulate, whose Video BIOSes are made to be run, and
# old or simple designs, up to the ones soldered to servers' boards.
OTHER_ALLOWED = [
    (0x1234, 0x1111, 0x1111, 'QEMU standard VGA'),
    (0x1af4, 0x1050, 0x1050, 'virtio-vga'),
    (0x1b36, 0x0100, 0x0100, 'QXL'),
    (0x15ad, 0x0405, 0x0405, 'VMware SVGA II'),
    (0x80ee, 0xbeef, 0xbeef, 'VirtualBox'),
    (0x1013, 0x0000, 0xffff, 'Cirrus Logic'),
    (0x1023, 0x0000, 0xffff, 'Trident'),
    (0x1039, 0x0000, 0xffff, 'SiS'),
    (0x102b, 0x0000, 0xffff, 'Matrox'),
    (0x121a, 0x0000, 0xffff, '3dfx'),
    (0x126f, 0x0000, 0xffff, 'Silicon Motion'),
    (0x18ca, 0x0000, 0xffff, 'XGI'),
    (0x1a03, 0x2000, 0x2000, 'ASPEED, on servers\' boards'),
    (0x5333, 0x0000, 0xffff, 'S3'),
]


def read_pci_ids(path):
    ids = {}
    version = '?'
    vendor = None
    for line in open(path, errors='replace'):
        m = re.match(r'^#\s*Version:\s*(\S+)', line)
        if m:
            version = m.group(1)
        if line.startswith('#') or not line.strip():
            continue
        m = re.match(r'^([0-9a-f]{4})  ', line)
        if m:
            vendor = int(m.group(1), 16)
            ids.setdefault(vendor, {})
            continue
        if line.startswith('C '):
            break
        m = re.match(r'^\t([0-9a-f]{4})  (.*)$', line)
        if m and vendor is not None:
            ids[vendor][int(m.group(1), 16)] = m.group(2).strip()
    return ids, version


def read_amdgpu_ids(path):
    rows = []
    for line in open(path, errors='replace'):
        m = re.match(
            r'^([0-9A-Fa-f]{4}),\s*([0-9A-Fa-f]{2}),\s*(.*)$', line.strip())
        if m:
            rows.append((int(m.group(1), 16), int(m.group(2), 16),
                         m.group(3).strip()))
    return rows


def read_intel_ids(path):
    text = open(path).read().replace('\\\n', ' ')
    macros = {}
    for m in re.finditer(r'#define INTEL_(\w+)_IDS\(MACRO__, \.\.\.\)\s+(.*)',
                         text):
        macros[m.group(1)] = m.group(2)

    def expand(name):
        found = []
        for t in re.finditer(r'MACRO__\(0x([0-9A-Fa-f]{4})|INTEL_(\w+)_IDS\(',
                             macros[name]):
            found += [int(t.group(1), 16)] if t.group(1) else expand(t.group(2))
        return found

    return expand


def clean(name):
    name = re.sub(r'\s+', ' ', name).encode('ascii', 'replace').decode()
    return name.replace('*/', '* /').replace('\\', '/')


def row(vendor, first, last, revision, name):
    rev = 'GPU_ANY_REVISION' if revision == ANY else '%#04x' % revision
    entry = '    { %#06x, %#06x, %#06x, %s },' % (vendor, first, last, rev)
    comment = ' // ' + clean(name)
    if len(entry) + len(comment) > 79:
        lead = '    //' + comment[3:]
        return lead[:79].rstrip() + '\n' + entry
    return entry + comment


def main():
    pci_ids, pci_version = read_pci_ids(sys.argv[1])
    amdgpu = read_amdgpu_ids(sys.argv[2])
    intel = read_intel_ids(sys.argv[3] + '/include/drm/intel/pciids.h')
    out = sys.argv[4]

    blocked = []
    allowed = []
    intel_names = pci_ids[INTEL]

    # Intel's integrated graphics.
    amber = {i for p in INTEL_AMBER_LAKE for i in intel(p)}
    before = {i for p in INTEL_BEFORE for i in intel(p)} - amber
    later = {i for p in INTEL_LATER for i in intel(p)} - before
    for i in sorted(before):
        allowed.append((INTEL, i, i, ANY, intel_names.get(i, 'Intel graphics')))
    for i in sorted(later):
        blocked.append((INTEL, i, i, ANY, intel_names.get(i, 'Intel graphics')))

    # Intel's discrete GPUs.
    for i, name in sorted(intel_names.items()):
        if INTEL_DISCRETE.match(name) and not NOT_GRAPHICS.search(name):
            soldered = re.search(r'\bA\d+M\b|Xe MAX|Mobile', name)
            (blocked if soldered else allowed).append(
                (INTEL, i, i, ANY, name)
            )

    # NVIDIA's.
    nvidia = pci_ids[NVIDIA]
    allowed.append((NVIDIA, 0x0000, NVIDIA_FIRST_TURING - 1, ANY,
                    'Up to Pascal, but for those blocked'))
    for i, name in sorted(nvidia.items()):
        if NOT_GRAPHICS.search(name):
            continue
        if i < NVIDIA_FIRST_TURING:
            if SOLDERED.search(name) and NVIDIA_PASCAL_LATER.search(name):
                blocked.append((NVIDIA, i, i, ANY, name))
        elif SOLDERED.search(name):
            blocked.append((NVIDIA, i, i, ANY, name))
        else:
            allowed.append((NVIDIA, i, i, ANY, name))

    # AMD's.
    blocked.append((AMD, AMD_INTEGRATED_LATER[0], AMD_INTEGRATED_LATER[1], ANY,
                    'Integrated graphics after Kaveri'))
    for first, last in AMD_BEFORE_RDNA:
        allowed.append((AMD, first, last, ANY, 'Up to Fiji and Vega, but for '
                        'those blocked'))
    for device, revision, name in amdgpu:
        if AMD_INTEGRATED_LATER[0] <= device <= AMD_INTEGRATED_LATER[1]:
            continue
        if device < AMD_FIRST_RDNA or device > AMD_LAST_RDNA:
            if AMD_LATER_NAME.search(name):
                blocked.append((AMD, device, device, revision, name))
        elif AMD_SOLDERED_NAME.search(name):
            blocked.append((AMD, device, device, revision, name))
        else:
            allowed.append((AMD, device, device, revision, name))

    # Others.
    for vendor, first, last, name in OTHER_ALLOWED:
        allowed.append((vendor, first, last, ANY, name))

    with open(out, 'w') as f:
        f.write(NOTICE)
        f.write('\n// Generated by tools/gpu-lists.py from pci.ids %s, '
                'libdrm\'s\n// amdgpu.ids, and Linux 7.3\'s '
                'include/drm/intel/pciids.h.\n\n' % pci_version)
        f.write('#include <stddef.h>\n#include <stdint.h>\n'
                '#include <loader/gpu_lists.h>\n\n')
        f.write('const struct gpu_id vbios_blocklist[] = {\n')
        f.write('\n'.join(row(*e) for e in blocked) + '\n};\n\n')
        f.write('const size_t vbios_blocklist_count = '
                'sizeof(vbios_blocklist)\n    / sizeof(struct gpu_id);\n\n')
        f.write('const struct gpu_id vbios_allowlist[] = {\n')
        f.write('\n'.join(row(*e) for e in allowed) + '\n};\n\n')
        f.write('const size_t vbios_allowlist_count = '
                'sizeof(vbios_allowlist)\n    / sizeof(struct gpu_id);\n')

    if len(sys.argv) > 5:
        with open(sys.argv[5], 'w') as f:
            for title, rows in (('BLOCKED', blocked), ('ALLOWED', allowed)):
                f.write('== %s %d\n' % (title, len(rows)))
                for vendor, first, last, revision, name in rows:
                    f.write('%04x %04x-%04x %3d %s\n' % (
                        vendor, first, last, revision, clean(name)))


main()
