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

# Usage: msr-table.py <specifications> <output.c> [report]
#
# Generates src/virt/msr_table.c: the MSRs the guest reaches as the processor
# has them, from the directory of cached specifications. Those of Intel's
# architecture come from Table 2-2 of the Intel SDM 325462-093, Vol. 4, as
# reviewed below; the model-specific ones from its other tables, for the
# processors that can run the hypervisor, and only those of the kinds reviewed
# as safe to give the guest. AMD's come alike from Table A-1 of APM 24593 rev.
# 3.45 and from each family's BKDGs and PPRs. The texts are `pdftotext -layout`
# output of the documents. With report, it also lists every model-specific MSR
# it leaves out.

import os

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

SDM = 'intel-sdm-325462-093US.txt'
APM = 'amd-apm-40332-rev4.10.txt'

# AMD's architectural MSRs the guest gets: those of APM 24593 rev. 3.45, Table
# A-1, and those every family's documents give, as reviewed. Left out are those
# modules of the hypervisor handle (APIC_BASE, the MTRRs, PAT, XSS's writes,
# EFER, HWCR, SYSCFG, the IORRs, the tops of memory, VM_CR, VM_HSAVE_PA, the SMM
# and SVM keys and controls, the x2APIC range), and those of what the guest does
# not get: FRED, SMM, SVM's own, SEV and SNP, IBS, the watchdog timer, the LBR
# stack, the processor name string, the MCA interrupt configuration, and the L3
# range reservation.
AMD_ARCH = [
    (0x0, 0x1, 'load-store MCA address and status'),
    (0x10, 0x10, 'TSC'),
    (0x2a, 0x2a, 'cluster ID'),
    (0x3b, 0x3b, 'TSC adjust'),
    (0x48, 0x49, 'speculation control and prediction command'),
    (0x8b, 0x8b, 'patch level'),
    (0xe7, 0xe8, 'MPERF and APERF'),
    (0x10b, 0x10b, 'flush command'),
    (0x174, 0x176, 'SYSENTER'),
    (0x179, 0x17b, 'machine check globals'),
    (0x1d9, 0x1d9, 'debug control'),
    (0x1db, 0x1de, 'last branch and exception records'),
    (0x400, 0x47f, 'machine check banks'),
    (0x6a0, 0x6a0, 'user CET'),
    (0x6a2, 0x6a2, 'supervisor CET'),
    (0x6a4, 0x6a8, 'shadow stack pointers and table'),
    (0xc81, 0xc81, 'L3 QoS configuration'),
    (0xc8d, 0xc8f, 'QoS monitoring and association'),
    (0xc90, 0xd8f, 'L3 allocation masks'),
    (0xc0000081, 0xc0000084, 'SYSCALL'),
    (0xc00000e7, 0xc00000e9, 'read-only MPERF and APERF, instructions retired'),
    (0xc0000100, 0xc0000103, 'FS, GS, and kernel GS bases, TSC_AUX'),
    (0xc0000108, 0xc0000108, 'prefetch control'),
    (0xc0000200, 0xc00002ff, 'L3 QoS bandwidth controls'),
    (0xc0000300, 0xc0000303, 'global performance counter control and status'),
    (0xc00003fd, 0xc00003ff, 'assignable bandwidth monitoring'),
    (0xc0000400, 0xc0000401, 'bandwidth monitoring event configuration'),
    (0xc0000408, 0xc000040a, 'MC4 thresholds'),
    (0xc0000420, 0xc0000423, 'MCA query'),
    (0xc0002000, 0xc00023ff, 'MCAX banks'),
    (0xc0010000, 0xc0010007, 'legacy performance counters'),
    (0xc0010061, 0xc0010063, 'P-state limit, control, and status'),
    (0xc0010115, 0xc0010115, 'IGNNE'),
    (0xc0010140, 0xc0010141, 'OS visible workarounds'),
    (0xc0010200, 0xc001020b, 'core performance counters'),
    (0xc0010230, 0xc0010237, 'L2I or L3 performance counters'),
    (0xc0010240, 0xc0010247, 'northbridge or data fabric performance counters'),
    (0xc00102b0, 0xc00102b5, 'CPPC'),
    (0xc0011019, 0xc001101b, 'breakpoint address masks 1 to 3'),
    (0xc0011027, 0xc0011027, 'breakpoint address mask 0'),
]

