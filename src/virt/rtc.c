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
#include <uacpi/acpi.h>
#include <uacpi/namespace.h>
#include <uacpi/platform/types.h>
#include <uacpi/status.h>
#include <uacpi/tables.h>
#include <uacpi/types.h>
#include <uacpi/uacpi.h>
#include <uacpi/utilities.h>
#include <virt/rtc.h>
#include <core.h>
#include <handoff.h>
#include <hw/acpi.h>
#include <hw/cmos.h>
#include <lib/log.h>
#include <lib/spinlock.h>
#include <lib/time.h>
#include <lib/util.h>
#include <sys/x86.h>
#include <virt/hv_timer.h>
#include <virt/io.h>
#include <virt/irq.h>
#include <virt/layout.h>
#include <virt/vcpu.h>

// The guest's RTC and CMOS RAM, as the ICH10 has them (datasheet 319973-003,
// 13.6): the machine's RTC keeps the time, unless the firmware says it has
// none or it does not count, while the RAM, the extended bank, and the NMI
// mask bit of the index are the guest's own, so that the guest cannot touch
// the firmware's settings. Without a working RTC, an MC146818 is emulated.

#define PORT_FIRST 0x70
#define PORT_COUNT 8
// 74h-77h alias 70h-73h, but the standard index reads back from 74h alone,
// without its NMI mask bit (13.6.1).
#define OFFSET_INDEX 0
#define OFFSET_TARGET 1
#define OFFSET_EXT_INDEX 2
#define OFFSET_EXT_TARGET 3
#define OFFSET_ALIAS 4
#define INDEX_MASK 0x7f
#define BANK_SIZE 128

#define REG_SECONDS 0x00
#define REG_SECONDS_ALARM 0x01
#define REG_MINUTES 0x02
#define REG_MINUTES_ALARM 0x03
#define REG_HOURS 0x04
#define REG_HOURS_ALARM 0x05
#define REG_DAY_OF_WEEK 0x06
#define REG_DAY 0x07
#define REG_MONTH 0x08
#define REG_YEAR 0x09
#define REG_A 0x0a
#define REG_B 0x0b
#define REG_C 0x0c
#define REG_D 0x0d
#define CLOCK_REGS 0x0e

#define A_UIP 0x80
#define A_DV_MASK 0x70
#define A_DV_NORMAL 0x20
#define A_RS_MASK 0x0f
#define B_SET 0x80
#define B_PIE 0x40
#define B_AIE 0x20
#define B_UIE 0x10
#define B_BINARY 0x04
#define B_24_HOUR 0x02
#define C_IRQF 0x80
#define C_PF 0x40
#define C_AF 0x20
#define C_UF 0x10
#define D_VRT 0x80
#define D_DATE_ALARM 0x3f
#define HOURS_PM 0x80
// An alarm byte with both top bits set matches any value (MC146818).
#define ALARM_ANY 0xc0

// The PC/AT's CMOS RAM, as BIOSes keep it.
#define RAM_EQUIPMENT 0x14
#define RAM_BASE_MEMORY 0x15
#define RAM_EXT_MEMORY 0x17
#define RAM_CHECKSUM 0x2e
#define RAM_EXT_MEMORY_POST 0x30
#define RAM_CENTURY 0x32
#define RAM_CHECKSUM_FIRST 0x10
#define RAM_CHECKSUM_LAST 0x2d
#define EQUIPMENT_FPU 0x02
#define BASE_MEMORY_KIB 640
#define EXT_MEMORY_KIB_MAX 0xffff

#define IRQ_RTC 8
// The update starts within this of UIP's rise (13.6.2.1).
#define UIP_US 244
// How long an update may hold the time registers, and the seconds of the
// machine's RTC may take to change after rtc_sample().
#define UPDATE_WAIT_US 2500
#define COUNT_WAIT_MS 1100
// The 32.768 kHz oscillator's ticks of the shortest periodic interrupt rates.
#define OSCILLATOR_HZ 32768
// Whole steps of the emulated clock's updates, as their alarms need, while
// interrupts want them, else a leap.
#define STEPS_MAX 4

