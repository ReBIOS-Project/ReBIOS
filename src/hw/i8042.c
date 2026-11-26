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
#include <uacpi/acpi.h>
#include <uacpi/status.h>
#include <uacpi/tables.h>
#include <hw/i8042.h>
#include <core.h>
#include <handoff.h>
#include <hw/acpi.h>
#include <lib/log.h>
#include <lib/time.h>
#include <sys/x86.h>

#define STATUS_ABSENT 0xff
// What ACPI 2.0's FADTs are first, Linux 7.3's FADT2_REVISION_ID.
#define FADT_REVISION_ACPI_2 3

#define DEVICE_IDENTIFY 0xf2

// As Linux 7.3's i8042 driver and libps2 have them: the bytes the controller
// may hold; how long the driver waits for the controller, I8042_CTL_TIMEOUT
// polls of 50 us, long enough for the controller to time out sending to a
// mouse that is not there; and how long for a device's answer to a command,
// which ps2_command() gives every command but a reset.
#define BUFFER_SIZE 16
#define CONTROLLER_TIMEOUT_US 500000
#define DEVICE_TIMEOUT_US 500000
#define POLL_US 50
#define SEND_QUEUE_SIZE 64

// A byte for a device, or for the controller's command byte, and the command
// it follows, if any.
struct send {
    uint8_t byte;
    uint8_t command;
};

const char *const i8042_keyboard_ids[] = {
    "PNP0300", "PNP0301", "PNP0302", "PNP0303", "PNP0304", "PNP0305", "PNP0306", "PNP0309",
    "PNP030A", "PNP030B", "PNP0320", "PNP0343", "PNP0344", "PNP0345", "CPQA0D7", NULL
};
const char *const i8042_mouse_ids[] = {
    "AUI0200", "FJC6000", "FJC6001", "PNP0F03", "PNP0F0B", "PNP0F0E", "PNP0F12", "PNP0F13",
    "PNP0F19", "PNP0F1C", "SYN0801", NULL
};

// The chassis types of Linux 7.3's atkbd_is_portable_device(): portable,
// laptop, notebook, sub-notebook, convertible, and detachable (SMBIOS DSP0134
// 3.9.0, Table 17).
static const uint8_t portable_chassis[] = { 8, 9, 10, 14, 31, 32 };

// The first bytes of keyboards' identities, Linux 7.3's ps2_is_keyboard_id().
static const uint8_t keyboard_ids[] = { 0xab, 0xac, 0x2b, 0x5d, 0x60, 0x47 };

static bool present;
static bool keyboard;
static bool aux_device;
static uint8_t ccb;
static struct send sends[SEND_QUEUE_SIZE];
static size_t send_first;
static size_t send_count;
// The first in the queue has its command to the controller sent.
static bool send_prefixed;

static bool wait_input_empty(void) {
    for (size_t i = 0; i < CONTROLLER_TIMEOUT_US / POLL_US; i++) {
        if ((inb(I8042_PORT_COMMAND) & I8042_STATUS_INPUT_FULL) == 0) {
            return true;
        }
        time_udelay(POLL_US);
    }
    return false;
}

