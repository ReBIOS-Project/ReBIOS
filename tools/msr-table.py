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

# Usage: msr-table.py <SDM text> <output.c> [report]
#
# Generates src/virt/msr_table.c: the MSRs the guest reaches as the processor
# has them. Those of Intel's architecture come from Table 2-2 of the Intel SDM
# 325462-093, Vol. 4, as reviewed below; the model-specific ones from its other
# tables, for the processors that can run the hypervisor, and only those of
# the kinds reviewed as safe to give the guest. The text is `pdftotext -layout`
# output of the manual. With report, it also lists every model-specific MSR it
# leaves out.

import re
import sys
import textwrap

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

# The architectural MSRs the guest gets, by address or range. Left out are
# those modules of the hypervisor handle (APIC_BASE, FEATURE_CONTROL, the
# MTRRs, ARCH_CAPABILITIES, XAPIC_DISABLE_STATUS, the VMX capabilities, the
# x2APIC range), and those of what the guest does not get: the microcode
# update interface beyond the trigger and the signature, SMM, SGX, TME, TDX,
# DCA, debug interface, PASID, user MSRs, MSR lists, FRED, user interrupts,
# architectural LBRs and PEBS, and the reserved ranges.
INTEL_ARCH = [
    (0x0, 0x1),          # Pentium machine check address and type
    (0x6, 0x6),          # MONITOR filter size
    (0x10, 0x10),        # TSC
    (0x17, 0x17),        # platform ID
    (0x3b, 0x3b),        # TSC adjust
    (0x48, 0x49),        # speculation control and prediction command
    (0x4e, 0x4f),        # PPIN control and PPIN
    (0x79, 0x79),        # microcode update trigger: VMX loads nothing
    (0x8b, 0x8b),        # microcode signature
    (0xbc, 0xbc),        # package power filtering
    (0xbf, 0xbf),        # prediction barrier option
    (0xc1, 0xca),        # general-purpose counters
    (0xcf, 0xcf),        # core capabilities
    (0xe1, 0xe1),        # UMWAIT control
    (0xe7, 0xe8),        # MPERF and APERF
    (0x10b, 0x10b),      # flush command
    (0x10f, 0x10f),      # TSX force abort
    (0x122, 0x123),      # TSX control and microcode option control
    (0x174, 0x176),      # SYSENTER
    (0x179, 0x17b),      # machine check globals
    (0x186, 0x18f),      # general-purpose counter event selects
    (0x195, 0x195),      # overclocking status
    (0x198, 0x19c),      # performance status and control, thermal
    (0x1a0, 0x1a0),      # miscellaneous enables
    (0x1b0, 0x1b2),      # energy-performance bias, package thermal
    (0x1c4, 0x1c5),      # extended feature disable
    (0x1d9, 0x1d9),      # debug control
    (0x277, 0x277),      # PAT
    (0x280, 0x29f),      # machine check bank control 2
    (0x309, 0x30f),      # fixed counters
    (0x345, 0x345),      # performance capabilities
    (0x38d, 0x392),      # fixed counter control, global performance control
    (0x3f1, 0x3f1),      # PEBS enable
    (0x400, 0x47f),      # machine check banks
    (0x4c1, 0x4ca),      # full-width counters
    (0x4d0, 0x4d0),      # local machine check control
    (0x560, 0x561),      # Intel PT output
    (0x568, 0x572),      # Intel PT triggers, control, status, CR3 match
    (0x580, 0x587),      # Intel PT address ranges
    (0x600, 0x600),      # debug store area
    (0x6a0, 0x6a0),      # user CET
    (0x6a2, 0x6a2),      # supervisor CET
    (0x6a4, 0x6a8),      # shadow stack pointers and table
    (0x6e0, 0x6e1),      # TSC deadline, supervisor protection keys
    (0x770, 0x777),      # HWP
    (0x990, 0x991),      # Key Locker copy and backup status
    (0xc81, 0xc83),      # RDT configuration
    (0xc88, 0xc89),      # RDT resource priority
    (0xc8d, 0xc8f),      # RDT monitoring and association
    (0xc90, 0xd8f),      # RDT allocation masks and throttling
    (0xd90, 0xd90),      # MPX supervisor configuration
    (0xd91, 0xd92),      # Key Locker copies
    (0xda0, 0xda0),      # XSS
    (0xdb0, 0xdb2),      # HDC
    (0xe00, 0xe01),      # RDT core bandwidth throttling
    (0x17d0, 0x17d2),    # hardware feedback interface
    (0x17d4, 0x17d4),    # hardware feedback thread configuration
    (0x17da, 0x17da),    # history reset
    (0x1900, 0x1901),    # general-purpose counters, configurations A and B
    (0x1904, 0x1905),
    (0x1908, 0x190a),
    (0x190c, 0x190e),
    (0x1910, 0x1912),
    (0x1914, 0x1916),
    (0x1918, 0x191a),
    (0x191c, 0x191e),
    (0x1920, 0x1922),
    (0x1924, 0x1926),
    (0x1980, 0x1980),    # fixed counters, configuration B
    (0x1982, 0x1982),
    (0x1984, 0x1984),
    (0x1986, 0x1986),
    (0x1988, 0x1988),
    (0x198a, 0x198a),
    (0x198c, 0x198c),
    (0x198e, 0x198e),
    (0x1990, 0x1990),
    (0x1994, 0x1994),
    (0x1998, 0x1998),
    (0x1b01, 0x1b01),    # data operand independent timing
    (0xc0000080, 0xc0000084),  # EFER, SYSCALL
    (0xc0000100, 0xc0000103),  # FS, GS, and kernel GS bases, TSC_AUX
]