#define SECONDS_PER_DAY 86400
#define EPOCH_WEEKDAY 4

struct civil {
    int64_t year;
    unsigned int month;
    unsigned int day;
    unsigned int hour;
    unsigned int minute;
    unsigned int second;
    unsigned int weekday;
};

static struct spinlock lock;
static bool real;
static uint8_t century;
static uint8_t day_alarm_index;
static uint8_t month_alarm_index;
static uint8_t index;
static uint8_t ext_index;
static uint8_t ram[BANK_SIZE];
static uint8_t ext_ram[BANK_SIZE];

static uint8_t sampled_seconds;
static uint64_t sampled_tsc;
static bool sampled;

// The emulated clock: seconds since 1970, counting from second_tsc unless
// stopped, its registers, and the day of the week as far from the date's as
// the guest set it.
static int64_t seconds;
static uint64_t second_tsc;
static uint8_t reg_a = A_DV_NORMAL;
static uint8_t reg_b = B_24_HOUR;
static uint8_t reg_c;
static uint8_t date_alarm;
static uint8_t alarms[3];
static unsigned int weekday_offset;
static uint64_t periodic_tsc;
static struct hv_timer timer;

// Howard Hinnant's days_from_civil() and civil_from_days().
static int64_t days_from_civil(int64_t y, unsigned int m, unsigned int d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned int yoe = (unsigned int)(y - era * 400);
    unsigned int doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    unsigned int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

static void civil_from_days(int64_t z, int64_t *y, unsigned int *m, unsigned int *d) {
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned int doe = (unsigned int)(z - era * 146097);
    unsigned int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    unsigned int doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned int mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = (int64_t)yoe + era * 400 + (*m <= 2);
}

static struct civil civil_from_seconds(int64_t s) {
    int64_t days = s >= 0 ? s / SECONDS_PER_DAY : (s - SECONDS_PER_DAY + 1) / SECONDS_PER_DAY;
    int64_t in_day = s - days * SECONDS_PER_DAY;
    struct civil c;
    civil_from_days(days, &c.year, &c.month, &c.day);
    c.hour = (unsigned int)(in_day / 3600);
    c.minute = (unsigned int)(in_day / 60 % 60);
    c.second = (unsigned int)(in_day % 60);
    c.weekday = (unsigned int)(((days + EPOCH_WEEKDAY) % 7 + 7) % 7);
    return c;
}

static int64_t seconds_from_civil(const struct civil *c) {
    int64_t days = days_from_civil(c->year, c->month, c->day);
    return days * SECONDS_PER_DAY + (int64_t)c->hour * 3600 + (int64_t)c->minute * 60
         + c->second;
}

// A register's value in the clock's data mode, and back, as an MC146818 keeps
// no check of what it is given.
static uint8_t encode(unsigned int value) {
    if ((reg_b & B_BINARY) != 0) {
        return (uint8_t)value;
    }
    return (uint8_t)((value / 10 % 10) << 4 | value % 10);
}

static unsigned int decode(uint8_t value) {
    if ((reg_b & B_BINARY) != 0) {
        return value;
    }
    return (unsigned int)(value >> 4) * 10 + (value & 0x0f);
}

static uint8_t encode_hours(unsigned int hour) {
    if ((reg_b & B_24_HOUR) != 0) {
        return encode(hour);
    }
    unsigned int twelve = hour % 12 != 0 ? hour % 12 : 12;
    return (uint8_t)(encode(twelve) | (hour >= 12 ? HOURS_PM : 0));
}

static unsigned int decode_hours(uint8_t value) {
    if ((reg_b & B_24_HOUR) != 0) {
        return decode(value);
    }
    unsigned int twelve = decode(value & (uint8_t)~HOURS_PM) % 12;
    return twelve + ((value & HOURS_PM) != 0 ? 12 : 0);
}

static bool stopped(void) {
    return (reg_b & B_SET) != 0 || (reg_a & A_DV_MASK) != A_DV_NORMAL;
}

static bool alarm_matches(void) {
    struct civil c = civil_from_seconds(seconds);
    uint8_t now[3] = { encode(c.second), encode(c.minute), encode_hours(c.hour) };
    for (size_t i = 0; i < 3; i++) {
        if ((alarms[i] & ALARM_ANY) != ALARM_ANY && alarms[i] != now[i]) {
            return false;
        }
    }
    uint8_t day = date_alarm & D_DATE_ALARM;
    return day == 0 || decode(day) == c.day;
}

static uint64_t periodic_period(void) {
    unsigned int rs = reg_a & A_RS_MASK;
    if (rs == 0) {
        return 0;
    }
    // Rates 1 and 2 are 8's and 9's.
    unsigned int tap = rs <= 2 ? rs + 7 : rs;
    return time_tsc_hz() * ((uint64_t)1 << (tap - 1)) / OSCILLATOR_HZ;
}

static bool irq_wanted(void) {
    return ((reg_c & C_PF) != 0 && (reg_b & B_PIE) != 0)
        || ((reg_c & C_AF) != 0 && (reg_b & B_AIE) != 0)
        || ((reg_c & C_UF) != 0 && (reg_b & B_UIE) != 0);
}

static void update_line(void) {
    bool wanted = irq_wanted();
    reg_c = (uint8_t)((reg_c & ~C_IRQF) | (wanted ? C_IRQF : 0));
    irq_isa_set(IRQ_RTC, wanted);
}

// Brings the clock to now, its updates setting UF, and AF where the alarm
// matched.
static void advance(uint64_t now) {
    uint64_t hz = time_tsc_hz();
    if (stopped()) {
        second_tsc = now;
    } else if (now >= second_tsc + hz) {
        uint64_t elapsed = (now - second_tsc) / hz;
        second_tsc += elapsed * hz;
        if (elapsed <= STEPS_MAX) {
            for (size_t i = 0; i < elapsed; i++) {
                seconds++;
                if (alarm_matches()) {
                    reg_c |= C_AF;
                }
            }
        } else {
            seconds += (int64_t)elapsed;
            if (alarm_matches()) {
                reg_c |= C_AF;
            }
        }
        reg_c |= C_UF;
    }
    uint64_t period = periodic_period();
    if (period != 0 && now >= periodic_tsc + period) {
        periodic_tsc += (now - periodic_tsc) / period * period;
        reg_c |= C_PF;
    }
}

static void schedule(uint64_t now) {
    uint64_t deadline = 0;
    uint64_t period = periodic_period();
    if ((reg_b & B_PIE) != 0 && period != 0) {
        deadline = periodic_tsc + period;
    }
    if ((reg_b & (B_AIE | B_UIE)) != 0 && !stopped()) {
        uint64_t next = second_tsc + time_tsc_hz();
        if (deadline == 0 || next < deadline) {
            deadline = next;
        }
    }
    if (deadline != 0 && deadline <= now) {
        deadline = now + 1;
    }
    hv_timer_set(&timer, deadline);
}

static void timer_fired(void *ctx) {
    (void)ctx;
    spinlock_acquire(&lock);
    uint64_t now = rdtsc();
    advance(now);
    update_line();
    schedule(now);
    spinlock_release(&lock);
}

static uint8_t clock_read(uint8_t reg) {
    uint64_t now = rdtsc();
    advance(now);
    struct civil c = civil_from_seconds(seconds);
    switch (reg) {
        case REG_SECONDS: {
            return encode(c.second);
        }
        case REG_MINUTES: {
            return encode(c.minute);
        }
        case REG_HOURS: {
            return encode_hours(c.hour);
        }
        case REG_DAY_OF_WEEK: {
            return encode((c.weekday + weekday_offset) % 7 + 1);
        }
        case REG_DAY: {
            return encode(c.day);
        }
        case REG_MONTH: {
            return encode(c.month);
        }
        case REG_YEAR: {
            return encode((unsigned int)(c.year % 100));
        }
        case REG_SECONDS_ALARM: {
            return alarms[0];
        }
        case REG_MINUTES_ALARM: {
            return alarms[1];
        }
        case REG_HOURS_ALARM: {
            return alarms[2];
        }
        case REG_A: {
            uint64_t left = second_tsc + time_tsc_hz() - now;
            bool soon = !stopped() && left < time_tsc_hz() / 1000000 * UIP_US;
            return (uint8_t)(reg_a | (soon ? A_UIP : 0));
        }
        case REG_B: {
            return reg_b;
        }
        case REG_C: {
            uint8_t value = reg_c;
            reg_c = 0;
            update_line();
            schedule(now);
            return value;
        }
        case REG_D: {
            return (uint8_t)(D_VRT | date_alarm);
        }
        default: {
            return encode((unsigned int)(c.year / 100));
        }
    }
}

static void clock_write(uint8_t reg, uint8_t value) {
    uint64_t now = rdtsc();
    advance(now);
    struct civil c = civil_from_seconds(seconds);
    unsigned int weekday = (c.weekday + weekday_offset) % 7;
    switch (reg) {
        case REG_SECONDS: {
            c.second = decode(value) % 60;
            break;
        }
        case REG_MINUTES: {
            c.minute = decode(value) % 60;
            break;
        }
        case REG_HOURS: {
            c.hour = decode_hours(value) % 24;
            break;
        }
        case REG_DAY_OF_WEEK: {
            weekday = (decode(value) + 6) % 7;
            break;
        }
        case REG_DAY: {
            unsigned int day = decode(value);
            c.day = day >= 1 && day <= 31 ? day : 1;
            break;
        }
        case REG_MONTH: {
            unsigned int month = decode(value);
            c.month = month >= 1 && month <= 12 ? month : 1;
            break;
        }
        case REG_YEAR: {
            c.year = c.year / 100 * 100 + decode(value) % 100;
            break;
        }
        case REG_SECONDS_ALARM:
        case REG_MINUTES_ALARM:
        case REG_HOURS_ALARM: {
            alarms[(reg - REG_SECONDS_ALARM) / 2] = value;
            schedule(now);
            return;
        }
        case REG_A: {
            bool was_stopped = stopped();
            reg_a = value & (uint8_t)~A_UIP;
            if (was_stopped && !stopped()) {
                second_tsc = now;
            }
            periodic_tsc = now;
            update_line();
            schedule(now);
            return;
        }
        case REG_B: {
            bool was_stopped = stopped();
            reg_b = value;
            if (was_stopped && !stopped()) {
                second_tsc = now;
            }
            // Setting SET ends UIE, as on the MC146818.
            if ((reg_b & B_SET) != 0) {
                reg_b &= (uint8_t)~B_UIE;
            }
            update_line();
            schedule(now);
            return;
        }
        case REG_C: {
            return;
        }
        case REG_D: {
            date_alarm = value & D_DATE_ALARM;
            return;
        }
        default: {
            c.year = (int64_t)decode(value) * 100 + c.year % 100;
            break;
        }
    }
    seconds = seconds_from_civil(&c);
    weekday_offset = (weekday + 7 - civil_from_seconds(seconds).weekday) % 7;
}

static bool clock_register(uint8_t reg) {
    if (reg < CLOCK_REGS) {
        return true;
    }
    return reg == century || reg == day_alarm_index || reg == month_alarm_index;
}

static uint8_t standard_read(uint8_t reg) {
    if (!clock_register(reg)) {
        return ram[reg];
    }
    return real ? cmos_read(reg) : clock_read(reg);
}

static void standard_write(uint8_t reg, uint8_t value) {
    if (!clock_register(reg)) {
        ram[reg] = value;
    } else if (real) {
        cmos_write(reg, value);
    } else {
        clock_write(reg, value);
    }
}

static void rtc_access(
    struct vcpu *v, void *ctx, uint16_t offset, size_t size, bool in, uint32_t *value
) {
    (void)v;
    (void)ctx;
    (void)size;
    uint8_t byte = 0xff;
    spinlock_acquire(&lock);
    switch (offset & ~OFFSET_ALIAS) {
        case OFFSET_INDEX: {
            if (!in) {
                index = (uint8_t)*value & INDEX_MASK;
            } else if ((offset & OFFSET_ALIAS) != 0) {
                byte = index;
            }
            break;
        }
        case OFFSET_TARGET: {
            if (in) {
                byte = standard_read(index);
            } else {
                standard_write(index, (uint8_t)*value);
            }
            break;
        }
        case OFFSET_EXT_INDEX: {
            if (!in) {
                ext_index = (uint8_t)*value & INDEX_MASK;
            }
            break;
        }
        default: {
            if (in) {
                byte = ext_ram[ext_index];
            } else {
                ext_ram[ext_index] = (uint8_t)*value;
            }
            break;
        }
    }
    spinlock_release(&lock);
    if (in) {
        *value = (*value & ~(uint32_t)0xff) | byte;
    }
}

static bool fadt_allows(void) {
    struct acpi_fadt *fadt;
    return acpi_tables_ready() && uacpi_table_fadt(&fadt) == UACPI_STATUS_OK
        && (fadt->iapc_boot_arch & ACPI_IA_PC_NO_CMOS_RTC) == 0;
}

// The machine's seconds, once no update holds them.
static uint8_t real_seconds(void) {
    uint64_t end = rdtsc() + time_tsc_hz() / 1000000 * UPDATE_WAIT_US;
    while ((cmos_read(REG_A) & A_UIP) != 0 && rdtsc() < end) {
        cpu_relax();
    }
    return cmos_read(REG_SECONDS);
}

void rtc_sample(void) {
    if (!fadt_allows()) {
        return;
    }
    sampled_seconds = real_seconds();
    sampled_tsc = rdtsc();
    sampled = true;
}

// The firmware has not said the RTC is absent, and it reads back as one that
// runs: its RAM and time valid, its divider running, and its seconds counting.
static bool real_works(void) {
    if (!sampled) {
        return false;
    }
    uint8_t a = cmos_read(REG_A);
    uint8_t d = cmos_read(REG_D);
    if (d == 0xff || (d & D_VRT) == 0 || (a & A_DV_MASK) != A_DV_NORMAL) {
        return false;
    }
    uint64_t end = sampled_tsc + time_tsc_hz() / 1000 * COUNT_WAIT_MS;
    for (;;) {
        if (real_seconds() != sampled_seconds) {
            return true;
        }
        if (rdtsc() >= end) {
            return false;
        }
        time_udelay(1000);
    }
}

static const char *const tad_ids[] = { "ACPI000E", NULL };

static uacpi_iteration_decision tad_found(
    void *user, uacpi_namespace_node *node, uacpi_u32 depth
) {
    (void)depth;
    *(uacpi_namespace_node **)user = node;
    return UACPI_ITERATION_DECISION_BREAK;
}

// The time of the firmware's Time and Alarm Device, as its _GRT gives it (ACPI
// 6.6, 9.17.3).
static bool tad_time(struct civil *c) {
    uacpi_namespace_node *tad = NULL;
    acpi_lock();
    uacpi_find_devices_at(uacpi_namespace_root(), tad_ids, tad_found, &tad);
    uacpi_object *grt = NULL;
    bool got = false;
    if (tad != NULL) {
        uacpi_status status = uacpi_eval_typed(tad, "_GRT", NULL, UACPI_OBJECT_BUFFER_BIT, &grt);
        got = status == UACPI_STATUS_OK;
    }
    uacpi_data_view view = { .data = NULL, .length = 0 };
    if (got && uacpi_object_get_buffer(grt, &view) == UACPI_STATUS_OK && view.length >= 8
     && view.const_bytes[7] == 1) {
        c->year = view.const_bytes[0] | (unsigned int)view.const_bytes[1] << 8;
        c->month = view.const_bytes[2];
        c->day = view.const_bytes[3];
        c->hour = view.const_bytes[4];
        c->minute = view.const_bytes[5];
        c->second = view.const_bytes[6];
    } else {
        got = false;
    }
    if (grt != NULL) {
        uacpi_object_unref(grt);
    }
    acpi_unlock();
    return got && c->year >= 1900 && c->year <= 2099 && c->month >= 1 && c->month <= 12
        && c->day >= 1 && c->day <= 31 && c->hour <= 23 && c->minute <= 59 && c->second <= 59;
}

// From the Time and Alarm Device, else the firmware's GetTime() of the
// loader's, else 2000's first second.
static void clock_start(void) {
    struct civil c = { 2000, 1, 1, 0, 0, 0, 0 };
    uint64_t tsc = rdtsc();
    const struct handoff *h = core_handoff;
    if (tad_time(&c)) {
        printf("The guest's RTC starts from the firmware's Time and Alarm Device\n");
    } else if (h->time_valid) {
        c = (struct civil){
            h->time_year, h->time_month, h->time_day, h->time_hour, h->time_minute,
            h->time_second, 0
        };
        tsc = h->time_tsc;
        printf("The guest's RTC starts from the firmware's GetTime()\n");
    } else {
        printf("The guest's RTC starts from 2000-01-01, as the firmware gave no time\n");
    }
    seconds = seconds_from_civil(&c);
    second_tsc = tsc;
    periodic_tsc = tsc;
    timer.fired = timer_fired;
    hv_timer_add(&timer);
}

// The PC/AT's CMOS RAM, as a BIOS sets it up: an FPU, a VGA, no floppies, the
// memory below and above 1 MiB, and the checksum over 10h-2Dh.
static void ram_init(void) {
    ram[RAM_EQUIPMENT] = EQUIPMENT_FPU;
    ram[RAM_BASE_MEMORY] = (uint8_t)BASE_MEMORY_KIB;
    ram[RAM_BASE_MEMORY + 1] = (uint8_t)(BASE_MEMORY_KIB >> 8);
    uintptr_t extended = (layout_low_ram_end() - ONE_MIB) / 1024;
    if (extended > EXT_MEMORY_KIB_MAX) {
        extended = EXT_MEMORY_KIB_MAX;
    }
    ram[RAM_EXT_MEMORY] = ram[RAM_EXT_MEMORY_POST] = (uint8_t)extended;
    ram[RAM_EXT_MEMORY + 1] = ram[RAM_EXT_MEMORY_POST + 1] = (uint8_t)(extended >> 8);
    unsigned int sum = 0;
    for (size_t i = RAM_CHECKSUM_FIRST; i <= RAM_CHECKSUM_LAST; i++) {
        sum += ram[i];
    }
    ram[RAM_CHECKSUM] = (uint8_t)(sum >> 8);
    ram[RAM_CHECKSUM + 1] = (uint8_t)sum;
    // Where the machine's RTC keeps no century the guest finds, the firmware's
    // year gives it, else 20.
    const struct handoff *h = core_handoff;
    unsigned int hundreds = h->time_valid ? h->time_year / 100 : 20;
    ram[RAM_CENTURY] = (uint8_t)((hundreds / 10) << 4 | hundreds % 10);
}

void rtc_init(void) {
    real = real_works();
    struct acpi_fadt *fadt;
    if (real && uacpi_table_fadt(&fadt) == UACPI_STATUS_OK) {
        century = fadt->century;
        day_alarm_index = fadt->day_alrm;
        month_alarm_index = fadt->mon_alrm;
    }
    if (real) {
        irq_give_isa_line(IRQ_RTC);
    } else {
        // Register D keeps the date alarm (13.6.2.4).
        century = RAM_CENTURY;
        day_alarm_index = REG_D;
        clock_start();
    }
    ram_init();
    printf(
        "The guest's RTC %s\n",
        real ? "keeps the machine's time" : "is emulated, as the machine's does not work"
    );

    struct io_handler ports = { PORT_FIRST, PORT_COUNT, rtc_access, NULL, true };
    io_register(&ports);
}

void rtc_fadt_indexes(uint8_t *century_index, uint8_t *day_alarm, uint8_t *month_alarm) {
    *century_index = century;
    *day_alarm = day_alarm_index;
    *month_alarm = month_alarm_index;
}
