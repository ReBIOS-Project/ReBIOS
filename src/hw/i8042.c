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
#include <hw/smbios.h>
#include <lib/log.h>
#include <lib/time.h>
#include <lib/util.h>
#include <sys/x86.h>

#define STATUS_ABSENT 0xff
// What ACPI 2.0's FADTs are first, Linux 7.3's FADT2_REVISION_ID.
#define FADT_REVISION_ACPI_2 3

#define DEVICE_IDENTIFY 0xf2

// What Linux 7.3's i8042_check_aux() loops back through the AUX output buffer,
// the AUX test's answers it takes for a port that works, and how often it
// reads the command byte back for its AUX-disable bit to follow.
#define AUX_LOOP_BYTE 0x5a
#define AUX_TEST_OK 0x00
#define AUX_TEST_OK_NOTEBOOK 0xfa
#define AUX_TEST_OK_GENERAL 0xff
#define AUX_TOGGLE_TRIES 100

// As Linux 7.3's i8042_controller_selftest() runs the self-test, as some
// controllers fail it at first: how often, and how long apart; and how long
// its i8042_controller_resume() waits to write the command byte again.
#define SELF_TEST_TRIES 6
#define SELF_TEST_RETRY_US 50000
#define CCB_RETRY_US 50000

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

// The machines whose controller Linux 7.3 leaves as it is across S3, the
// entries with SERIO_QUIRK_FORCENORESTORE in its i8042_dmi_quirk_table
// (drivers/input/serio/i8042-acpipnpio.h), matched as its DMI_MATCH() matches:
// Clevo designs, which their boards' names tell apart. An empty string matches
// anything.
struct no_restore_quirk {
    const char *board_vendor;
    const char *board_name;
    const char *product_name;
};

static const struct no_restore_quirk no_restore_quirks[] = {
    { "TUXEDO", "AURA1501", "" },
    { "TUXEDO", "EDUBOOK1502", "" },
    { "", "LAPQC71A", "" },
    { "", "LAPQC71B", "" },
    { "", "N140CU", "" },
    { "", "N141CU", "" },
    { "", "N150CU", "" },
    { "", "NH5xAx", "" },
    { "", "NHxxRZQ", "" },
    { "", "NL5xRU", "" },
    { "", "NS50MU", "" },
    { "", "NS50_70MU", "" },
    { "", "NJ50_70CU", "" },
    { "", "P640RE", "" },
    { "", "", "P65xH" },
    { "", "", "P65xRP" },
    { "", "", "P65_P67H" },
    { "", "", "P65_67RP" },
    { "", "", "P65_67RS" },
    { "", "", "P67xRP" },
    { "", "PB50_70DFx,DDx", "" },
    { "", "PB51RF", "" },
    { "", "PB71RD", "" },
    { "", "PC70DR", "" },
    { "", "PCX0DX", "" },
    { "", "PCX0DX_GN20", "" },
    { "", "X170SM", "" },
    { "", "X170KM-G", "" }
};

static bool present;
static bool keyboard;
static bool aux_device;
static uint8_t firmware_ccb;
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

