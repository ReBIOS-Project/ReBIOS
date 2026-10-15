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

#include <stdint.h>
#include <stdbool.h>
#include <virt/pic.h>

#define ICW1 0x10
#define ICW1_IC4 0x01
#define ICW1_SNGL 0x02
#define ICW4_AEOI 0x02
#define ICW4_SFNM 0x10
#define OCW3 0x08
#define OCW3_RIS 0x01
#define OCW3_RR 0x02
#define OCW3_P 0x04
#define OCW3_SMM 0x20
#define OCW3_ESMM 0x40
#define POLL_INTERRUPT 0x80

#define CASCADE_IRQ 2
#define DEFAULT_IRQ 7

// Inputs that cannot be level-triggered: IRQs 0, 1, and 2, and 8 and 13.
#define ELCR_MASTER_WRITABLE 0xf8
#define ELCR_SLAVE_WRITABLE 0xde

enum {
    OCW2_ROTATE_AEOI_CLEAR,
    OCW2_EOI,
    OCW2_NOP,
    OCW2_SPECIFIC_EOI,
    OCW2_ROTATE_AEOI_SET,
    OCW2_ROTATE_EOI,
    OCW2_SET_PRIORITY,
    OCW2_ROTATE_SPECIFIC_EOI,
};

struct chip {
    uint8_t isr;
    uint8_t imr;
    // The input lines' levels.
    uint8_t lines;
    // Edge-triggered inputs that rose since their last acknowledge.
    uint8_t edges;
    // Lines a pulse raised, which their acknowledge lowers.
    uint8_t pulses;
    // Inputs acknowledged since their lines last rose.
    uint8_t acked;
    uint8_t elcr;
    uint8_t elcr_writable;
    uint8_t base;
    // The input with the lowest priority.
    uint8_t lowest;
    // The initialisation command word expected next, or 0.
    uint8_t init_step;
    bool icw4;
    bool single;
    bool read_isr;
    bool poll;
    bool special_mask;
    bool auto_eoi;
    bool rotate_auto_eoi;
    bool special_nested;
};

static struct chip chips[2];

// Inputs request while high: level-triggered ones for as long as that,
// edge-triggered ones from a rising edge on, as they must stay high until
// acknowledged.
static uint8_t irr(const struct chip *c) {
    return c->lines & (c->elcr | c->edges);
}

static unsigned priority(const struct chip *c, unsigned irq) {
    return (irq - c->lowest - 1) & 7;
}

// The input of highest priority among bits, or -1.
static int highest(const struct chip *c, uint8_t bits) {
    for (unsigned p = 0; p < 8; p++) {
        unsigned irq = (c->lowest + 1 + p) & 7;
        if (bits & (1u << irq)) {
            return (int)irq;
        }
    }
    return -1;
}

// The input the chip interrupts for, or -1.
static int request(const struct chip *c) {
    int irq = highest(c, irr(c) & ~c->imr);
    if (irq < 0) {
        return -1;
    }
    uint8_t in_service = c->isr;
    if (c->special_mask) {
        in_service &= ~c->imr;
    }
    // In the special fully nested mode, the slave in service does not lock
    // the slave's further requests out.
    if (c == &chips[0] && c->special_nested && irq == CASCADE_IRQ) {
        in_service &= ~(1u << CASCADE_IRQ);
    }
    int current = highest(c, in_service);
    if (current >= 0 && priority(c, (unsigned)current) <= priority(c, (unsigned)irq)) {
        return -1;
    }
    return irq;
}

static void set_line(struct chip *c, unsigned irq, bool level) {
    uint8_t mask = 1u << irq;
    if (level && !(c->lines & mask)) {
        c->edges |= mask;
        c->acked &= ~mask;
    }
    c->lines = level ? c->lines | mask : c->lines & ~mask;
}

static void update(void) {
    set_line(&chips[0], CASCADE_IRQ, request(&chips[1]) >= 0);
}

static void acknowledge(struct chip *c, unsigned irq) {
    uint8_t mask = 1u << irq;
    c->edges &= ~mask;
    c->acked |= mask;
    if (c->pulses & mask) {
        c->pulses &= ~mask;
        c->lines &= ~mask;
    }
    if (!c->auto_eoi) {
        c->isr |= mask;
    } else if (c->rotate_auto_eoi) {
        c->lowest = (uint8_t)irq;
    }
}

void pic_reset(void) {
    for (unsigned i = 0; i < 2; i++) {
        chips[i] = (struct chip){.imr = 0xff, .lowest = 7};
    }
    chips[0].elcr_writable = ELCR_MASTER_WRITABLE;
    chips[1].elcr_writable = ELCR_SLAVE_WRITABLE;
}