# The series APM 24593 rev. 3.45, Table A-1, lists the first members of: the
# machine check banks (APM 24593 rev. 3.45, 9.3.2: 400h + 4i), the L3 allocation
# masks and bandwidth controls (APM 24593 rev. 3.45, 19.4: C90h + n, C000_0200h
# + n, and C000_0280h + n), and the MCAX banks (C000_2000h + 10h * i).
AMD_SERIES = [(0x400, 0x47f), (0xc90, 0xd8f), (0xc0000200, 0xc00002ff), (0xc0002000, 0xc00023ff)]

# The families that can run the hypervisor, with an IOMMU and nested paging,
# and their documents: Family 10h from revision D on, whose platforms are the
# first with an IOMMU (BKDG 31116 rev. 3.62), but neither Family 12h nor 14h.
# Hygon's 18h has none, and gets only the architectural MSRs.
AMD_FAMILIES = [
    (0x10, ['amd-bkdg-fam10h-31116-rev3.62']),
    (0x15, ['amd-bkdg-fam15h-m00h-0fh-42301-rev3.14', 'amd-bkdg-fam15h-m10h-1fh-42300-rev3.12',
            'amd-bkdg-fam15h-m30h-3fh-49125-rev3.06', 'amd-bkdg-fam15h-m60h-6fh-50742-rev3.05',
            'amd-bkdg-fam15h-m70h-7fh-55072-rev3.09']),
    (0x16, ['amd-bkdg-fam16h-m00h-0fh-48751-rev3.03', 'amd-bkdg-fam16h-m30h-3fh-52740-rev3.06']),
    (0x17, ['amd-ppr-54945-rev3.03', 'amd-ppr-55570-rev3.16', 'amd-ppr-55772-rev3.08',
            'amd-ppr-55803-rev0.93', 'amd-ppr-55922-rev3.06', 'amd-ppr-56176-rev3.06']),
    (0x19, ['amd-ppr-55898-rev0.50-vol%d' % i for i in range(1, 3)]
           + ['amd-ppr-55901-rev0.47-vol%d' % i for i in range(1, 7)]
           + ['amd-ppr-56214-rev3.05', 'amd-ppr-56569-rev3.03', 'amd-ppr-56713-rev3.05',
              'amd-ppr-57019-rev3.00']
           + ['amd-ppr-57228-rev0.45-vol%d' % i for i in range(1, 7)]),
    (0x1a, ['amd-ppr-57238-rev0.49-vol%d' % i for i in range(1, 8)]
           + ['amd-ppr-57883-rev0.51-vol%d' % i for i in range(1, 8)]
           + ['amd-ppr-57930-rev3.00']
           + ['amd-ppr-58264-rev0.45-vol%d' % i for i in range(1, 7)]
           + ['amd-ppr-58655-rev0.42-vol%d' % i for i in range(1, 7)]),
]

# The kinds of model-specific MSR the guest gets, as for Intel's: power and
# RAPL, thermal, C-states, performance monitoring, and what identifies the
# processor; the machine check masks of the banks the guest has; and the
# microarchitecture configurations, through which errata workarounds and
# mitigations act. Names are those of PPRs and the abbreviations BKDGs give,
# else BKDGs' descriptions, matched with digits folded to n.
AMD_KINDS = [
    ('power', r'PStateDef|RAPL_PWR_UNIT|CORE_ENERGY_STAT|PKG_ENERGY_STAT|LnRAPLPowerUnitn?'
              r'|LnPackageEnergyStatusn?|CU_PTSC|P-[Ss]tate \[n:n\]( Registers)?'
              r'|COFVID (Control|Status)( Register)?|SBI P-state Limit( Register)?|APML TDP Limit'
              r'|Processor Power in TDP|Power Averaging Period|DRAM Controller Command Throttle'
              r'|(Max )?Compute Unit Power Accumulator'
              r'|Frequency Sensitivity Feedback Monitor (Actual Count|Reference)( n)?'
              r'|Processor Feedback Constants( n)?'),
    ('thermal', r'HTC'),
    ('C-states', r'CStateBaseAddr|C-state Base Address|Interrupt Pending( and CMP-Halt Register)?'),
    ('perfmon', r'ChLnPmc(Cfg)?|DF_PERF_CT[LR]|UMC_PerfMon(Ctl|Cntr)'),
    ('identification', r'EBL_CR_POWERON|PATCH_LEVEL|PPIN(_CTL)?|TSC_ADJUST|BIST Results'),
    ('machine checks', r'MCA_CTL_MASK_\w+|MCn_CTL_MASK|MCA_(ADDR|STATUS)_LS'
                       r'|(\w+ )?Machine Check Control Mask( Registers)?'
                       r'|Load-Store MCA (Address|Status)( Register)?'),
    ('configuration', r'(LS|BU|CU)_CFGn?|IC_CFG|DC_CFG|TW_CFG|FP_CFG|DE_CFG|EX_CFG|LnI_CFG'
                      r'|CU_PFTCFG|CU_CBBCFG'),
]

