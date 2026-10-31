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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <virt/pit.h>
#include <lib/spinlock.h>
#include <lib/time.h>
#include <sys/cpu.h>
#include <sys/x86.h>
#include <virt/hpet.h>
#include <virt/hv_timer.h>
#include <virt/io.h>
#include <virt/irq.h>

struct vcpu;

// The 8254 is modelled from the Intel 8254 datasheet 231164-005 and timed by
// the TSC, as the chipset's may be stopped for good by firmware, out of reach
// behind a hidden P2SB, or missing. Only the speaker's channel still reaches
// the chipset's, where the sound comes from.

#define PORT_COUNTER_FIRST 0x40
#define PORT_COUNT 4
#define OFFSET_CONTROL 3
#define PORT_CONTROL (PORT_COUNTER_FIRST + OFFSET_CONTROL)
#define PORT_NMI_STATUS 0x61

#define PIT_HZ 1193182
#define CHANNELS 3
#define CHANNEL_SPEAKER 2
#define IRQ_TIMER 0

#define SELECT_SHIFT 6
#define SELECT_READ_BACK 3
#define ACCESS_SHIFT 4
#define ACCESS_MASK 3
#define ACCESS_LATCH 0
#define ACCESS_LOW 1
#define ACCESS_HIGH 2
#define ACCESS_WORD 3
// Second byte of a word: the states go on from ACCESS_WORD.
#define ACCESS_WORD_HIGH 4
#define MODE_SHIFT 1
#define MODE_MASK 7
#define CONTROL_BCD 1
// The mode that leaves OUT low and stops counting until a count is loaded.
#define CONTROL_STOP_COUNTER_0 0x30

#define READ_BACK_NO_COUNT 0x20
#define READ_BACK_NO_STATUS 0x10
#define READ_BACK_CHANNEL(n) ((uint8_t)2 << (n))

#define STATUS_OUT 0x80
#define STATUS_NULL_COUNT 0x40

#define NMI_GATE 0x01
#define NMI_WRITABLE 0x0f
#define NMI_REFRESH 0x10
#define NMI_OUT2 0x20
#define NMI_STATUS_BITS 0xc0
// PIT clocks a memory refresh took on PCs, which the toggle still follows.
#define REFRESH_TICKS 18

struct channel {
    // The TSC when the count started, so that counting is d ticks in.
    uint64_t start;
    // While not running, the ticks counted so far.
    uint64_t held;
    // The initial count, 0 standing for its highest.
    uint32_t count;
    uint16_t latched;
    int latch_state;
    uint8_t status;
    bool status_latched;
    int read_state;
    int write_state;
    uint8_t write_low;
    uint8_t access;
    uint8_t mode;
    bool bcd;
    bool gate;
    // A count is in, and the gate lets it count.
    bool loaded;
    bool running;
    // Modes 1 and 5 only count from a rising gate.
    bool triggered;
};

static struct spinlock lock;
static struct channel channels[CHANNELS];
static uint64_t counter_0_edge;
static struct hv_timer tick;
static uint64_t tick_begun;
static uint64_t tick_held_until;
static uint8_t nmi_written;
static uint64_t boot_tsc;

static uint64_t ticks_since(uint64_t start, uint64_t now) {
    if (now <= start) {
        return 0;
    }
    uint64_t elapsed = now - start;
    uint64_t hz = time_tsc_hz();
    // Split, so that the product cannot overflow.
    return elapsed / hz * PIT_HZ + elapsed % hz * PIT_HZ / hz;
}

static uint64_t ticks_to_tsc(uint64_t ticks) {
    uint64_t hz = time_tsc_hz();
    return ticks / PIT_HZ * hz + ticks % PIT_HZ * hz / PIT_HZ;
}

static uint64_t counted(const struct channel *c, uint64_t now) {
    return c->running ? ticks_since(c->start, now) : c->held;
}

static uint32_t highest(const struct channel *c) {
    return c->bcd ? 10000 : 0x10000;
}

static uint32_t from_bcd(uint32_t value) {
    return ((value >> 12) & 0xf) * 1000 + ((value >> 8) & 0xf) * 100
        + ((value >> 4) & 0xf) * 10 + (value & 0xf);
}

static uint32_t to_bcd(uint32_t value) {
    return value / 1000 % 10 << 12 | value / 100 % 10 << 8
        | value / 10 % 10 << 4 | value % 10;
}