# The tables of model-specific MSRs, by the processors SDM 325462-093 Vol. 4
# names for them, for those that can run the hypervisor: with unrestricted
# guest, from Westmere on, and among Intel Atom processors from Goldmont on, the
# first with VT-d. The common set of its section 2.17 is the processors its
# Table 2-1 names in its Table 2-39's title, where the section's own list leaves
# some out.
SKYLAKE_ON = ['2-20', '2-21', '2-25', '2-29', '2-35', '2-39']
MODELS = [
    (['06_25', '06_2C'], ['2-15', '2-16', '2-18']),
    (['06_2F'], ['2-15', '2-16', '2-19']),
    (['06_2A'], ['2-20', '2-21', '2-22']),
    (['06_2D'], ['2-20', '2-23', '2-24']),
    (['06_3A'], ['2-20', '2-21', '2-22', '2-25']),
    (['06_3E'], ['2-20', '2-24', '2-26', '2-27', '2-28']),
    (['06_3C', '06_46'], ['2-20', '2-21', '2-22', '2-29', '2-30']),
    (['06_45'], ['2-20', '2-21', '2-22', '2-29', '2-30', '2-31']),
    (['06_3F'], ['2-20', '2-29', '2-32', '2-33']),
    (['06_3D', '06_47'], ['2-20', '2-21', '2-22', '2-25', '2-29', '2-30', '2-34', '2-35']),
    (['06_56'], ['2-20', '2-29', '2-34', '2-36', '2-37']),
    (['06_4F'], ['2-20', '2-21', '2-29', '2-34', '2-36', '2-38']),
    (['06_4E', '06_5E'], SKYLAKE_ON + ['2-40']),
    (['06_8E', '06_9E'], SKYLAKE_ON + ['2-40', '2-41']),
    (['06_66'], SKYLAKE_ON + ['2-40', '2-42', '2-43']),
    (['06_A5', '06_A6', '06_A7', '06_B5', '06_C5', '06_C6'], SKYLAKE_ON),
    (['06_7E'], SKYLAKE_ON + ['2-44']),
    (['06_8C', '06_8D'], SKYLAKE_ON + ['2-44', '2-45']),
    (['06_97', '06_9A', '06_B7', '06_BA', '06_BF'],
     SKYLAKE_ON + ['2-44', '2-45', '2-46', '2-47', '2-48', '2-49']),
    (['06_55'], SKYLAKE_ON + ['2-50']),
    (['06_6A', '06_6C'], SKYLAKE_ON + ['2-51']),
    (['06_8F', '06_CF'], SKYLAKE_ON + ['2-52']),
    (['06_AA'], SKYLAKE_ON + ['2-53', '2-54', '2-55']),
    (['06_AD', '06_AE'], SKYLAKE_ON + ['2-56']),
    (['06_AF'], SKYLAKE_ON + ['2-57']),
    (['06_BD'], SKYLAKE_ON + ['2-58', '2-59', '2-60']),
    (['06_5C', '06_5F'], ['2-6', '2-12']),
    (['06_7A'], ['2-6', '2-12', '2-13']),
    (['06_86', '06_96', '06_9C'], ['2-6', '2-12', '2-13', '2-14']),
    (['06_B6', '06_BE'], ['2-6']),
]