static void command(struct chip *c, uint8_t value) {
    if (value & ICW1) {
        c->init_step = 2;
        c->icw4 = value & ICW1_IC4;
        c->single = value & ICW1_SNGL;
        // Edge-triggered inputs need a new edge.
        c->edges = 0;
        c->isr = 0;
        c->imr = 0;
        c->lowest = 7;
        c->read_isr = false;
        c->poll = false;
        c->special_mask = false;
        c->rotate_auto_eoi = false;
        if (!c->icw4) {
            c->auto_eoi = false;
            c->special_nested = false;
        }
        return;
    }
    if (value & OCW3) {
        if (value & OCW3_P) {
            c->poll = true;
        }
        if (value & OCW3_RR) {
            c->read_isr = value & OCW3_RIS;
        }
        if (value & OCW3_ESMM) {
            c->special_mask = value & OCW3_SMM;
        }
        return;
    }

    unsigned level = value & 7;
    switch (value >> 5) {
        case OCW2_ROTATE_AEOI_CLEAR: {
            c->rotate_auto_eoi = false;
            break;
        }
        case OCW2_ROTATE_AEOI_SET: {
            c->rotate_auto_eoi = true;
            break;
        }
        case OCW2_EOI:
        case OCW2_ROTATE_EOI: {
            // A masked IS bit stays set in the special mask mode.
            int irq = highest(c, c->special_mask ? c->isr & ~c->imr : c->isr);
            if (irq >= 0) {
                c->isr &= ~(1u << irq);
                if (value >> 5 == OCW2_ROTATE_EOI) {
                    c->lowest = (uint8_t)irq;
                }
            }
            break;
        }
        case OCW2_SPECIFIC_EOI:
        case OCW2_ROTATE_SPECIFIC_EOI: {
            c->isr &= ~(1u << level);
            if (value >> 5 == OCW2_ROTATE_SPECIFIC_EOI) {
                c->lowest = (uint8_t)level;
            }
            break;
        }
        case OCW2_SET_PRIORITY: {
            c->lowest = (uint8_t)level;
            break;
        }
        default: {
            break;
        }
    }
}

static void data(struct chip *c, uint8_t value) {
    switch (c->init_step) {
        case 2: {
            c->base = value & 0xf8;
            c->init_step = !c->single ? 3 : c->icw4 ? 4 : 0;
            break;
        }
        case 3: {
            // The cascade is fixed: the slave on the master's IRQ 2.
            c->init_step = c->icw4 ? 4 : 0;
            break;
        }
        case 4: {
            c->auto_eoi = value & ICW4_AEOI;
            c->special_nested = value & ICW4_SFNM;
            c->init_step = 0;
            break;
        }
        default: {
            c->imr = value;
            break;
        }
    }
}

static void elcr_write(struct chip *c, uint8_t value) {
    c->elcr = value & c->elcr_writable;
}

void pic_write(uint16_t port, uint8_t value) {
    switch (port) {
        case PIC_MASTER_CMD:
        case PIC_SLAVE_CMD: {
            command(&chips[port == PIC_SLAVE_CMD], value);
            break;
        }
        case PIC_MASTER_DATA:
        case PIC_SLAVE_DATA: {
            data(&chips[port == PIC_SLAVE_DATA], value);
            break;
        }
        case PIC_ELCR_MASTER:
        case PIC_ELCR_SLAVE: {
            elcr_write(&chips[port == PIC_ELCR_SLAVE], value);
            break;
        }
        default: {
            break;
        }
    }
    update();
}

uint8_t pic_read(uint16_t port) {
    uint8_t value = 0;
    switch (port) {
        case PIC_MASTER_CMD:
        case PIC_SLAVE_CMD: {
            struct chip *c = &chips[port == PIC_SLAVE_CMD];
            if (c->poll) {
                // An interrupt acknowledge, the slave's left for its own poll.
                c->poll = false;
                int irq = request(c);
                if (irq >= 0) {
                    acknowledge(c, (unsigned)irq);
                    value = POLL_INTERRUPT | (uint8_t)irq;
                }
            } else {
                value = c->read_isr ? c->isr : irr(c);
            }
            break;
        }
        case PIC_MASTER_DATA:
        case PIC_SLAVE_DATA: {
            value = chips[port == PIC_SLAVE_DATA].imr;
            break;
        }
        case PIC_ELCR_MASTER:
        case PIC_ELCR_SLAVE: {
            value = chips[port == PIC_ELCR_SLAVE].elcr;
            break;
        }
        default: {
            break;
        }
    }
    update();
    return value;
}

void pic_set_irq(unsigned irq, bool level) {
    set_line(&chips[irq >> 3], irq & 7, level);
    update();
}

void pic_pulse_irq(unsigned irq) {
    struct chip *c = &chips[irq >> 3];
    uint8_t mask = 1u << (irq & 7);
    c->lines |= mask;
    c->edges |= mask;
    c->pulses |= mask;
    c->acked &= ~mask;
    update();
}

bool pic_output(void) {
    return request(&chips[0]) >= 0;
}

uint8_t pic_acknowledge(void) {
    struct chip *master = &chips[0];
    struct chip *slave = &chips[1];
    int irq = request(master);
    uint8_t vector;
    if (irq < 0) {
        // Nothing requests any more: the default IRQ 7, with no IS bit.
        vector = master->base + DEFAULT_IRQ;
    } else {
        acknowledge(master, (unsigned)irq);
        if (irq == CASCADE_IRQ) {
            int slave_irq = request(slave);
            if (slave_irq < 0) {
                vector = slave->base + DEFAULT_IRQ;
            } else {
                acknowledge(slave, (unsigned)slave_irq);
                vector = slave->base + (uint8_t)slave_irq;
            }
        } else {
            vector = master->base + (uint8_t)irq;
        }
    }
    update();
    return vector;
}

bool pic_masked(unsigned irq) {
    if (irq >= 8 && (chips[0].imr & (1u << CASCADE_IRQ))) {
        return true;
    }
    return (chips[irq >> 3].imr >> (irq & 7)) & 1;
}

bool pic_in_service(unsigned irq) {
    return (chips[irq >> 3].isr >> (irq & 7)) & 1;
}

bool pic_acknowledged(unsigned irq) {
    return (chips[irq >> 3].acked >> (irq & 7)) & 1;
}

bool pic_level_triggered(unsigned irq) {
    return (chips[irq >> 3].elcr >> (irq & 7)) & 1;
}

bool pic_in_use(void) {
    return chips[0].imr != 0xff;
}