// Mode 3 counts by two, from an odd count by one first while OUT is high, and
// by three first while it is low.
static uint32_t square_wave_count(uint32_t count, uint32_t position) {
    if ((count & 1) == 0) {
        return count - 2 * (position % (count / 2));
    }
    uint32_t high = (count + 1) / 2;
    if (position < high) {
        return position == 0 ? count : count + 1 - 2 * position;
    }
    uint32_t low = position - high;
    return low == 0 ? count : count - 1 - 2 * low;
}

static uint16_t current_count(const struct channel *c, uint64_t now) {
    if (!c->loaded) {
        return 0;
    }
    uint64_t d = counted(c, now);
    uint32_t value;
    switch (c->mode) {
        case 2: {
            value = c->count - (uint32_t)(d % c->count);
            break;
        }
        case 3: {
            value = square_wave_count(c->count, (uint32_t)(d % c->count));
            break;
        }
        // The others wrap around and keep counting.
        default: {
            uint32_t top = highest(c);
            value = (c->count + top - (uint32_t)(d % top)) % top;
            break;
        }
    }
    value %= highest(c);
    return (uint16_t)(c->bcd ? to_bcd(value) : value);
}

static bool out_level(const struct channel *c, uint64_t now) {
    if (!c->loaded) {
        return c->mode != 0;
    }
    uint64_t d = counted(c, now);
    switch (c->mode) {
        case 0: {
            return d >= c->count;
        }
        case 1: {
            return !c->triggered || d >= c->count;
        }
        case 2: {
            return !c->gate || d % c->count != c->count - 1;
        }
        case 3: {
            return !c->gate || d % c->count < (c->count + 1) / 2;
        }
        case 4: {
            return d != c->count;
        }
        default: {
            return !c->triggered || d != c->count;
        }
    }
}

// Counter 0's OUT rising next, in ticks counted: IRQ 0.
static void schedule_counter_0(uint64_t now) {
    struct channel *c = &channels[0];
    uint64_t edge = 0;
    if (c->loaded && c->running) {
        uint64_t d = counted(c, now);
        if (c->mode == 2 || c->mode == 3) {
            edge = (d / c->count + 1) * c->count;
        } else if (c->mode == 0 && d < c->count) {
            edge = c->count;
        } else if (c->mode == 4 && d <= c->count) {
            edge = (uint64_t)c->count + 1;
        }
    }
    counter_0_edge = edge;
    hv_timer_set(&tick, edge != 0 ? c->start + ticks_to_tsc(edge) : 0);
}

// What the hypervisor took over the last tick, the guest gets before the next
// one, as its handling must not leave it none when exits are slow: a virtual
// machine's or an emulator's. By a real processor's it is too short to notice.
void pit_tick_begun(void) {
    __atomic_store_n(&tick_begun, cpu_root_time(), __ATOMIC_RELAXED);
}

void pit_tick_ended(void) {
    uint64_t taken = cpu_root_time()
        - __atomic_load_n(&tick_begun, __ATOMIC_RELAXED);
    __atomic_store_n(&tick_held_until, rdtsc() + taken, __ATOMIC_RELAXED);
}

static void tick_fired(void *ctx) {
    (void)ctx;
    uint64_t now = rdtsc();
    spinlock_acquire(&lock);
    uint64_t held_until = __atomic_load_n(&tick_held_until, __ATOMIC_RELAXED);
    if (now < held_until) {
        hv_timer_set(&tick, held_until);
        spinlock_release(&lock);
        return;
    }
    struct channel *c = &channels[0];
    bool edge = counter_0_edge != 0 && c->running
        && counted(c, now) >= counter_0_edge;
    schedule_counter_0(now);
    spinlock_release(&lock);
    if (edge && !hpet_replaces_pit()) {
        irq_isa_pulse(IRQ_TIMER);
    }
}

static void load_count(struct channel *c, size_t index, uint32_t value) {
    uint64_t now = rdtsc();
    if (c->bcd) {
        value = from_bcd(value);
    }
    if (value == 0) {
        value = highest(c);
    }
    // The datasheet leaves a count of 1 undefined in the modes that reload.
    if (value == 1 && (c->mode == 2 || c->mode == 3)) {
        value = 2;
    }
    c->count = value;
    c->loaded = true;
    c->start = now;
    c->held = 0;
    if (c->mode == 1 || c->mode == 5) {
        c->running = c->triggered;
    } else {
        c->running = c->gate;
    }
    if (index == 0) {
        schedule_counter_0(now);
    }
}

