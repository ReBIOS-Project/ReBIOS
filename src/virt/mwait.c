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
// You should have received a copy of the GNU General Public License along with
// ReBIOS. If not, see <https://www.gnu.org/licenses/>.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <virt/mwait.h>
#include <lib/util.h>
#include <memory.h>
#include <sys/cpu.h>
#include <sys/x86.h>
#include <virt/cpuid.h>
#include <virt/event.h>
#include <virt/guest_mem.h>
#include <virt/insn.h>
#include <virt/vcpu.h>
#include <virt/virt.h>

// The instructions' ModRM bytes in the 0F 01 group (SDM 325462-093 Vol. 2B;
// APM 40332 rev. 4.11, MONITORX and MWAITX).
#define MODRM_MONITORX 0xfa
#define MODRM_MWAITX 0xfb

// MWAIT's extensions in ECX: interrupts end the wait even while masked,
// MWAITX's timeout in EBX, and Intel's monitorless MWAIT (SDM 325462-093 Vol.
// 2B, Table 1-2; APM 40332 rev. 4.11, MWAITX).
#define EXT_INTERRUPTS ((uint32_t)1 << 0)
#define EXT_TIMER ((uint32_t)1 << 1)
#define EXT_MONITORLESS ((uint32_t)1 << 2)

// CPUID's word on them: 01H ECX MONITOR, 05H ECX interrupts as break events
// and monitorless MWAIT, 07H.01H EDX MWAIT, and 8000_0001H ECX MONITORX.
#define LEAF1_MONITOR ((uint32_t)1 << 3)
#define LEAF5_INTERRUPTS ((uint32_t)1 << 1)
#define LEAF5_MONITORLESS ((uint32_t)1 << 3)
#define LEAF7_1_MWAIT ((uint32_t)1 << 23)
#define EXT_LEAF1_MONITORX ((uint32_t)1 << 29)

// A hint's target C-state, in EAX[7:4]: 0 for C1, 0FH for C0; EAX[31:8] are
// reserved.
#define HINT_STATE_SHIFT 4
#define HINT_STATE_C1 0x0
#define HINT_STATE_C0 0xf
#define HINT_MASK 0xff

// A line's size where the processor reports none.
#define LINE_DEFAULT 64

// What the guest's processor offers, whatever the guest's CPUID limit:
// MONITOR, MONITORX, MWAIT without MONITOR, and CPUID 05H ECX's extensions;
// the processor's largest monitor line, CPUID 05H EBX[15:0], which no store to
// it goes past unseen (SDM 325462-093 Vol. 3A, 11.10.5); and whether its
// timers go on in deep C-states.
static bool offers_monitor;
static bool offers_monitorx;
static bool offers_mwait;
static uint32_t offered_extensions;
static size_t line_size;
static bool deep_safe;

void mwait_init(void) {
    offers_monitor = (cpuid_offered(1, 0).ecx & LEAF1_MONITOR) != 0;
    offers_monitorx = (cpuid_offered(0x80000001, 0).ecx & EXT_LEAF1_MONITORX) != 0;
    offers_mwait = offers_monitor || (cpuid_offered(7, 1).edx & LEAF7_1_MWAIT) != 0;
    offered_extensions = cpuid_offered(5, 0).ecx;
    size_t largest = cpuid(0, 0).eax >= 5 ? cpuid(5, 0).ebx & 0xffff : 0;
    line_size = LINE_DEFAULT;
    if (largest > LINE_DEFAULT && (largest & (largest - 1)) == 0) {
        line_size = largest < VCPU_MONITOR_LINE_MAX ? largest : VCPU_MONITOR_LINE_MAX;
    }
    deep_safe = cpu_deep_idle_safe();
}

void mwait_monitor(struct vcpu *v) {
    struct insn insn;
    // One the hypervisor cannot read, as where another processor just changed
    // its mapping, runs again.
    if (!insn_decode(v, &insn)) {
        return;
    }
    bool x = insn.has_modrm && insn.modrm == MODRM_MONITORX;
    if (x ? !offers_monitorx : !offers_monitor) {
        vcpu_inject_exception(v, VECTOR_UD, false, 0);
        return;
    }
    uint64_t extensions = insn.long_mode ? v->gprs.rcx : (uint32_t)v->gprs.rcx;
    if (extensions != 0) {
        vcpu_inject_exception(v, VECTOR_GP, true, 0);
        return;
    }
    // DS:rAX, or the segment an override names.
    uint64_t ea = v->gprs.rax;
    if (insn.address_size == 4) {
        ea = (uint32_t)ea;
    } else if (insn.address_size == 2) {
        ea = (uint16_t)ea;
    }
    int segment = insn.segment >= 0 ? insn.segment : VCPU_SEG_DS;
    uint64_t linear;
    if (!insn_segment_linear(v, segment, ea, 1, false, &linear)) {
        return;
    }
    uint64_t gpa;
    uint32_t pf_error;
    if (!guest_mem_linear_to_phys(v, linear, false, false, &gpa, &pf_error)) {
        vcpu_inject_page_fault(v, linear, pf_error);
        return;
    }
    // What is not write-back RAM arms nothing, and MWAIT goes on at once, as
    // the processor's own monitoring may not be armed there.
    uint64_t at = gpa & ~(uint64_t)(line_size - 1);
    uint8_t *page = guest_mem_page(page_down(at));
    v->monitor_line = page != NULL ? page + (at - page_down(at)) : NULL;
    if (v->monitor_line != NULL) {
        memcpy(v->monitor_copy, v->monitor_line, line_size);
    }
    vcpu_skip_instruction(v);
}