# The kinds of model-specific MSR the guest gets: power and RAPL, turbo,
# thermal, C-states, performance monitoring with the legacy LBRs, split-lock
# and UC-lock disable, and CPUID faulting, whose #GP comes before the VM exit
# (SDM 325462-093 Vol. 3C, 28.1.1). Names are matched with digits folded to n.
KINDS = [
    ('power', r'MSR_(RAPL_POWER_UNIT|PKG_(POWER_LIMIT(_n)?|ENERGY_STATUS|PERF_STATUS|POWER_INFO)'
              r'|DRAM_(POWER_LIMIT|ENERGY_STATUS|PERF_STATUS|POWER_INFO)'
              r'|PPn_(POWER_LIMIT|ENERGY_STATUS|POLICY)'
              r'|PLATFORM_(ENERGY_COUNTER|ENERGY_STATUS|POWER_INFO|POWER_LIMIT'
              r'|RAPL_SOCKET_PERF_STATUS)'
              r'|PACKAGE_ENERGY_TIME_STATUS|VR_CURRENT_CONFIG|POWER_CTL|MISC_PWR_MGMT|PPERF'
              r'|CONFIG_TDP_(CONTROL|LEVELn|NOMINAL)|(CORE|GRAPHICS|RING)_PERF_LIMIT_REASONS'
              r'|TURBO_POWER_CURRENT_LIMIT|PKG_HDC_(CONFIG|DEEP_RESIDENCY|SHALLOW_RESIDENCY)'
              r'|CORE_HDC_RESIDENCY|PERF_STATUS)'),
    ('turbo', r'MSR_((PRIMARY_|SECONDARY_)?TURBO_RATIO_LIMIT(n|_CORES)?|TURBO_ACTIVATION_RATIO'
              r'|TURBO_GROUP_CORECNT|PLATFORM_INFO|UNCORE_RATIO_LIMIT|RING_RATIO_LIMIT)'),
    ('thermal', r'MSR_TEMPERATURE_TARGET'),
    ('C-states', r'MSR_((CORE|PKG)_Cn_RESIDENCY|MCn_RESIDENCY(_COUNTER)?|PKG_CST_CONFIG_CONTROL'
                 r'|(PMG_)?IO_CAPTURE_BASE|PKGCn_IRTL|PKGC_IRTLn|ANY_CORE_Cn|ANY_GFXE_Cn'
                 r'|CORE_GFXE_OVERLAP_Cn|WEIGHTED_CORE_Cn)'),
    ('perfmon', r'MSR_(OFFCORE_RSP_n|PEBS_(LD_LAT|FRONTEND|NUM_ALT|DATA_CFG)|PERF_METRICS'
                r'|PERF_GLOBAL_(OVF_CTRL|STATUS)|RELOAD_(FIXED_CTRx|PMCx)|SMI_COUNT|UNC_\w+'
                r'|UNCORE_(PERF_GLOBAL_\w+|PMCn|FIXED_CTRn|FIXED_CTR_CTRL|PERFEVTSELn'
                r'|ADDR_OPCODE_MATCH)|(\w+_)?PMON_\w+'
                r'|LBR_SELECT|LASTBRANCH_TOS|LASTBRANCH_n_(FROM|TO)_IP|LASTBRANCH_INFO_n'
                r'|LBR_INFO_n|LER_(FROM|TO)_LIP)'),
    ('bus locks', r'MSR_MEMORY_(CTRL|CONTROL)'),
    ('CPUID faulting', r'MSR_FEATURE_ENABLES'),
    ('architectural', r'MSR_PLATFORM_ID'),
]