static void latch_count(struct channel *c, uint64_t now) {
    if (c->latch_state == 0) {
        c->latched = current_count(c, now);
        c->latch_state = c->access;
    }
}

static void latch_status(struct channel *c, uint64_t now) {
    if (c->status_latched) {
        return;
    }
    bool null_count = !c->loaded
        || (c->running && ticks_since(c->start, now) == 0);
    c->status = (uint8_t)((out_level(c, now) ? STATUS_OUT : 0)
        | (null_count ? STATUS_NULL_COUNT : 0) | c->access << ACCESS_SHIFT
        | c->mode << MODE_SHIFT | (c->bcd ? CONTROL_BCD : 0));
    c->status_latched = true;
}

static void control_write(uint8_t value) {
    uint64_t now = rdtsc();
    size_t select = value >> SELECT_SHIFT;
    if (select == SELECT_READ_BACK) {
        for (size_t i = 0; i < CHANNELS; i++) {
            if ((value & READ_BACK_CHANNEL(i)) == 0) {
                continue;
            }
            // A channel's status comes first when both are latched.
            if ((value & READ_BACK_NO_COUNT) == 0) {
                latch_count(&channels[i], now);
            }
            if ((value & READ_BACK_NO_STATUS) == 0) {
                latch_status(&channels[i], now);
            }
        }
        return;
    }
    struct channel *c = &channels[select];
    uint8_t access = (value >> ACCESS_SHIFT) & ACCESS_MASK;
    if (access == ACCESS_LATCH) {
        latch_count(c, now);
        return;
    }
    uint8_t mode = (value >> MODE_SHIFT) & MODE_MASK;
    c->access = access;
    c->mode = mode > 5 ? mode - 4 : mode;
    c->bcd = (value & CONTROL_BCD) != 0;
    c->read_state = access;
    c->write_state = access;
    c->loaded = false;
    c->running = false;
    c->triggered = false;
    c->held = 0;
    if (select == 0) {
        schedule_counter_0(now);
    }
    if (select == CHANNEL_SPEAKER) {
        outb(PORT_CONTROL, value);
    }
}

static void counter_write(size_t index, uint8_t value) {
    struct channel *c = &channels[index];
    switch (c->write_state) {
        case ACCESS_LOW: {
            load_count(c, index, value);
            break;
        }
        case ACCESS_HIGH: {
            load_count(c, index, (uint32_t)value << 8);
            break;
        }
        case ACCESS_WORD: {
            c->write_low = value;
            c->write_state = ACCESS_WORD_HIGH;
            break;
        }
        case ACCESS_WORD_HIGH: {
            load_count(c, index, c->write_low | (uint32_t)value << 8);
            c->write_state = ACCESS_WORD;
            break;
        }
        default: {
            break;
        }
    }
    if (index == CHANNEL_SPEAKER) {
        outb(PORT_COUNTER_FIRST + CHANNEL_SPEAKER, value);
    }
}

static uint8_t counter_read(size_t index) {
    struct channel *c = &channels[index];
    if (c->status_latched) {
        c->status_latched = false;
        return c->status;
    }
    if (c->latch_state != 0) {
        int latch = c->latch_state;
        c->latch_state = latch == ACCESS_WORD ? ACCESS_HIGH : 0;
        return (uint8_t)(latch == ACCESS_HIGH ? c->latched >> 8 : c->latched);
    }
    uint16_t count = current_count(c, rdtsc());
    int state = c->read_state;
    if (state == ACCESS_WORD) {
        c->read_state = ACCESS_WORD_HIGH;
    } else if (state == ACCESS_WORD_HIGH) {
        c->read_state = ACCESS_WORD;
    }
    return (uint8_t)(state == ACCESS_HIGH || state == ACCESS_WORD_HIGH
        ? count >> 8 : count);
}

