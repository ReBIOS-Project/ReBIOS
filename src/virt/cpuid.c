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

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/x86.h>
#include <virt/cpuid.h>
#include <virt/nvmx.h>
#include <virt/vcpu.h>
#include <virt/virt.h>
#include <virt/vlapic.h>
#include <virt/vmx.h>

#define SUBLEAF_ALL UINT32_MAX

// The bits of a leaf's EAX, EBX, ECX, and EDX the guest gets as the processor
// has them; a leaf with no entry, or a subleaf with none, reads as zeros, as
// one the processor does not have.
struct leaf_bits {
    uint32_t leaf;
    uint32_t subleaf;
    uint32_t keep[4];
};

// SDM 325462-093 Vol. 1, chapter 21.
static const struct leaf_bits intel_leaves[] = {
    // EBX vendor; ECX vendor; EDX vendor.
    {0x0, SUBLEAF_ALL, {0x00000000, 0xffffffff, 0xffffffff, 0xffffffff}},
    // EAX stepping, model, family, type, extended model, extended family; EBX brand index, CLFLUSH
    // size, IDs, initial APIC ID; ECX SSE3, PCLMULQDQ, DTES64, MONITOR, DS-CPL, EIST, TM2, SSSE3,
    // CNXT-ID, FMA, CMPXCHG16B, xTPR, PDCM, PCID, SSE4.1, SSE4.2, x2APIC, MOVBE, POPCNT,
    // TSC-deadline, AESNI, XSAVE, AVX, F16C, RDRAND; EDX FPU, VME, DE, PSE, TSC, MSR, PAE, MCE,
    // CX8, SEP, MTRR, PGE, MCA, CMOV, PAT, PSE-36, PSN, CLFSH, DS, ACPI, MMX, FXSR, SSE, SSE2, SS,
    // HTT, TM, PBE.
    {0x1, SUBLEAF_ALL, {0x0fff3fff, 0xffffffff, 0x77faf79f, 0xbfeff9ff}},
    // EAX descriptors; EBX descriptors; ECX descriptors; EDX descriptors.
    {0x2, SUBLEAF_ALL, {0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff}},
    // ECX serial number; EDX serial number.
    {0x3, SUBLEAF_ALL, {0x00000000, 0x00000000, 0xffffffff, 0xffffffff}},
    // EAX type, level, self-initialising, fully associative, IDs sharing, core IDs; EBX line size,
    // partitions, ways; ECX sets; EDX WBINVD scope, inclusive, complex indexing.
    {0x4, SUBLEAF_ALL, {0xffffc3ff, 0xffffffff, 0xffffffff, 0x00000007}},
    // EAX smallest line; EBX largest line; ECX extensions, interrupt break event, monitorless
    // MWAIT; EDX C sub-states.
    {0x5, SUBLEAF_ALL, {0x0000ffff, 0x0000ffff, 0x0000000b, 0xffffffff}},
    // EAX DTS, turbo, ARAT, PLN, ECMD, PTM, HWP, HWP interrupt, HWP activity window, HWP EPP, HWP
    // package request, HDC, turbo max 3.0, HWP capabilities, HWP PECI override, flexible HWP, HWP
    // fast access, HFI, HWP ignore idle, HWP_CTL, Thread Director; EBX DTS thresholds; ECX
    // APERF/MPERF, EPB, Thread Director classes; EDX HFI capabilities, HFI table size, HFI row.
    {0x6, SUBLEAF_ALL, {0x00dfeff7, 0x0000000f, 0x0000ff09, 0xffff0fff}},
    // EAX last subleaf; EBX FSGSBASE, TSC_ADJUST, BMI1, HLE, AVX2, FDP_EXCPTN_ONLY, SMEP, BMI2,
    // ERMS, INVPCID, RTM, RDT-M, FCS/FDS deprecation, MPX, RDT-A, AVX512F, AVX512DQ, RDSEED, ADX,
    // SMAP, AVX512_IFMA, CLFLUSHOPT, CLWB, Intel PT, AVX512PF, AVX512ER, AVX512CD, SHA, AVX512BW,
    // AVX512VL; ECX PREFETCHWT1, AVX512_VBMI, UMIP, PKU, WAITPKG, AVX512_VBMI2, CET_SS, GFNI, VAES,
    // VPCLMULQDQ, AVX512_VNNI, AVX512_BITALG, AVX512_VPOPCNTDQ, LA57, MAWAU, RDPID, Key Locker, bus
    // lock detect, CLDEMOTE, MOVDIRI, MOVDIR64B, PKS; EDX AVX512_4VNNIW, AVX512_4FMAPS, FSRM,
    // AVX512_VP2INTERSECT, MCU_OPT_CTRL, MD_CLEAR, RTM_ALWAYS_ABORT, RTM_FORCE_ABORT, SERIALIZE,
    // hybrid, TSXLDTRK, arch LBRs, CET_IBT, AMX_BF16, AVX512_FP16, AMX_TILE, AMX_INT8, IBRS/IBPB,
    // STIBP, L1D_FLUSH, ARCH_CAPABILITIES, CORE_CAPABILITIES, SSBD.
    {0x7, 0, {0xffffffff, 0xffbffffb, 0x9bff5fef, 0xffd9ef1c}},
    // EAX SHA512, SM3, SM4, AVX_VNNI, AVX512_BF16, LASS, CMPCCXADD, ARCH_PERFMON_EXT, fast
    // zero-length MOVSB, fast short STOSB, fast short CMPSB/SCASB, LKGS, WRMSRNS, AMX_FP16, HRESET,
    // AVX_IFMA, LAM; EBX PPIN, CPUIDMAXVAL_LIM_RMV; ECX RDT-M asymmetric, RDT-A asymmetric; EDX
    // AVX_VNNI_INT8, AVX_NE_CONVERT, AMX_COMPLEX, AVX_VNNI_INT16, PREFETCHI, CET_SSS, AVX10, MWAIT.
    {0x7, 1, {0x04ec1df7, 0x00000009, 0x00000003, 0x008c4530}},
    // EDX PSFD, IPRED_CTRL, RRSBA_CTRL, DDPD_U, BHI_CTRL, MCDT_NO, UC-lock disable,
    // MONITOR_MITG_NO.
    {0x7, 2, {0x00000000, 0x00000000, 0x00000000, 0x000000ff}},
    // EAX version, counters, width, events; EBX events missing; ECX fixed counters; EDX fixed
    // counters, width, AnyThread deprecation, slots.
    {0xa, SUBLEAF_ALL, {0xffffffff, 0x00001fff, 0xffffffff, 0x000f9fff}},
    // EAX shift; EBX processors; ECX level, type; EDX x2APIC ID.
    {0xb, SUBLEAF_ALL, {0x0000001f, 0x0000ffff, 0x0000ffff, 0xffffffff}},
    // EAX x87, SSE, AVX, BNDREGS, BNDCSR, opmask, ZMM_Hi256, Hi16_ZMM, PKRU, TILECFG, TILEDATA; EBX
    // size enabled; ECX size supported.
    {0xd, 0, {0x000602ff, 0xffffffff, 0xffffffff, 0x00000000}},
    // EAX XSAVEOPT, XSAVEC, XGETBV1, XSAVES, XFD; EBX size enabled; ECX PT, CET_U, CET_S, HDC, LBR,
    // HWP.
    {0xd, 1, {0x0000001f, 0xffffffff, 0x0001b900, 0x00000000}},
    // EBX last RMID; EDX L3 monitoring.
    {0xf, 0, {0x00000000, 0xffffffff, 0x00000000, 0x00000002}},
    // EAX counter width, overflow, I/O RDT CMT, I/O RDT MBM; EBX factor; ECX last RMID; EDX
    // occupancy, total bandwidth, local bandwidth.
    {0xf, 1, {0x000007ff, 0xffffffff, 0xffffffff, 0x00000007}},
    // EBX L3 CAT, L2 CAT, MBA, CBA, resource priority.
    {0x10, 0, {0x00000000, 0x0000006e, 0x00000000, 0x00000000}},
    // EAX mask length; EBX contention; ECX non-CPU, CDP, non-contiguous; EDX last CLOS.
    {0x10, 1, {0x0000001f, 0xffffffff, 0x0000000e, 0x0000ffff}},
    // EAX mask length; EBX contention; ECX CDP, non-contiguous; EDX last CLOS.
    {0x10, 2, {0x0000001f, 0xffffffff, 0x0000000c, 0x0000ffff}},
    // EAX maximum throttling; ECX per thread, linear; EDX last CLOS.
    {0x10, 3, {0x00000fff, 0x00000000, 0x00000005, 0x0000ffff}},
    // EAX levels, scope; ECX linear; EDX last CLOS.
    {0x10, 5, {0x00000fff, 0x00000000, 0x00000008, 0x0000ffff}},
    // EAX thread enable, package enable.
    {0x10, 6, {0x00000003, 0x00000000, 0x00000000, 0x00000000}},
    // EAX last subleaf; EBX capabilities; ECX output schemes, LIP.
    {0x14, 0, {0xffffffff, 0x000003ff, 0x8000000f, 0x00000000}},
    // EAX ranges, triggers, MTC rates; EBX thresholds, PSB rates; ECX ICNT, trigger pause, trigger
    // DR match.
    {0x14, 1, {0xffff0707, 0xffffffff, 0x00008003, 0x00000000}},
    // EAX denominator; EBX numerator; ECX crystal.
    {0x15, SUBLEAF_ALL, {0xffffffff, 0xffffffff, 0xffffffff, 0x00000000}},
    // EAX base MHz; EBX maximum MHz; ECX bus MHz.
    {0x16, SUBLEAF_ALL, {0x0000ffff, 0x0000ffff, 0x0000ffff, 0x00000000}},
    // EAX last index; EBX vendor, scheme; ECX project; EDX stepping.
    {0x17, 0, {0xffffffff, 0x0001ffff, 0xffffffff, 0xffffffff}},
    // EAX brand; EBX brand; ECX brand; EDX brand.
    {0x17, 1, {0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff}},
    // EAX brand; EBX brand; ECX brand; EDX brand.
    {0x17, 2, {0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff}},
    // EAX brand; EBX brand; ECX brand; EDX brand.
    {0x17, 3, {0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff}},
    // EBX page sizes, partitioning, ways; ECX sets; EDX type, level, fully associative, IDs
    // sharing.
    {0x18, SUBLEAF_ALL, {0x00000000, 0xffff070f, 0xffffffff, 0x03ffc1ff}},
    // EAX last subleaf; EDX type.
    {0x18, 0, {0xffffffff, 0x00000000, 0x00000000, 0x0000001f}},
    // EAX CPL0 only, no encrypt, no decrypt; EBX AESKLE, wide, backup; ECX no backup, random IWKey.
    {0x19, SUBLEAF_ALL, {0x00000007, 0x00000015, 0x00000003, 0x00000000}},
    // EAX native model, core type.
    {0x1a, SUBLEAF_ALL, {0xffffffff, 0x00000000, 0x00000000, 0x00000000}},
    // EAX depths, deep C-state reset, LIP; EBX CPL filter, branch filter, call stack; ECX
    // mispredict, timed, branch type, events.
    {0x1c, SUBLEAF_ALL, {0xc00000ff, 0x00000007, 0x000f0007, 0x00000000}},
    // EAX last palette.
    {0x1d, 0, {0xffffffff, 0x00000000, 0x00000000, 0x00000000}},
    // EAX tile bytes; EBX row bytes, names; ECX rows.
    {0x1d, 1, {0xffffffff, 0xffffffff, 0x0000ffff, 0x00000000}},
    // EBX TMUL K, TMUL N.
    {0x1e, 0, {0x00000000, 0x00ffffff, 0x00000000, 0x00000000}},
    // EAX shift; EBX processors; ECX level, type; EDX x2APIC ID.
    {0x1f, SUBLEAF_ALL, {0x0000001f, 0x0000ffff, 0x0000ffff, 0xffffffff}},
    // EAX last subleaf; EBX Thread Director HRESET.
    {0x20, 0, {0xffffffff, 0x00000001, 0x00000000, 0x00000000}},
    // EAX subleaves; EBX UnitMask2, EQ; ECX slots.
    {0x23, 0, {0x0000003f, 0x00000003, 0x000000ff, 0x00000000}},
    // EAX counters; EBX fixed counters.
    {0x23, 1, {0xffffffff, 0xffffffff, 0x00000000, 0x00000000}},
    // EAX reload; EBX reload; ECX reload; EDX reload.
    {0x23, 2, {0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff}},
    // EAX events.
    {0x23, 3, {0x00001fff, 0x00000000, 0x00000000, 0x00000000}},
    // EBX allow in record, counters, LBR, XER, GPR, AUX.
    {0x23, 4, {0x00000000, 0x60ff03f8, 0x00000000, 0x00000000}},
    // EAX PEBS counters; EBX PEBS counters; ECX PEBS counters; EDX PEBS counters.
    {0x23, 5, {0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff}},
    // EAX last subleaf; EBX version, reserved at 1 (AVX10.2 361050-007 rev. 7.0, Table 3.1).
    {0x24, 0, {0xffffffff, 0x000700ff, 0x00000000, 0x00000000}},
    // ECX AVX10_V1_AUX (AVX10.2 361050-007 rev. 7.0, 3.1.2).
    {0x24, 1, {0x00000000, 0x00000000, 0x00000004, 0x00000000}},
    // EBX last RMID; EDX L3 monitoring.
    {0x27, 0, {0x00000000, 0xffffffff, 0x00000000, 0x00000002}},
    // EAX counter width, overflow, I/O RDT CMT, I/O RDT MBM; EBX factor; ECX last RMID; EDX
    // occupancy, total bandwidth, local bandwidth.
    {0x27, 1, {0x000007ff, 0xffffffff, 0xffffffff, 0x00000007}},
    // EBX L3 CAT, L2 CAT, MBA, CBA, resource priority.
    {0x28, 0, {0x00000000, 0x0000006e, 0x00000000, 0x00000000}},
    // EAX mask length; EBX contention; ECX non-CPU, CDP, non-contiguous; EDX last CLOS.
    {0x28, 1, {0x0000001f, 0xffffffff, 0x0000000e, 0x0000ffff}},
    // EAX mask length; EBX map; ECX CDP, non-contiguous; EDX last CLOS.
    {0x28, 2, {0x0000001f, 0xffffffff, 0x0000000c, 0x0000ffff}},
    // EAX maximum throttling; ECX per thread, linear; EDX last CLOS.
    {0x28, 3, {0x00000fff, 0x00000000, 0x00000005, 0x0000ffff}},
    // EAX levels, scope; ECX linear; EDX last CLOS.
    {0x28, 5, {0x00000fff, 0x00000000, 0x00000008, 0x0000ffff}},
    // EAX thread enable, package enable.
    {0x28, 6, {0x00000003, 0x00000000, 0x00000000, 0x00000000}},
    // nothing.
    {0x80000000, SUBLEAF_ALL, {0x00000000, 0x00000000, 0x00000000, 0x00000000}},
    // ECX LAHF/SAHF, LZCNT, PREFETCHW; EDX SYSCALL, NX, 1 GiB pages, RDTSCP, Intel 64.
    {0x80000001, SUBLEAF_ALL, {0x00000000, 0x00000000, 0x00000121, 0x2c100800}},
    // EAX brand; EBX brand; ECX brand; EDX brand.
    {0x80000002, SUBLEAF_ALL, {0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff}},
    // EAX brand; EBX brand; ECX brand; EDX brand.
    {0x80000003, SUBLEAF_ALL, {0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff}},
    // EAX brand; EBX brand; ECX brand; EDX brand.
    {0x80000004, SUBLEAF_ALL, {0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff}},
    // ECX line size, associativity, size.
    {0x80000006, SUBLEAF_ALL, {0x00000000, 0x00000000, 0xfffff0ff, 0x00000000}},
    // EDX invariant TSC.
    {0x80000007, SUBLEAF_ALL, {0x00000000, 0x00000000, 0x00000000, 0x00000100}},
    // EAX physical bits, linear bits; EBX WBNOINVD.
    {0x80000008, SUBLEAF_ALL, {0x0000ffff, 0x00000200, 0x00000000, 0x00000000}},
};

