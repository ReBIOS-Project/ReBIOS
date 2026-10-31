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
#include <hw/i8042.h>
#include <lib/log.h>
#include <lib/time.h>
#include <sys/x86.h>

#define PORT_DATA 0x60
#define PORT_COMMAND 0x64

#define STATUS_OUTPUT_FULL 0x01
#define STATUS_INPUT_FULL 0x02
#define STATUS_AUX_DATA 0x20
#define STATUS_ABSENT 0xff

#define COMMAND_READ_CCB 0x20
#define COMMAND_WRITE_CCB 0x60
#define COMMAND_AUX_DISABLE 0xa7
#define COMMAND_AUX_ENABLE 0xa8
#define COMMAND_AUX_TEST 0xa9
#define COMMAND_KBD_DISABLE 0xad
#define COMMAND_KBD_ENABLE 0xae
#define COMMAND_WRITE_AUX 0xd4

#define CCB_KBD_INTERRUPT 0x01
#define CCB_AUX_INTERRUPT 0x02
#define CCB_SYSTEM 0x04
#define CCB_KBD_DISABLED 0x10
#define CCB_AUX_DISABLED 0x20

#define DEVICE_IDENTIFY 0xf2
#define DEVICE_ACK 0xfa

// As Linux 7.3's i8042 driver has it: the buffer it may hold, and how long it
// waits for the controller. Devices get longer to answer.
#define BUFFER_SIZE 16
#define CONTROLLER_TIMEOUT_US 10000
#define DEVICE_TIMEOUT_US 100000
#define POLL_US 50
#define SEND_QUEUE_SIZE 64

struct send {
    uint8_t byte;
    bool aux;
};

static bool present;
static bool keyboard;
static bool aux_device;
static struct send sends[SEND_QUEUE_SIZE];
static size_t send_first;
static size_t send_count;
// The first in the queue has its command to the controller sent.
static bool send_prefixed;

static bool wait_input_empty(void) {
    for (size_t i = 0; i < CONTROLLER_TIMEOUT_US / POLL_US; i++) {
        if ((inb(PORT_COMMAND) & STATUS_INPUT_FULL) == 0) {
            return true;
        }
        time_udelay(POLL_US);
    }
    return false;
}

static bool wait_output(uint8_t *byte, uint64_t timeout_us) {
    for (size_t i = 0; i < timeout_us / POLL_US; i++) {
        if ((inb(PORT_COMMAND) & STATUS_OUTPUT_FULL) != 0) {
            *byte = inb(PORT_DATA);
            return true;
        }
        time_udelay(POLL_US);
    }
    return false;
}

static bool command(uint8_t value) {
    if (!wait_input_empty()) {
        return false;
    }
    outb(PORT_COMMAND, value);
    return true;
}

// A controller that keeps reading full, or reads all ones, is not one.
static bool flush(void) {
    for (size_t i = 0; i < BUFFER_SIZE; i++) {
        uint8_t status = inb(PORT_COMMAND);
        if (status == STATUS_ABSENT) {
            return false;
        }
        if ((status & STATUS_OUTPUT_FULL) == 0) {
            return true;
        }
        inb(PORT_DATA);
        time_udelay(POLL_US);
    }
    return false;
}

static bool identify(bool aux) {
    if ((aux && !command(COMMAND_WRITE_AUX)) || !wait_input_empty()) {
        return false;
    }
    outb(PORT_DATA, DEVICE_IDENTIFY);
    uint8_t reply;
    if (!wait_output(&reply, DEVICE_TIMEOUT_US) || reply != DEVICE_ACK) {
        return false;
    }
    // The identity's bytes, however many this device has.
    while (wait_output(&reply, CONTROLLER_TIMEOUT_US)) {
    }
    return true;
}

void i8042_init(void) {
    if (!flush() || !command(COMMAND_KBD_DISABLE)
     || !command(COMMAND_AUX_DISABLE) || !flush()
     || !command(COMMAND_READ_CCB)) {
        printf("No keyboard controller\n");
        return;
    }
    uint8_t ccb;
    if (!wait_output(&ccb, CONTROLLER_TIMEOUT_US)) {
        printf("No keyboard controller\n");
        return;
    }
    present = true;

    // Bytes as the devices send them, and no interrupts while looking.
    uint8_t off = CCB_SYSTEM | CCB_KBD_DISABLED | CCB_AUX_DISABLED;
    if (command(COMMAND_WRITE_CCB) && wait_input_empty()) {
        outb(PORT_DATA, off);
    }
    command(COMMAND_KBD_ENABLE);
    keyboard = identify(false);
    command(COMMAND_KBD_DISABLE);
    flush();

    command(COMMAND_AUX_ENABLE);
    uint8_t test;
    bool test_passed = command(COMMAND_AUX_TEST)
        && wait_output(&test, CONTROLLER_TIMEOUT_US) && test == 0;
    aux_device = test_passed && identify(true);
    flush();

    uint8_t on = CCB_SYSTEM | (keyboard ? CCB_KBD_INTERRUPT : CCB_KBD_DISABLED)
        | (aux_device ? CCB_AUX_INTERRUPT : CCB_AUX_DISABLED);
    if (command(COMMAND_WRITE_CCB) && wait_input_empty()) {
        outb(PORT_DATA, on);
    }
    printf(
        "Keyboard controller, %s keyboard, %s mouse\n",
        keyboard ? "with a" : "with no", aux_device ? "a" : "no"
    );
}

bool i8042_keyboard(void) {
    return keyboard;
}

bool i8042_aux(void) {
    return aux_device;
}

void i8042_send(bool aux, uint8_t byte) {
    if (present && send_count < SEND_QUEUE_SIZE) {
        size_t at = (send_first + send_count) % SEND_QUEUE_SIZE;
        sends[at] = (struct send){ byte, aux };
        send_count++;
    }
    i8042_send_more();
}

bool i8042_send_more(void) {
    while (send_count != 0) {
        uint8_t status = inb(PORT_COMMAND);
        if (status == STATUS_ABSENT) {
            send_count = 0;
            break;
        }
        if ((status & STATUS_INPUT_FULL) != 0) {
            return true;
        }
        const struct send *s = &sends[send_first];
        if (s->aux && !send_prefixed) {
            outb(PORT_COMMAND, COMMAND_WRITE_AUX);
            send_prefixed = true;
            continue;
        }
        outb(PORT_DATA, s->byte);
        send_prefixed = false;
        send_first = (send_first + 1) % SEND_QUEUE_SIZE;
        send_count--;
    }
    return false;
}

bool i8042_receive(bool *aux, uint8_t *byte) {
    if (!present) {
        return false;
    }
    uint8_t status = inb(PORT_COMMAND);
    if ((status & STATUS_OUTPUT_FULL) == 0 || status == STATUS_ABSENT) {
        return false;
    }
    *aux = (status & STATUS_AUX_DATA) != 0;
    *byte = inb(PORT_DATA);
    return true;
}