// A byte from the controller, false where none comes, or where one that should
// come from the AUX port does not.
static bool wait_output_from(bool aux, uint8_t *byte) {
    for (size_t i = 0; i < CONTROLLER_TIMEOUT_US / POLL_US; i++) {
        uint8_t status = inb(I8042_PORT_COMMAND);
        if ((status & I8042_STATUS_OUTPUT_FULL) != 0) {
            *byte = inb(I8042_PORT_DATA);
            return !aux || (status & I8042_STATUS_AUX_DATA) != 0;
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

static bool write_ccb(uint8_t value) {
    if (!command(I8042_COMMAND_WRITE_CCB) || !wait_input_empty()) {
        return false;
    }
    outb(I8042_PORT_DATA, value);
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

// The AUX port enabled or disabled, and whether the command byte's AUX-disable
// bit follows, which takes some controllers a while (Linux 7.3's
// i8042_toggle_aux()).
static bool aux_toggle(bool on) {
    if (!command(on ? I8042_COMMAND_AUX_ENABLE : I8042_COMMAND_AUX_DISABLE)) {
        return false;
    }
    for (size_t i = 0; i < AUX_TOGGLE_TRIES; i++) {
        time_udelay(POLL_US);
        uint8_t value;
        if (!command(I8042_COMMAND_READ_CCB) || !wait_output(&value, CONTROLLER_TIMEOUT_US)) {
            return false;
        }
        if (((value & I8042_CCB_AUX_DISABLED) == 0) == on) {
            return true;
        }
    }
    return false;
}

// Whether the controller has an AUX port, as Linux 7.3's i8042_check_aux()
// decides, which leaves it enabled: the AUX loopback gives its byte back, or
// else the AUX test answers no error, or one of the errors chipsets report for
// ports that work, which is why it is the second test; then the command byte's
// AUX-disable bit follows the port being enabled. The test of its interrupt
// that follows there needs interrupts, which the probe does not use.
static bool aux_port(void) {
    flush();
    uint8_t loop = 0;
    bool looped = command(I8042_COMMAND_WRITE_AUX_BUFFER) && wait_input_empty();
    if (looped) {
        outb(I8042_PORT_DATA, AUX_LOOP_BYTE);
        looped = wait_output_from(true, &loop) && loop == AUX_LOOP_BYTE;
    }
    if (!looped) {
        uint8_t test;
        if (!command(I8042_COMMAND_AUX_TEST) || !wait_output(&test, CONTROLLER_TIMEOUT_US)) {
            printf("Keyboard controller: no mouse port, as its AUX test went unanswered\n");
            return false;
        }
        if (test != AUX_TEST_OK && test != AUX_TEST_OK_NOTEBOOK && test != AUX_TEST_OK_GENERAL) {
            printf("Keyboard controller: no mouse port, as its AUX test answered %#x\n", test);
            return false;
        }
    }
    if (!aux_toggle(false)) {
        printf("Keyboard controller: its mouse port does not disable\n");
    }
    if (!aux_toggle(true)) {
        printf("Keyboard controller: no mouse port, as it does not enable\n");
        return false;
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
    if (!wait_output(&firmware_ccb, CONTROLLER_TIMEOUT_US)) {
        printf("No keyboard controller\n");
        return;
    }
    present = true;

    // The keyboard's bytes translated as the guest asks, as Linux 7.3's i8042
    // driver keeps them translated, since controllers of notebooks may not
    // take it otherwise; and no interrupts while looking.
    uint8_t xlate = translate ? I8042_CCB_TRANSLATE : 0;
    write_ccb(I8042_CCB_SYSTEM | I8042_CCB_KBD_DISABLED | I8042_CCB_AUX_DISABLED | xlate);
    if (keyboard_assumed(translate)) {
        keyboard = true;
    } else {
        command(I8042_COMMAND_KBD_ENABLE);
        keyboard = identify(false);
        command(I8042_COMMAND_KBD_DISABLE);
        flush();
    }

    aux_device = aux_port() && identify(true);
    flush();

    ccb = I8042_CCB_SYSTEM | (keyboard ? I8042_CCB_KBD_INTERRUPT : I8042_CCB_KBD_DISABLED)
        | (aux_device ? I8042_CCB_AUX_INTERRUPT : I8042_CCB_AUX_DISABLED) | xlate;
    write_ccb(ccb);
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

// Whether Linux 7.3 leaves this machine's controller as it is across S3.
static bool restore_skipped(void) {
    for (size_t i = 0; i < ARRAY_COUNT(no_restore_quirks); i++) {
        const struct no_restore_quirk *q = &no_restore_quirks[i];
        if (smbios_dmi_contains(core_handoff->dmi_board_vendor, q->board_vendor)
         && smbios_dmi_contains(core_handoff->dmi_board_name, q->board_name)
         && smbios_dmi_contains(core_handoff->dmi_product_name, q->product_name)) {
            return true;
        }
    }
    return false;
}

// The controller's self-test, false only where it goes unanswered: as Linux
// 7.3 does on x86, a controller that fails it every time is used anyway, as
// its keyboard may still work.
static bool self_test(void) {
    for (size_t i = 0; i < SELF_TEST_TRIES; i++) {
        uint8_t reply;
        if (!command(I8042_COMMAND_SELF_TEST) || !wait_output(&reply, CONTROLLER_TIMEOUT_US)) {
            printf("Keyboard controller: its self-test went unanswered\n");
            return false;
        }
        if (reply == I8042_SELF_TEST_PASSED) {
            return true;
        }
        time_udelay(SELF_TEST_RETRY_US);
    }
    printf("Keyboard controller: it fails its self-test, and is used anyway\n");
    return true;
}

// The command byte with both ports off and their interrupts with them.
static uint8_t ports_off(uint8_t value) {
    return (value | I8042_CCB_KBD_DISABLED | I8042_CCB_AUX_DISABLED)
         & (uint8_t)~(I8042_CCB_KBD_INTERRUPT | I8042_CCB_AUX_INTERRUPT);
}

// Nothing queued goes any more: before S3, the guest has had the answers it
// waited for, and after it, anything left is stale.
static void sends_drop(void) {
    send_count = 0;
    send_prefixed = false;
}

void i8042_suspend(void) {
    if (!present) {
        return;
    }
    if (restore_skipped()) {
        printf(
            "Keyboard controller: left as it is across S3, as Linux 7.3 leaves this machine's\n"
        );
        return;
    }
    sends_drop();
    flush();
    if (!write_ccb(ports_off(ccb))) {
        printf("Keyboard controller: its command byte does not take before S3\n");
    }
    self_test();
    if (!write_ccb(firmware_ccb)) {
        printf("Keyboard controller: the firmware's command byte does not take before S3\n");
    }
}

void i8042_resume(bool slept) {
    if (!present || restore_skipped()) {
        return;
    }
    sends_drop();
    if (!flush()) {
        printf("Keyboard controller: none found after S3\n");
        return;
    }
    if (slept && !self_test()) {
        return;
    }
    uint8_t value = ports_off(ccb);
    if (!write_ccb(value)) {
        time_udelay(CCB_RETRY_US);
        if (!write_ccb(value)) {
            printf("Keyboard controller: its command byte does not take after S3\n");
            return;
        }
    }
    if (aux_device) {
        value = (value & (uint8_t)~I8042_CCB_AUX_DISABLED) | I8042_CCB_AUX_INTERRUPT;
        if (!write_ccb(value)) {
            printf("Keyboard controller: its mouse port does not enable after S3\n");
        }
    }
    if (keyboard) {
        value = (value & (uint8_t)~I8042_CCB_KBD_DISABLED) | I8042_CCB_KBD_INTERRUPT;
        if (!write_ccb(value)) {
            printf("Keyboard controller: its keyboard port does not enable after S3\n");
        }
    }
}