// APM 24594 rev. 3.38, Appendix E.
static const struct leaf_bits amd_leaves[] = {
    // EBX vendor; ECX vendor; EDX vendor.
    {0x0, SUBLEAF_ALL, {0x00000000, 0xffffffff, 0xffffffff, 0xffffffff}},
    // EAX stepping, base model, base family, extended model, extended family; EBX brand ID, CLFLUSH
    // size, processor count, initial APIC ID; ECX SSE3, PCLMULQDQ, MONITOR, SSSE3, FMA, CMPXCHG16B,
    // SSE4.1, SSE4.2, x2APIC, MOVBE, POPCNT, AES, XSAVE, AVX, F16C, RDRAND; EDX FPU, VME, DE, PSE,
    // TSC, MSR, PAE, MCE, CMPXCHG8B, SYSENTER, MTRR, PGE, MCA, CMOV, PAT, PSE36, CLFSH, MMX, FXSR,
    // SSE, SSE2, HTT.
    {0x1, SUBLEAF_ALL, {0x0fff0fff, 0xffffffff, 0x76f8320b, 0x178bf9ff}},
    // EAX smallest line; EBX largest line; ECX EMX, IBE.
    {0x5, SUBLEAF_ALL, {0x0000ffff, 0x0000ffff, 0x00000003, 0x00000000}},
    // EAX ARAT; ECX EffFreq.
    {0x6, SUBLEAF_ALL, {0x00000004, 0x00000000, 0x00000001, 0x00000000}},
    // EAX last subleaf; EBX FSGSBASE, TSC_ADJUST, BMI1, AVX2, SMEP, BMI2, ERMS, INVPCID, PQM, PQE,
    // AVX512F, AVX512DQ, RDSEED, ADX, SMAP, AVX512_IFMA, CLFLUSHOPT, CLWB, AVX512CD, SHA, AVX512BW,
    // AVX512VL; ECX AVX512_VBMI, UMIP, PKU, AVX512_VBMI2, CET_SS, GFNI, VAES, VPCLMULQDQ,
    // AVX512_VNNI, AVX512_BITALG, AVX512_VPOPCNTDQ, LA57, RDPID, bus lock trap, MOVDIRI, MOVDIR64B;
    // EDX FSRM, AVX512_FP16, L1D flush (FSRM as SDM 325462-093 Vol. 1, chapter 21 defines it).
    {0x7, 0, {0xffffffff, 0xf1bf97ab, 0x19415fce, 0x10800010}},
    // EAX AVX_VNNI, AVX512_BF16, AVX_IFMA; EDX AVX_VNNI_INT8, AVX_NE_CONVERT.
    {0x7, 1, {0x00800030, 0x00000000, 0x00000000, 0x00000030}},
    // EAX shift; EBX processors; ECX level, type; EDX x2APIC ID.
    {0xb, SUBLEAF_ALL, {0x0000001f, 0x0000ffff, 0x0000ffff, 0xffffffff}},
    // EAX x87, SSE, AVX, opmask, ZMM_Hi256, Hi16_ZMM, PKRU; EBX size enabled; ECX size supported.
    {0xd, 0, {0x000002e7, 0xffffffff, 0xffffffff, 0x00000000}},
    // EAX XSAVEOPT, XSAVEC, XGETBV, XSAVES; EBX size enabled; ECX CET_U, CET_S.
    {0xd, 1, {0x0000000f, 0xffffffff, 0x00001800, 0x00000000}},
    // EBX last RMID; EDX L3 monitoring.
    {0xf, 0, {0x00000000, 0xffffffff, 0x00000000, 0x00000002}},
    // EAX counter size, overflow; EBX scale; ECX last RMID; EDX occupancy, bandwidth 0, bandwidth
    // 1.
    {0xf, 1, {0x000001ff, 0xffffffff, 0xffffffff, 0x00000007}},
    // EBX L3 allocation.
    {0x10, 0, {0x00000000, 0x00000002, 0x00000000, 0x00000000}},
    // EAX mask length; EBX shared mask; ECX CDP; EDX last COS.
    {0x10, 1, {0x0000001f, 0xffffffff, 0x00000004, 0x0000ffff}},
    // EBX vendor; ECX vendor; EDX vendor.
    {0x80000000, SUBLEAF_ALL, {0x00000000, 0xffffffff, 0xffffffff, 0xffffffff}},
    // EAX stepping, base model, base family, extended model, extended family; EBX brand ID, package
    // type; ECX LAHF/SAHF, CmpLegacy, AltMovCr8, ABM, SSE4A, MisAlignSse, 3DNowPrefetch, OSVW, XOP,
    // FMA4, TCE, TBM, topology extensions, PerfCtrExtCore, PerfCtrExtNB, DataBkptExt, PerfTsc,
    // PerfCtrExtLLC, MONITORX, AddrMaskExt; EDX FPU, VME, DE, PSE, TSC, MSR, PAE, MCE, CMPXCHG8B,
    // SYSCALL, MTRR, PGE, MCA, CMOV, PAT, PSE36, NX, MmxExt, MMX, FXSR, FFXSR, 1 GiB pages, RDTSCP,
    // LM, 3DNowExt, 3DNow.
    {0x80000001, SUBLEAF_ALL, {0x0fff0fff, 0xf000ffff, 0x7de30bf3, 0xefd3f9ff}},
    // EAX name; EBX name; ECX name; EDX name.
    {0x80000002, SUBLEAF_ALL, {0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff}},
    // EAX name; EBX name; ECX name; EDX name.
    {0x80000003, SUBLEAF_ALL, {0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff}},
    // EAX name; EBX name; ECX name; EDX name.
    {0x80000004, SUBLEAF_ALL, {0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff}},
    // EAX L1 cache and TLB; EBX L1 cache and TLB; ECX L1 cache and TLB; EDX L1 cache and TLB.
    {0x80000005, SUBLEAF_ALL, {0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff}},
    // EAX L2 and L3 cache and TLB; EBX L2 and L3 cache and TLB; ECX L2 and L3 cache and TLB; EDX L2
    // and L3 cache and TLB.
    {0x80000006, SUBLEAF_ALL, {0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff}},
    // EBX MCA overflow recovery, SUCCOR, HWA, scalable MCA, MCA query MSRs; ECX power sample ratio;
    // EDX TS, FID, VID, TTP, TM, 100 MHz steps, HwPstate, invariant TSC, CPB, EffFreqRO,
    // ProcFeedbackInterface, ProcPowerReporting, CppcPerfPriority.
    {0x80000007, SUBLEAF_ALL, {0x00000000, 0x0000800f, 0xffffffff, 0x00011fdf}},
    // EAX physical bits, linear bits; EBX CLZERO, InstRetCntMsr, RstrFpErrPtrs, RDPRU, BE,
    // WBNOINVD, IBPB, INT_WBINVD, IBRS, STIBP, IBRS always on, STIBP always on, IBRS preferred,
    // IBRS same mode, LMSLE unsupported, SSBD, SSBD not required, CPPC, PSFD, BTC_NO, IBPB_RET; ECX
    // cores, APIC ID size, PerfTsc size; EDX RDPRU registers.
    {0x80000008, SUBLEAF_ALL, {0x0000ffff, 0x7d1ff257, 0x0003f0ff, 0x00ff0000}},
    // EAX 1 GiB TLB; EBX 1 GiB TLB.
    {0x80000019, SUBLEAF_ALL, {0xffffffff, 0xffffffff, 0x00000000, 0x00000000}},
    // EAX FP128, MOVU, FP256.
    {0x8000001a, SUBLEAF_ALL, {0x00000007, 0x00000000, 0x00000000, 0x00000000}},
    // EAX type, level, self-initialising, fully associative, sharing; EBX line size, partitions,
    // ways; ECX sets; EDX WBINVD scope, inclusive.
    {0x8000001d, SUBLEAF_ALL, {0x03ffc3ff, 0xffffffff, 0xffffffff, 0x00000003}},
    // EAX extended APIC ID; EBX compute unit, threads per unit; ECX node, nodes.
    {0x8000001e, SUBLEAF_ALL, {0xffffffff, 0x0000ffff, 0x000007ff, 0x00000000}},
    // EBX L3MBE, L3SMBE, BMEC, ABMC.
    {0x80000020, 0, {0x00000000, 0x0000002e, 0x00000000, 0x00000000}},
    // EAX bandwidth length; EDX last COS.
    {0x80000020, 1, {0xffffffff, 0x00000000, 0x00000000, 0xffffffff}},
    // EAX bandwidth length; EDX last COS.
    {0x80000020, 2, {0xffffffff, 0x00000000, 0x00000000, 0xffffffff}},
    // EBX events; ECX transaction types.
    {0x80000020, 3, {0x00000000, 0x000000ff, 0x0000007f, 0x00000000}},
    // EAX counter size, overflow; EBX last counter; ECX COS counting.
    {0x80000020, 5, {0x000001ff, 0x0000ffff, 0x00000001, 0x00000000}},
    // EAX NoNestedDataBp, FsGsBaseWriteNotSerializing, LFENCE serialising, null selector clears
    // base, AutomaticIBRS, NoSmmCtlMSR, fast short STOSB, fast short CMPSB,
    // PreciseRetirePmc2Manual, PrefetchCtlMsr, L2 TLB size x32, AMD ERMSB, 0F 01 /7 reclaim, EPSF,
    // fast REP SCASB, PREFETCHI, FP512 downgrade, AVX512_BMM, SBPB, IBPB_BRTYPE, SRSO_NO,
    // SRSO_USER_KERNEL_NO, SRSO_MSR_FIX; ECX TSA_SQ_NO, TSA_L1_NO, VERW_CLEAR, BTB context
    // isolation.
    {0x80000021, SUBLEAF_ALL, {0xf8bdff47, 0x00000000, 0x00000126, 0x00000000}},
    // EAX PerfMonV2, LBR and PMC freeze; EBX core counters, NB counters.
    {0x80000022, SUBLEAF_ALL, {0x00000005, 0x0000fc0f, 0x00000000, 0x00000000}},
    // EAX shift, asymmetric, heterogeneous, efficiency ranking; EBX processors, efficiency rank,
    // native model, core type; ECX level, type; EDX extended APIC ID.
    {0x80000026, SUBLEAF_ALL, {0xe000001f, 0xffffffff, 0x0000ffff, 0xffffffff}},
};