static bool line_changed(struct vcpu *v) {
    return memcmp(v->monitor_line, v->monitor_copy, line_size) != 0;
}

// What ends the wait: an interrupt, while masked too with ECX[0], an NMI, an
// INIT, a store that changed the line, or MWAITX's timeout.
static bool ends(struct vcpu *v) {
    if (event_wait_ends(v, (v->mwait_extensions & EXT_INTERRUPTS) != 0)) {
        return true;
    }
    if ((v->mwait_extensions & EXT_MONITORLESS) == 0 && line_changed(v)) {
        return true;
    }
    return v->mwait_deadline != 0 && rdtsc() >= v->mwait_deadline;
}

// The processor's own MWAIT, with interrupts on, so that any of them ends it.
// The line goes under the monitor again first, a store between the guest's
// MONITOR and this showing as a change since then.
static void idle(struct vcpu *v) {
    uint32_t hint = v->mwait_hint & HINT_MASK;
    unsigned int state = hint >> HINT_STATE_SHIFT;
    if (state != HINT_STATE_C1 && state != HINT_STATE_C0 && !deep_safe) {
        hint = HINT_STATE_C1 << HINT_STATE_SHIFT;
    }
    if ((v->mwait_extensions & EXT_MONITORLESS) != 0) {
        asm volatile (
            "sti\n\t"
            "mwait\n\t"
            "cli"
            :
            : [hint] "a"(hint), [extensions] "c"(EXT_MONITORLESS)
            : "memory"
        );
        return;
    }
    if (v->mwait_x) {
        asm volatile (
            "monitorx"
            :
            : [monitor_line] "a"(v->monitor_line), [extensions] "c"(0), [hints] "d"(0)
            : "memory"
        );
    } else {
        asm volatile (
            "monitor"
            :
            : [monitor_line] "a"(v->monitor_line), [extensions] "c"(0), [hints] "d"(0)
            : "memory"
        );
    }
    if (line_changed(v)) {
        return;
    }
    if (!v->mwait_x) {
        asm volatile (
            "sti\n\t"
            "mwait\n\t"
            "cli"
            :
            : [hint] "a"(hint), [extensions] "c"(0)
            : "memory"
        );
        return;
    }
    uint32_t extensions = 0;
    uint32_t wait = 0;
    if (v->mwait_deadline != 0) {
        uint64_t now = rdtsc();
        if (now >= v->mwait_deadline) {
            return;
        }
        uint64_t left = v->mwait_deadline - now;
        extensions = EXT_TIMER;
        wait = left < UINT32_MAX ? (uint32_t)left : UINT32_MAX;
    }
    asm volatile (
        "sti\n\t"
        "mwaitx\n\t"
        "cli"
        :
        : [hint] "a"(hint), [extensions] "c"(extensions), [wait] "b"(wait)
        : "memory"
    );
}

void mwait_wait(struct vcpu *v) {
    struct insn insn;
    if (!insn_decode(v, &insn)) {
        return;
    }
    bool x = insn.has_modrm && insn.modrm == MODRM_MWAITX;
    uint32_t extensions = (uint32_t)v->gprs.rcx;
    uint32_t allowed;
    if (x) {
        if (!offers_monitorx) {
            vcpu_inject_exception(v, VECTOR_UD, false, 0);
            return;
        }
        allowed = EXT_INTERRUPTS | EXT_TIMER;
    } else if (!virt_vmx) {
        if (!offers_monitor) {
            vcpu_inject_exception(v, VECTOR_UD, false, 0);
            return;
        }
        allowed = EXT_INTERRUPTS;
    } else {
        // Intel's MWAIT is there with MONITOR, or monitorless alone, ECX[2]
        // then being required (SDM 325462-093 Vol. 2B, MWAIT).
        if (!offers_mwait) {
            vcpu_inject_exception(v, VECTOR_UD, false, 0);
            return;
        }
        allowed = EXT_INTERRUPTS | EXT_MONITORLESS;
        if (!offers_monitor && (extensions & EXT_MONITORLESS) == 0) {
            allowed = 0;
        }
    }
    if ((extensions & ~allowed) != 0
     || ((extensions & EXT_INTERRUPTS) != 0 && (offered_extensions & LEAF5_INTERRUPTS) == 0)
     || ((extensions & EXT_MONITORLESS) != 0
      && (offered_extensions & LEAF5_MONITORLESS) == 0)) {
        vcpu_inject_exception(v, VECTOR_GP, true, 0);
        return;
    }
    vcpu_skip_instruction(v);
    // Without a line armed, it goes on at once.
    if (v->monitor_line == NULL && (extensions & EXT_MONITORLESS) == 0) {
        return;
    }
    v->mwait_hint = (uint32_t)v->gprs.rax;
    v->mwait_extensions = extensions;
    v->mwait_x = x;
    v->mwait_deadline = 0;
    // Setting the timer with EBX 0 sets none (APM 40332 rev. 4.11, MWAITX).
    if ((extensions & EXT_TIMER) != 0 && (uint32_t)v->gprs.rbx != 0) {
        v->mwait_deadline = rdtsc() + (uint32_t)v->gprs.rbx;
    }
    vcpu_wait(v, ends, idle, false);
    // Monitoring is clear once MWAIT is done, monitorless too.
    v->monitor_line = NULL;
}