# The configuration MSRs AMD's other documents give, by family: DE_CFG, whose
# LFENCE serialisation every family from 10h has and future ones are to have
# ("Software Techniques for Managing Speculation on AMD Processors" 70586 rev.
# 5.09.23, G-2); LS_CFG, Family 17h's speculative store bypass disable (124441
# rev. 5.21.18); EX_CFG, which system software sets for erratum 1100 of Family
# 17h models 00h-0Fh (revision guide 55449 rev. 1.21); and those AMD's errata
# fixes and mitigations in Linux 7.3 set (arch/x86/kernel/cpu/amd.c, bugs.c):
# FP_CFG's Zen 1 denormal fix, BP_CFG (C001_102E) on Zen 2 to 5, and Zen 2's
# C001_10E3.
AMD_EXTRA = [
    (0xc0011029, 'DE_CFG', [0x10, 0x15, 0x16, 0x17, 0x19, 0x1a]),
    (0xc0011020, 'LS_CFG', [0x17]),
    (0xc0011028, 'FP_CFG', [0x17]),
    (0xc001102c, 'EX_CFG', [0x17]),
    (0xc001102e, 'BP_CFG', [0x17, 0x19, 0x1a]),
    (0xc00110e3, 'SPECTRAL_CHICKEN', [0x17]),
]

# What the hypervisor's modules handle, which no table needs (msr.c): NB_CFG,
# the configuration space window, the CPUID overrides, and the patch loader.
AMD_HANDLED = r'NB_CFGn?|MmioCfgBaseAddr|MMIO Configuration Base Address( Register)?|PATCH_LOADER' \
              r'|CPUID_(n_Features|PWR_THERM|Features|ExtFeatures)|(Extended )?CPUID Features.*' \
              r'|Thermal and Power Management CPUID Features.*'

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


def intel(specs):
    """Intel's part of the output, and what its report lists."""
    lines = open(os.path.join(specs, SDM), encoding='utf-8', errors='replace').read().split('\n')
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

    out = ['// SDM 325462-093 Vol. 4, Table 2-2, as reviewed.',
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
            'sizeof(msr_intel_models) / sizeof(msr_intel_models[0]);']
    order = sorted(left_out.items(), key=lambda x: (int(x[0][0].split('-')[1]), min(x[1])))
    left = [f'left out: Table {t} {min(addrs):#x} {name}' for (t, name), addrs in order]
    return out, left


AMD_ADDR = (r'MSR([0-9A-F]{4})_([0-9A-F]*)((?:\[[0-9A-F,:. ]+\]|\[[0-9A-F]\.\.\.[0-9A-F]\])?)'
            r'([0-9A-F]*)')
PPR_HEADING = re.compile(r'^\s*' + AMD_ADDR + r'(?:\.\.\.MSR[0-9A-F]{4}_[0-9A-F]{4})?'
                         r'\s*(?:\[((?:[^\[\]]|\[[^\]]*\])+)\])?'
                         r'\s*(\(([A-Za-z0-9_]+::)+[A-Za-z0-9_\[\]]+\))?\s*$')
BKDG_ENTRY = re.compile(AMD_ADDR + r'\s*:\s+')


def amd_expand(hi, prefix, inner, suffix):
    """The addresses of MSRC001_00[18,16], MSRC001_00[35:30], and MSRC001_006[4...B]."""
    lows = [prefix + suffix]
    if inner:
        lows = []
        for part in inner[1:-1].replace(' ', '').split(','):
            if '...' in part or ':' in part:
                a, b = part.split('...') if '...' in part else part.split(':')[::-1]
                lows += [prefix + format(v, f'0{len(a)}X') + suffix
                         for v in range(int(a, 16), int(b, 16) + 1)]
            else:
                lows.append(prefix + part + suffix)
    return [int(hi + low, 16) for low in lows if len(low) == 4]