// The last leaves the tables have.
#define INTEL_LAST_BASIC 0x28
#define INTEL_LAST_EXTENDED 0x80000008
#define AMD_LAST_BASIC 0x10
#define AMD_LAST_EXTENDED 0x80000026

// The leaves whose subleaf 0 EAX gives their last subleaf, and the last ones
// the tables have.
struct last_subleaf {
    uint32_t leaf;
    uint32_t last;
};

static const struct last_subleaf intel_last_subleaves[] = {
    {0x7, 2}, {0x14, 1}, {0x17, 3}, {0x1d, 1}, {0x20, 0}, {0x24, 1},
};

static const struct last_subleaf amd_last_subleaves[] = {
    {0x7, 1},
};

#define CPUID_1_ECX_VMX (1u << 5)
#define CPUID_1_ECX_XSAVE (1u << 26)
#define CPUID_1_ECX_OSXSAVE (1u << 27)
#define CPUID_1_EDX_APIC (1u << 9)
#define CPUID_7_EBX_INVPCID (1u << 10)
#define CPUID_7_EBX_MPX (1u << 14)
#define CPUID_7_EBX_PT (1u << 25)
#define CPUID_7_ECX_PKU (1u << 3)
#define CPUID_7_ECX_OSPKE (1u << 4)
#define CPUID_7_ECX_WAITPKG (1u << 5)
#define CPUID_7_ECX_MAWAU (0x1fu << 17)
#define CPUID_7_ECX_RDPID (1u << 22)
#define CPUID_7_EDX_ARCH_LBR (1u << 19)
#define CPUID_D_1_EAX_XSAVES (1u << 3)
#define CPUID_PT 0x14
#define CPUID_ARCH_LBR 0x1c
#define CPUID_PERFMON_EXT 0x23
#define CPUID_PERFMON_EXT_PEBS (3u << 4)
#define CPUID_80000001_ECX_TCE (1u << 17)
#define CPUID_80000001_EDX_APIC (1u << 9)
#define CPUID_80000001_EDX_SYSCALL (1u << 11)
#define CPUID_80000001_EDX_NX (1u << 20)
#define CPUID_80000001_EDX_FFXSR (1u << 25)
#define CPUID_80000001_EDX_RDTSCP (1u << 27)
#define CPUID_80000001_EDX_LM (1u << 29)
#define CPUID_80000008_EBX_LMSLE_UNSUPPORTED (1u << 20)
#define CPUID_80000021_EAX_AUTOMATIC_IBRS (1u << 8)