static bool wait_output(uint8_t *byte, uint64_t timeout_us) {
    for (size_t i = 0; i < timeout_us / POLL_US; i++) {
        if ((inb(I8042_PORT_COMMAND) & I8042_STATUS_OUTPUT_FULL) != 0) {
            *byte = inb(I8042_PORT_DATA);
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
    outb(I8042_PORT_COMMAND, value);
    return true;
}

// A controller that keeps reading full, or reads all ones, is not one.
static bool flush(void) {
    for (size_t i = 0; i < BUFFER_SIZE; i++) {
        uint8_t status = inb(I8042_PORT_COMMAND);
        if (status == STATUS_ABSENT) {
            return false;
        }
        if ((status & I8042_STATUS_OUTPUT_FULL) == 0) {
            return true;
        }
        inb(I8042_PORT_DATA);
        time_udelay(POLL_US);
    }
    return false;
}

// Whether a device answers the identify command, its identity read as Linux
// 7.3's libps2 reads it: one byte, then a second only where the first is a
// keyboard's, as no other device sends one (ps2_adjust_timeout()).
static bool identify(bool aux) {
    if ((aux && !command(I8042_COMMAND_WRITE_AUX)) || !wait_input_empty()) {
        return false;
    }
    outb(I8042_PORT_DATA, DEVICE_IDENTIFY);
    uint8_t reply;
    if (!wait_output(&reply, DEVICE_TIMEOUT_US) || reply != I8042_DEVICE_ACK) {
        return false;
    }
    if (wait_output(&reply, DEVICE_TIMEOUT_US)) {
        for (size_t i = 0; i < sizeof(keyboard_ids); i++) {
            if (reply == keyboard_ids[i]) {
                wait_output(&reply, DEVICE_TIMEOUT_US);
                break;
            }
        }
    }
    return true;
}

// Whether the firmware's ACPI leaves room for a controller, as Linux 7.3's
// i8042 driver decides before probing one: either of its devices in the
// namespace, or else an FADT from before ACPI 2.0 or one with the 8042 flag
// (ACPI 6.6, Table 5.11). Firmware errs both ways, declaring the devices of a
// controller it lacks and flagging one it disabled, so the probe decides.
static bool firmware_allows(void) {
    if (acpi_has_device(i8042_keyboard_ids) || acpi_has_device(i8042_mouse_ids)) {
        return true;
    }
    struct acpi_fadt *fadt;
    if (!acpi_tables_ready() || uacpi_table_fadt(&fadt) != UACPI_STATUS_OK) {
        return false;
    }
    return fadt->hdr.revision < FADT_REVISION_ACPI_2
        || (fadt->iapc_boot_arch & ACPI_IA_PC_8042) != 0;
}

// As Linux 7.3's atkbd_skip_getid() has it: a portable machine's controller
// that translates, whose keyboard may not take well being asked for its ID.
static bool keyboard_assumed(bool translate) {
    if (!translate) {
        return false;
    }
    for (size_t i = 0; i < sizeof(portable_chassis); i++) {
        if (core_handoff->dmi_chassis_type == portable_chassis[i]) {
            return true;
        }
    }
    return false;
}

void i8042_init(bool translate) {
    if (!firmware_allows()) {
        printf("No keyboard controller, as the firmware's ACPI has it\n");
        return;
    }
    if (!flush() || !command(I8042_COMMAND_KBD_DISABLE) || !command(I8042_COMMAND_AUX_DISABLE)
     || !flush() || !command(I8042_COMMAND_READ_CCB)) {
        printf("No keyboard controller\n");
        return;
    }
    uint8_t firmware_ccb;
    if (!wait_output(&firmware_ccb, CONTROLLER_TIMEOUT_US)) {
        printf("No keyboard controller\n");
        return;
    }
    present = true;

    // The keyboard's bytes translated as the guest asks, as Linux 7.3's i8042
    // driver keeps them translated, since controllers of notebooks may not
    // take it otherwise; and no interrupts while looking.
    uint8_t xlate = translate ? I8042_CCB_TRANSLATE : 0;
    uint8_t off = I8042_CCB_SYSTEM | I8042_CCB_KBD_DISABLED | I8042_CCB_AUX_DISABLED | xlate;
    if (command(I8042_COMMAND_WRITE_CCB) && wait_input_empty()) {
        outb(I8042_PORT_DATA, off);
    }
    if (keyboard_assumed(translate)) {
        keyboard = true;
    } else {
        command(I8042_COMMAND_KBD_ENABLE);
        keyboard = identify(false);
        command(I8042_COMMAND_KBD_DISABLE);
        flush();
    }

    command(I8042_COMMAND_AUX_ENABLE);
    uint8_t test;
    bool test_passed = command(I8042_COMMAND_AUX_TEST) && wait_output(&test, CONTROLLER_TIMEOUT_US)
                    && test == 0;
    aux_device = test_passed && identify(true);
    flush();

    ccb = I8042_CCB_SYSTEM | (keyboard ? I8042_CCB_KBD_INTERRUPT : I8042_CCB_KBD_DISABLED)
        | (aux_device ? I8042_CCB_AUX_INTERRUPT : I8042_CCB_AUX_DISABLED) | xlate;
    if (command(I8042_COMMAND_WRITE_CCB) && wait_input_empty()) {
        outb(I8042_PORT_DATA, ccb);
    }
    printf(
        "Keyboard controller, command byte %#x from the firmware, %s keyboard%s, %s mouse\n",
        firmware_ccb, keyboard ? "with a" : "with no",
        keyboard_assumed(translate) ? " (not asked for its ID, as on a portable machine)" : "",
        aux_device ? "a" : "no"
    );
}

static void send_queue(uint8_t command_byte, uint8_t byte) {
    if (present && send_count < SEND_QUEUE_SIZE) {
        size_t at = (send_first + send_count) % SEND_QUEUE_SIZE;
        sends[at] = (struct send){ byte, command_byte };
        send_count++;
    }
    i8042_send_more();
}

void i8042_translate(bool on) {
    uint8_t want = on ? ccb | I8042_CCB_TRANSLATE : ccb & (uint8_t)~I8042_CCB_TRANSLATE;
    if (!present || want == ccb) {
        return;
    }
    ccb = want;
    send_queue(I8042_COMMAND_WRITE_CCB, ccb);
}

bool i8042_present(void) {
    return present;
}

bool i8042_keyboard(void) {
    return keyboard;
}

bool i8042_aux(void) {
    return aux_device;
}

void i8042_send(bool aux, uint8_t byte) {
    send_queue(aux ? I8042_COMMAND_WRITE_AUX : 0, byte);
}

bool i8042_send_more(void) {
    while (send_count != 0) {
        uint8_t status = inb(I8042_PORT_COMMAND);
        if (status == STATUS_ABSENT) {
            send_count = 0;
            break;
        }
        if ((status & I8042_STATUS_INPUT_FULL) != 0) {
            return true;
        }
        const struct send *s = &sends[send_first];
        if (s->command != 0 && !send_prefixed) {
            outb(I8042_PORT_COMMAND, s->command);
            send_prefixed = true;
            continue;
        }
        outb(I8042_PORT_DATA, s->byte);
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
    uint8_t status = inb(I8042_PORT_COMMAND);
    if ((status & I8042_STATUS_OUTPUT_FULL) == 0 || status == STATUS_ABSENT) {
        return false;
    }
    *aux = (status & I8042_STATUS_AUX_DATA) != 0;
    *byte = inb(I8042_PORT_DATA);
    return true;
}