def ppr_msrs(lines):
    """A PPR's MSR headings, a namespace path wrapped to the next line taken in:
    (address, description, name), with the name the path's last part."""
    out = []
    for i, line in enumerate(lines):
        if not re.match(r'^\s*MSR[0-9A-F]{4}_', line):
            continue
        text = line.rstrip()
        if i + 1 < len(lines) and re.match(r'^\s*\(([A-Za-z0-9_]+::)+', lines[i + 1]) \
           and '(' not in text:
            text += ' ' + lines[i + 1].strip()
        m = PPR_HEADING.match(text)
        if not m or not m.group(6):
            continue
        desc = m.group(5)
        if desc is None:
            # The description follows the access line.
            for j in range(i + 1, min(i + 5, len(lines))):
                t = lines[j].strip()
                if t and not t.startswith(('Read', 'Write', 'Reset', '(')) and '::' not in t:
                    desc = t
                    break
        name = m.group(6)[1:-1].split('::')[-1]
        for a in amd_expand(m.group(1), m.group(2), m.group(3), m.group(4)):
            out.append((a, (desc or '').strip(), name))
    return out


def bkdg_msrs(lines):
    """A BKDG's register index, two columns of "<page> MSRxxxx_xxxx:
    <description>": (address, description, the abbreviation it ends with)."""
    out = []
    for line in lines:
        starts = list(BKDG_ENTRY.finditer(line))
        for k, m in enumerate(starts):
            end = starts[k + 1].start() if k + 1 < len(starts) else len(line)
            desc = re.sub(r'\s{2,}.*', '', line[m.end():end].strip())
            desc = re.sub(r'(\s+\d+)+$', '', desc)
            if re.match(r'^[0-9]', desc) or desc.startswith(('Reserved', 'Updated')):
                continue
            short = re.search(r'\(([A-Za-z0-9_\[\]:]+)\)$', desc)
            for a in amd_expand(m.group(1), m.group(2), m.group(3), m.group(4)):
                out.append((a, desc, short.group(1) if short else ''))
    return out


def apm_table_a1(lines):
    """The addresses Table A-1 lists."""
    start = next(i for i, l in enumerate(lines)
                 if l.strip() == 'Table A-1. MSRs of the AMD64 Architecture')
    end = next(i for i in range(start, len(lines)) if lines[i].strip() == 'A.2')
    addr = re.compile(r'^([0-9A-F]{4}(?:_[0-9A-F]{4})?)h(\+n| \+)?\s*(.*)$')
    return {int(m.group(1).replace('_', ''), 16)
            for m in (addr.match(l.strip()) for l in lines[start:end]) if m}


def amd_kind(desc, name):
    for text in (name, desc):
        folded = re.sub(r'\d+', 'n', text)
        for k, pattern in AMD_KINDS:
            if folded and re.fullmatch(pattern, folded):
                return k
    return None


def amd_documents(docs):
    """Cites a family's documents, as "BKDGs 42301 rev. 3.14 and 42300 rev. 3.12"."""
    cited = []
    for doc in docs:
        m = re.fullmatch(r'amd-(bkdg|ppr)-.*?(\d{5})-rev([\d.]+)(?:-vol\d+)?', doc)
        if m.groups() not in cited:
            cited.append(m.groups())
    assert len({kind for kind, _, _ in cited}) == 1, docs
    names = [f'{number} rev. {revision}' for _, number, revision in cited]
    kind = cited[0][0].upper()
    if len(names) == 1:
        return f'{kind} {names[0]}'
    listed = ', '.join(names[:-1]) + (',' if len(names) > 2 else '') + ' and ' + names[-1]
    return f'{kind}s {listed}'