// XSAVE's state components: their flags in subleaf ECX (supervisor, aligned,
// XFD), and the components of the features the guest gets only where the
// hypervisor lets it have them.
#define XSAVE_LAST_COMPONENT 63
#define XSAVE_COMPONENT_FLAGS 7u
#define XSAVE_LEGACY_AND_HEADER 576
#define XSAVE_MPX (3u << 3)
#define XSAVE_PT 8
#define XSAVE_LBR 15

// The processor's last leaves, the guest's, and the XSAVE state the guest may
// have, with the size of the standard format's area holding all of its user
// components.
static uint32_t processor_last_basic;
static uint32_t processor_last_extended;
static uint32_t last_basic;
static uint32_t last_extended;
static bool xsaves;
static uint64_t xcr0_allowed;
static uint64_t xss_allowed;
static uint32_t xsave_size;

static const struct leaf_bits *find(uint32_t leaf, uint32_t subleaf) {
    const struct leaf_bits *table = virt_vmx ? intel_leaves : amd_leaves;
    size_t count = virt_vmx ? sizeof(intel_leaves) / sizeof(intel_leaves[0])
                            : sizeof(amd_leaves) / sizeof(amd_leaves[0]);
    const struct leaf_bits *any = NULL;
    for (size_t i = 0; i < count; i++) {
        if (table[i].leaf == leaf && table[i].subleaf == subleaf) {
            return &table[i];
        }
        if (table[i].leaf == leaf && table[i].subleaf == SUBLEAF_ALL) {
            any = &table[i];
        }
    }
    return any;
}