ADDR = re.compile(r'^\s*Register Address: ([0-9A-F_]+)H(?:\s*[-–−]\s*([0-9A-F_]+)H)?'
                  r'(?:,\s*[0-9]+(?:\s*[-–−]\s*[0-9]+)?)?\s+(.*)$')
HEADING = re.compile(r'^\s*Table (2-\d+)\.\s')


def parse_tables(lines):
    """Every MSR of chapter 2's tables: {table: [(lo, hi, name)]}."""
    start = next(i for i, l in enumerate(lines)
                 if 'Table 2-2. IA-32 Architectural MSRs' in l and '. . .' not in l)
    p5 = next(i for i in range(start, len(lines))
              if 'Table 2-69. MSRs in the Pentium Processor' in lines[i]
              and '. . .' not in lines[i])
    end = next(i for i in range(p5, len(lines)) if lines[i].startswith('Numerics'))
    tables = {}
    table = None
    for line in lines[start:end]:
        m = HEADING.match(line)
        if m and '. . .' not in line:
            table = m.group(1)
            continue
        m = ADDR.match(line)
        if m and table:
            lo = int(m.group(1).replace('_', ''), 16)
            hi = int(m.group(2).replace('_', ''), 16) if m.group(2) else lo
            tables.setdefault(table, []).append((lo, hi, m.group(3).strip()))
    return tables


def kind(name):
    folded = re.sub(r'\d+', 'n', name.split()[0])
    for k, pattern in KINDS:
        if re.fullmatch(pattern, folded):
            return k
    return None


def ranges(addresses):
    out = []
    for a in sorted(addresses):
        if out and out[-1][1] + 1 == a:
            out[-1][1] = a
        else:
            out.append([a, a])
    return out


def label(names, lo, hi):
    # The names without their former ones.
    first = names.get(lo, hex(lo)).split()[0]
    return first if hi == lo else first + ' to ' + names.get(hi, hex(hi)).split()[0]