static void counters_access(
    struct vcpu *v, void *ctx, uint16_t offset, size_t size, bool in,
    uint32_t *value
) {
    (void)v;
    (void)ctx;
    spinlock_acquire(&lock);
    for (size_t i = 0; i < size; i++) {
        size_t port = offset + i;
        if (in) {
            uint8_t byte = 0;
            if (port < OFFSET_CONTROL) {
                byte = counter_read(port);
            } else if (port > OFFSET_CONTROL) {
                byte = inb((uint16_t)(PORT_COUNTER_FIRST + port));
            }
            *value = (*value & ~((uint32_t)0xff << (8 * i)))
                | (uint32_t)byte << (8 * i);
            continue;
        }
        uint8_t byte = (uint8_t)(*value >> (8 * i));
        if (port < OFFSET_CONTROL) {
            counter_write(port, byte);
        } else if (port == OFFSET_CONTROL) {
            control_write(byte);
        } else {
            outb((uint16_t)(PORT_COUNTER_FIRST + port), byte);
        }
    }
    spinlock_release(&lock);
}

static void speaker_gate(bool gate) {
    struct channel *c = &channels[CHANNEL_SPEAKER];
    if (gate == c->gate) {
        return;
    }
    uint64_t now = rdtsc();
    uint64_t d = counted(c, now);
    c->gate = gate;
    if (!c->loaded) {
        return;
    }
    switch (c->mode) {
        // The gate pauses these.
        case 0:
        case 4: {
            c->running = gate;
            c->held = d;
            c->start = now - ticks_to_tsc(d);
            break;
        }
        // These stop, and count again from the start.
        case 2:
        case 3: {
            c->running = gate;
            c->held = 0;
            c->start = now;
            break;
        }
        // These count from each rising edge, whatever comes after.
        default: {
            if (gate) {
                c->triggered = true;
                c->running = true;
                c->held = 0;
                c->start = now;
            }
            break;
        }
    }
}

static void nmi_status_access(
    struct vcpu *v, void *ctx, uint16_t offset, size_t size, bool in,
    uint32_t *value
) {
    (void)v;
    (void)ctx;
    (void)offset;
    spinlock_acquire(&lock);
    uint64_t now = rdtsc();
    if (in) {
        // The chipset's own status bits, where it has any.
        uint8_t real = inb(PORT_NMI_STATUS);
        uint8_t byte = (uint8_t)(nmi_written
            | (real != 0xff ? real & NMI_STATUS_BITS : 0)
            | (ticks_since(boot_tsc, now) / REFRESH_TICKS % 2 != 0
                ? NMI_REFRESH : 0)
            | (out_level(&channels[CHANNEL_SPEAKER], now) ? NMI_OUT2 : 0));
        *value = (*value & ~(uint32_t)0xff) | byte;
    } else {
        nmi_written = (uint8_t)*value & NMI_WRITABLE;
        speaker_gate((nmi_written & NMI_GATE) != 0);
        outb(PORT_NMI_STATUS, (uint8_t)*value);
    }
    // Wider accesses go on to ports that are no business of this one's.
    for (size_t i = 1; i < size; i++) {
        uint16_t port = (uint16_t)(PORT_NMI_STATUS + i);
        if (in) {
            *value = (*value & ~((uint32_t)0xff << (8 * i)))
                | (uint32_t)inb(port) << (8 * i);
        } else {
            outb(port, (uint8_t)(*value >> (8 * i)));
        }
    }
    spinlock_release(&lock);
}

void pit_init(void) {
    boot_tsc = rdtsc();
    // The chipset's stops interrupting, where it runs: its line stays for the
    // HPET, whose legacy replacement route a guest may use. The count is for
    // the implementations that only heed the mode once one comes, as Linux
    // 7.3's clockevent_i8253_disable() has it.
    outb(PORT_CONTROL, CONTROL_STOP_COUNTER_0);
    outb(PORT_COUNTER_FIRST, 0);
    outb(PORT_COUNTER_FIRST, 0);
    outb(PORT_CONTROL, CONTROL_STOP_COUNTER_0);
    uint8_t real = inb(PORT_NMI_STATUS);
    nmi_written = real != 0xff ? real & NMI_WRITABLE : 0;
    channels[0].gate = true;
    channels[1].gate = true;
    channels[CHANNEL_SPEAKER].gate = (nmi_written & NMI_GATE) != 0;

    tick.fired = tick_fired;
    hv_timer_add(&tick);

    struct io_handler counters = {
        PORT_COUNTER_FIRST, PORT_COUNT, counters_access, NULL
    };
    io_register(&counters);
    struct io_handler nmi_status = {
        PORT_NMI_STATUS, 1, nmi_status_access, NULL
    };
    io_register(&nmi_status);
}