static struct cpuid_regs masked(uint32_t leaf, uint32_t subleaf) {
    const struct leaf_bits *bits = find(leaf, subleaf);
    if (bits == NULL || (leaf < 0x80000000 ? leaf > processor_last_basic
                                           : leaf > processor_last_extended)) {
        return (struct cpuid_regs){0};
    }
    struct cpuid_regs r = cpuid(leaf, subleaf);
    r.eax &= bits->keep[0];
    r.ebx &= bits->keep[1];
    r.ecx &= bits->keep[2];
    r.edx &= bits->keep[3];
    return r;
}

static uint32_t min32(uint32_t a, uint32_t b) {
    return a < b ? a : b;
}

static uint32_t last_subleaf(uint32_t leaf, uint32_t eax) {
    const struct last_subleaf *table = virt_vmx ? intel_last_subleaves : amd_last_subleaves;
    size_t count = virt_vmx ? sizeof(intel_last_subleaves) / sizeof(intel_last_subleaves[0])
                            : sizeof(amd_last_subleaves) / sizeof(amd_last_subleaves[0]);
    for (size_t i = 0; i < count; i++) {
        if (table[i].leaf == leaf) {
            return min32(eax, table[i].last);
        }
    }
    return eax;
}

// Intel PT where VMX keeps it the guest's (vmx.c), and the instructions VMX
// makes #UD unless the guest's VMCS lets them run (SDM 325462-093 Vol. 3C,
// 28.3) only where it does.
static bool pt(void) {
    return virt_vmx && vmx_pt_passthrough();
}