def main():
    sdm, output = sys.argv[1], sys.argv[2]
    report = len(sys.argv) > 3 and sys.argv[3] == 'report'
    lines = open(sdm, encoding='utf-8', errors='replace').read().split('\n')
    tables = parse_tables(lines)

    # The architectural names, some of which only model tables list, as
    # IA32_PBOPT_CTRL, which IA32_ARCH_CAPABILITIES[32] enumerates.
    names = {}
    for t, entries in sorted(tables.items(), key=lambda x: int(x[0].split('-')[1])):
        for lo, hi, name in entries:
            if t == '2-2' or name.startswith('IA32_'):
                for a in range(lo, hi + 1):
                    names.setdefault(a, name)
    arch = set()
    for lo, hi in INTEL_ARCH:
        for a in range(lo, hi + 1):
            assert a in names, hex(a)
        arch.update(range(lo, hi + 1))

    # A table's MSRs of the kinds the guest gets; a later table of a model's
    # list redefining one of them must not take it out.
    used = sorted({t for _, ts in MODELS for t in ts}, key=lambda t: int(t.split('-')[1]))
    passed = {}
    left_out = {}
    for t in used:
        passed[t] = set()
        for lo, hi, name in tables[t]:
            if name.startswith('IA32_') or name.startswith('Reserved') or name == 'N/A':
                continue
            k = kind(name)
            for a in range(lo, hi + 1):
                if k:
                    passed[t].add(a)
                else:
                    left_out.setdefault((t, name), set()).add(a)
    for _, ts in MODELS:
        for i, t in enumerate(ts):
            for later in ts[i + 1:]:
                hidden = {a for (lt, _), addrs in left_out.items() if lt == later for a in addrs}
                assert not passed[t] & hidden, (t, later, sorted(passed[t] & hidden))

    out = list(NOTICE)
    out += ['',
            '// Generated by tools/msr-table.py from the Intel SDM 325462-093, Vol. 4: the',
            '// MSRs the guest reaches as the processor has them.',
            '',
            '#include <stddef.h>',
            '#include <stdint.h>',
            '#include <virt/msr_table.h>',
            '',
            '// SDM 325462-093 Vol. 4, Table 2-2, as reviewed.',
            'const struct msr_range msr_intel_architectural[] = {']
    for lo, hi in ranges(arch):
        out.append(f'    {{{lo:#x}, {hi:#x}}}, // {label(names, lo, hi)}')
    out += ['};',
            'const size_t msr_intel_architectural_count =',
            '    sizeof(msr_intel_architectural) / sizeof(msr_intel_architectural[0]);']
    # Tables that give the guest nothing more are left out.
    used = [t for t in used if passed[t]]
    models = [(signatures, [t for t in ts if passed[t]]) for signatures, ts in MODELS]
    for t in used:
        kinds = sorted({kind(n) for _, _, n in tables[t] if not n.startswith('IA32_') and kind(n)})
        tnames = {}
        for lo, hi, name in tables[t]:
            for a in range(lo, hi + 1):
                tnames.setdefault(a, name)
        name = t.replace('-', '_')
        title = f'SDM 325462-093 Vol. 4, Table {t}: ' + ', '.join(kinds) + '.'
        out += [''] + textwrap.wrap(title, 100, initial_indent='// ', subsequent_indent='// ')
        out.append(f'static const struct msr_range ranges_{name}[] = {{')
        for lo, hi in ranges(passed[t]):
            out.append(f'    {{{lo:#x}, {hi:#x}}}, // {label(tnames, lo, hi)}')
        out += ['};',
                f'static const struct msr_ranges table_{name} = {{',
                f'    ranges_{name}, sizeof(ranges_{name}) / sizeof(ranges_{name}[0]),',
                '};']
    for i, (signatures, ts) in enumerate(models):
        heading = (', '.join(sig + 'H' for sig in signatures) + ': SDM 325462-093 Vol. 4, Tables '
                   + ', '.join(ts) + '.')
        out += [''] + textwrap.wrap(heading, 100, initial_indent='// ', subsequent_indent='// ')
        out.append(f'static const struct msr_ranges *const model_tables_{i}[] = {{')
        out += textwrap.wrap(', '.join('&table_' + t.replace('-', '_') for t in ts) + ',', 100,
                             initial_indent='    ', subsequent_indent='    ')
        out.append('};')
    out += ['', '// By display family and model.', 'const struct msr_model msr_intel_models[] = {']
    for i, (signatures, ts) in enumerate(models):
        for sig in signatures:
            family, model = (int(x, 16) for x in sig.split('_'))
            out.append(f'    {{{family:#x}, {model:#x}, model_tables_{i}, {len(ts)}}},')
    out += ['};',
            'const size_t msr_intel_model_count = '
            'sizeof(msr_intel_models) / sizeof(msr_intel_models[0]);',
            '']
    open(output, 'w').write('\n'.join(out))

    if report:
        order = sorted(left_out.items(), key=lambda x: (int(x[0][0].split('-')[1]), min(x[1])))
        for (t, name), addrs in order:
            print(f'left out: Table {t} {min(addrs):#x} {name}')


main()