def amd(specs):
    """AMD's part of the output, and what its report lists."""
    def read(name):
        return open(os.path.join(specs, name + '.txt'), encoding='utf-8',
                    errors='replace').read().split('\n')

    table_a1 = apm_table_a1(read(APM[:-4]))
    documented = {}
    families = {}
    for family, docs in AMD_FAMILIES:
        families[family] = []
        for doc in docs:
            lines = read(doc)
            msrs = bkdg_msrs(lines) if doc.startswith('amd-bkdg-') else ppr_msrs(lines)
            families[family] += msrs
            for a, desc, name in msrs:
                documented.setdefault(a, name or desc)
    arch = set()
    series = {a for lo, hi in AMD_SERIES for a in range(lo, hi + 1)}
    for lo, hi, _ in AMD_ARCH:
        for a in range(lo, hi + 1):
            assert a in table_a1 or a in series or a in documented, hex(a)
        arch.update(range(lo, hi + 1))
    # The rest of what APM 24593 rev. 3.45, Table A-1, its series, and the
    # x2APIC range cover is architectural: handled, or what the guest does not
    # get.
    architectural = table_a1 | series | set(range(0x800, 0x900))

    out = ['', '// APM 24593 rev. 3.45, Table A-1, and what every family has, as reviewed.',
           'const struct msr_range msr_amd_architectural[] = {']
    for lo, hi, what in AMD_ARCH:
        out.append(f'    {{{lo:#x}, {hi:#x}}}, // {what}')
    out += ['};',
            'const size_t msr_amd_architectural_count =',
            '    sizeof(msr_amd_architectural) / sizeof(msr_amd_architectural[0]);']
    left = []
    for family, docs in AMD_FAMILIES:
        passed = {}
        kinds = set()
        left_out = {}
        for a, desc, name in families[family]:
            if a in arch or a in architectural:
                continue
            k = amd_kind(desc, name)
            if k:
                passed.setdefault(a, name or re.sub(r'( Registers?)?( \[\d+:\d+\])?$', '', desc))
                kinds.add(k)
            elif not re.fullmatch(AMD_HANDLED, re.sub(r'\d+', 'n', name or desc)) \
                 and not re.fullmatch(AMD_HANDLED, desc):
                left_out.setdefault(a, set()).add(name or desc)
        for a, name, fams in AMD_EXTRA:
            if family in fams:
                passed.setdefault(a, name)
                kinds.add('configuration')
        # No document of the family may call an MSR it passes something else.
        clash = set(passed) & set(left_out)
        assert not clash, (hex(family), [(hex(a), left_out[a]) for a in sorted(clash)])
        title = f'Family {family:X}h, {amd_documents(docs)}: ' + ', '.join(sorted(kinds)) + '.'
        out += [''] + textwrap.wrap(title, 100, initial_indent='// ', subsequent_indent='// ')
        out.append(f'static const struct msr_range family_{family:x}h[] = {{')
        for lo, hi in ranges(passed):
            # A range whose names do not fit goes in runs of one name.
            line = f'    {{{lo:#x}, {hi:#x}}}, // {passed[lo]} to {passed[hi]}'
            runs = [[lo, hi]]
            if len(line) > 100:
                runs = []
                for a in range(lo, hi + 1):
                    if runs and passed[runs[-1][0]] == passed[a]:
                        runs[-1][1] = a
                    else:
                        runs.append([a, a])
            for rlo, rhi in runs:
                what = passed[rlo]
                if passed[rhi] != what:
                    what += ' to ' + passed[rhi]
                out.append(f'    {{{rlo:#x}, {rhi:#x}}}, // {what}')
        out.append('};')
        left += [f'left out: Family {family:X}h {a:#x} {", ".join(sorted(n))}'
                 for a, n in sorted(left_out.items())]
    out += ['', '// By display family.', 'const struct msr_family msr_amd_families[] = {']
    for family, _ in AMD_FAMILIES:
        out.append(f'    {{{family:#x}, family_{family:x}h, '
                   f'sizeof(family_{family:x}h) / sizeof(family_{family:x}h[0])}},')
    out += ['};',
            'const size_t msr_amd_family_count = '
            'sizeof(msr_amd_families) / sizeof(msr_amd_families[0]);']
    return out, left


def main():
    specs, output = sys.argv[1], sys.argv[2]
    report = len(sys.argv) > 3 and sys.argv[3] == 'report'
    intel_out, intel_left = intel(specs)
    amd_out, amd_left = amd(specs)
    out = list(NOTICE)
    out += ['',
            '// Generated by tools/msr-table.py from the Intel SDM 325462-093, Vol. 4, and',
            '// from APM 24593 rev. 3.45 and the BKDGs and PPRs each family names: the MSRs',
            '// the guest reaches as the processor has them.',
            '',
            '#include <stddef.h>',
            '#include <stdint.h>',
            '#include <virt/msr_table.h>',
            '']
    out += intel_out + amd_out + ['']
    open(output, 'w').write('\n'.join(out))
    if report:
        for line in intel_left + amd_left:
            print(line)


main()