static bool mpx(void) {
    return virt_vmx && vmx_mpx_passthrough();
}

static bool vmx_runs(uint32_t control) {
    return !virt_vmx || vmx_secondary_allowed(control);
}

void cpuid_init(void) {
    processor_last_basic = cpuid(0, 0).eax;
    processor_last_extended = cpuid(0x80000000, 0).eax;
    last_basic = min32(processor_last_basic, virt_vmx ? INTEL_LAST_BASIC : AMD_LAST_BASIC);
    last_extended = min32(processor_last_extended,
                          virt_vmx ? INTEL_LAST_EXTENDED : AMD_LAST_EXTENDED);

    struct cpuid_regs user = masked(0xd, 0);
    struct cpuid_regs supervisor = masked(0xd, 1);
    xcr0_allowed = user.eax | (uint64_t)user.edx << 32;
    if (!mpx()) {
        xcr0_allowed &= ~(uint64_t)XSAVE_MPX;
    }
    xsaves = (supervisor.eax & CPUID_D_1_EAX_XSAVES) && vmx_runs(PROC2_XSAVES);
    if (xsaves) {
        xss_allowed = supervisor.ecx | (uint64_t)supervisor.edx << 32;
        if (!pt()) {
            xss_allowed &= ~(1ull << XSAVE_PT);
        }
        // Architectural LBRs are not the guest's yet.
        xss_allowed &= ~(1ull << XSAVE_LBR);
    }
    xsave_size = XSAVE_LEGACY_AND_HEADER;
    for (uint32_t i = 2; i <= XSAVE_LAST_COMPONENT; i++) {
        if (xcr0_allowed & (1ull << i)) {
            struct cpuid_regs component = cpuid(0xd, i);
            if (component.ebx + component.eax > xsave_size) {
                xsave_size = component.ebx + component.eax;
            }
        }
    }
}

uint64_t cpuid_xcr0_allowed(void) {
    return xcr0_allowed;
}

uint64_t cpuid_xss_allowed(void) {
    return xss_allowed;
}

uint64_t cpuid_efer_allowed(struct vcpu *v) {
    struct cpuid_regs ext = cpuid_guest(v, 0x80000001, 0);
    uint64_t efer = 0;
    if (ext.edx & CPUID_80000001_EDX_SYSCALL) {
        efer |= EFER_SCE;
    }
    if (ext.edx & CPUID_80000001_EDX_LM) {
        efer |= EFER_LME | EFER_LMA;
    }
    if (ext.edx & CPUID_80000001_EDX_NX) {
        efer |= EFER_NXE;
    }
    if (ext.edx & CPUID_80000001_EDX_FFXSR) {
        efer |= EFER_FFXSR;
    }
    if (ext.ecx & CPUID_80000001_ECX_TCE) {
        efer |= EFER_TCE;
    }
    if (!virt_vmx) {
        if (!(cpuid_guest(v, 0x80000008, 0).ebx & CPUID_80000008_EBX_LMSLE_UNSUPPORTED)) {
            efer |= EFER_LMSLE;
        }
        if (cpuid_guest(v, 0x80000021, 0).eax & CPUID_80000021_EAX_AUTOMATIC_IBRS) {
            efer |= EFER_AIBRSE;
        }
    }
    return efer;
}

struct cpuid_regs cpuid_guest(struct vcpu *v, uint32_t leaf, uint32_t subleaf) {
    // Leaves past the last read on Intel's processors as the last basic one
    // (SDM 325462-093 Vol. 1, 21.1.3), which IA32_MISC_ENABLE's limit makes
    // leaf 2; APM 24594 rev. 3.38 leaves them undefined, and they read as
    // zeros.
    uint32_t basic = (v->misc_enable & MISC_ENABLE_LIMIT_CPUID) ? 2 : last_basic;
    if (leaf < 0x80000000 ? leaf > basic : leaf > last_extended) {
        if (!virt_vmx) {
            return (struct cpuid_regs){0};
        }
        leaf = basic;
    }

    struct cpuid_regs r = masked(leaf, subleaf);
    if (subleaf == 0) {
        r.eax = last_subleaf(leaf, r.eax);
    }
    switch (leaf) {
        case 0: {
            r.eax = basic;
            break;
        }
        case 1: {
            // VMX where the guest gets it, and what reflects the guest's CR4
            // and local APIC.
            if (nvmx_enabled) {
                r.ecx |= cpuid(1, 0).ecx & CPUID_1_ECX_VMX;
            }
            if ((vcpu_cr4(v) & CR4_OSXSAVE) && (r.ecx & CPUID_1_ECX_XSAVE)) {
                r.ecx |= CPUID_1_ECX_OSXSAVE;
            }
            if (vlapic_enabled(v)) {
                r.edx |= CPUID_1_EDX_APIC;
            }
            break;
        }
        case 7: {
            if (subleaf != 0) {
                break;
            }
            if ((vcpu_cr4(v) & CR4_PKE) && (r.ecx & CPUID_7_ECX_PKU)) {
                r.ecx |= CPUID_7_ECX_OSPKE;
            }
            if (!vmx_runs(PROC2_INVPCID)) {
                r.ebx &= ~CPUID_7_EBX_INVPCID;
            }
            if (!vmx_runs(PROC2_USER_WAIT_PAUSE)) {
                r.ecx &= ~CPUID_7_ECX_WAITPKG;
            }
            if (!vmx_runs(PROC2_RDTSCP)) {
                r.ecx &= ~CPUID_7_ECX_RDPID;
            }
            if (!pt()) {
                r.ebx &= ~CPUID_7_EBX_PT;
            }
            if (!mpx()) {
                r.ebx &= ~CPUID_7_EBX_MPX;
                r.ecx &= ~CPUID_7_ECX_MAWAU;
            }
            r.edx &= ~CPUID_7_EDX_ARCH_LBR;
            break;
        }
        case 0xd: {
            // The components the guest may enable, and their descriptions.
            if (subleaf == 0) {
                r.eax = (uint32_t)xcr0_allowed;
                r.ecx = xsave_size;
                r.edx = (uint32_t)(xcr0_allowed >> 32);
            } else if (subleaf == 1) {
                if (!xsaves) {
                    r.eax &= ~CPUID_D_1_EAX_XSAVES;
                }
                r.ecx = (uint32_t)xss_allowed;
                r.edx = (uint32_t)(xss_allowed >> 32);
            } else if (subleaf <= XSAVE_LAST_COMPONENT
                       && ((xcr0_allowed | xss_allowed) & (1ull << subleaf))) {
                r = cpuid(0xd, subleaf);
                r.ecx &= XSAVE_COMPONENT_FLAGS;
                r.edx = 0;
            }
            break;
        }
        case CPUID_PT: {
            if (!pt()) {
                r = (struct cpuid_regs){0};
            }
            break;
        }
        case CPUID_ARCH_LBR: {
            r = (struct cpuid_regs){0};
            break;
        }
        case CPUID_PERFMON_EXT: {
            // Architectural PEBS is not the guest's yet.
            if (subleaf == 0) {
                r.eax &= ~CPUID_PERFMON_EXT_PEBS;
            } else if (subleaf < 32 && (CPUID_PERFMON_EXT_PEBS & (1u << subleaf))) {
                r = (struct cpuid_regs){0};
            }
            break;
        }
        case 0x80000000: {
            r.eax = last_extended;
            break;
        }
        case 0x80000001: {
            // AMD's copy of the APIC flag, NX unless the guest's
            // IA32_MISC_ENABLE disables it, and RDTSCP where the guest's VMCS
            // lets it run.
            if (!virt_vmx && vlapic_enabled(v)) {
                r.edx |= CPUID_80000001_EDX_APIC;
            }
            if (v->misc_enable & MISC_ENABLE_XD_DISABLE) {
                r.edx &= ~CPUID_80000001_EDX_NX;
            }
            if (!vmx_runs(PROC2_RDTSCP)) {
                r.edx &= ~CPUID_80000001_EDX_RDTSCP;
            }
            break;
        }
        default: {
            break;
        }
    }
    return r;
}
