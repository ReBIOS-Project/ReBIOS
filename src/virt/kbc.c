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
#include <virt/kbc.h>
#include <hw/i8042.h>
#include <hw/power.h>
#include <lib/spinlock.h>
#include <lib/time.h>
#include <sys/x86.h>
#include <virt/a20.h>
#include <virt/hv_timer.h>
#include <virt/io.h>
#include <virt/irq.h>
#include <virt/kbc_keys.h>

struct vcpu;

// The 8042 as the IBM Personal System/2 Hardware Interface Technical
// Reference, Common Interfaces (84F9735) of September 1991 has its Type 1
// controller, and the devices behind it as Andries Brouwer's Keyboard
// scancodes v1.2g has them. Each port's device is the chipset's where it has
// one, whose bytes and commands pass through, and otherwise emulated. USB
// input reaches the guest through it either way, in the format the guest asked
// for.

#define PORT_DATA 0x60
#define PORT_COMMAND 0x64

#define IRQ_KEYBOARD 1
#define IRQ_AUX 12

#define STATUS_OUTPUT_FULL 0x01
#define STATUS_SYSTEM 0x04
#define STATUS_COMMAND 0x08
#define STATUS_NOT_INHIBITED 0x10
#define STATUS_AUX_DATA 0x20

#define CCB_KBD_INTERRUPT 0x01
#define CCB_AUX_INTERRUPT 0x02
#define CCB_SYSTEM 0x04
#define CCB_KBD_DISABLED 0x10
#define CCB_AUX_DISABLED 0x20
#define CCB_TRANSLATE 0x40

#define COMMAND_READ_RAM_FIRST 0x20
#define COMMAND_READ_RAM_LAST 0x3f
#define COMMAND_WRITE_RAM_FIRST 0x60
#define COMMAND_WRITE_RAM_LAST 0x7f
#define COMMAND_RAM_MASK 0x1f
#define COMMAND_VERSION 0xa1
#define COMMAND_PASSWORD_TEST 0xa4
#define COMMAND_PASSWORD_LOAD 0xa5
#define COMMAND_PASSWORD_ENABLE 0xa6
#define COMMAND_AUX_DISABLE 0xa7
#define COMMAND_AUX_ENABLE 0xa8
#define COMMAND_AUX_TEST 0xa9
#define COMMAND_SELF_TEST 0xaa
#define COMMAND_KBD_TEST 0xab
#define COMMAND_KBD_DISABLE 0xad
#define COMMAND_KBD_ENABLE 0xae
#define COMMAND_READ_INPUT 0xc0
#define COMMAND_READ_OUTPUT 0xd0
#define COMMAND_WRITE_OUTPUT 0xd1
#define COMMAND_WRITE_KBD_BUFFER 0xd2
#define COMMAND_WRITE_AUX_BUFFER 0xd3
#define COMMAND_WRITE_AUX 0xd4
#define COMMAND_A20_DISABLE 0xdd
#define COMMAND_A20_ENABLE 0xdf
#define COMMAND_READ_TEST 0xe0
#define COMMAND_PULSE_FIRST 0xf0

#define PASSWORD_NOT_INSTALLED 0xf1
#define SELF_TEST_PASSED 0x55
#define INTERFACE_TEST_PASSED 0x00
// Both data lines idle, and the keyboard not inhibited, as AT controllers had
// it.
#define INPUT_PORT 0x83

#define OUTPUT_RESET_HIGH 0x01
#define OUTPUT_A20 0x02
#define OUTPUT_KBD_FULL 0x10
#define OUTPUT_AUX_FULL 0x20
#define OUTPUT_LINES_HIGH 0xcc

#define EXPECT_NOTHING 0
#define EXPECT_RAM 1
#define EXPECT_OUTPUT 2
#define EXPECT_KBD_BUFFER 3
#define EXPECT_AUX_BUFFER 4
#define EXPECT_AUX 5
#define EXPECT_PASSWORD 6

#define DEVICE_ACK 0xfa
#define DEVICE_RESEND 0xfe
#define DEVICE_SELF_TEST_PASSED 0xaa
#define DEVICE_ID_KEYBOARD_1 0xab
#define DEVICE_ID_KEYBOARD_2 0x83

#define KBD_LEDS 0xed
#define KBD_ECHO 0xee
#define KBD_SCAN_SET 0xf0
#define KBD_IDENTIFY 0xf2
#define KBD_TYPEMATIC 0xf3
#define KBD_ENABLE 0xf4
#define KBD_DISABLE 0xf5
#define KBD_DEFAULTS 0xf6
#define KBD_SET3_FIRST 0xf7
#define KBD_SET3_LAST 0xfd
#define KBD_RESEND 0xfe
#define KBD_RESET 0xff

#define KBD_EXPECT_COMMAND 0
#define KBD_EXPECT_LEDS 1
#define KBD_EXPECT_SCAN_SET 2
#define KBD_EXPECT_TYPEMATIC 3

#define KBD_TYPEMATIC_DEFAULT 0x2b
#define SCAN_BREAK 0xf0
#define SCAN_EXTENDED 0xe0
#define SCAN_PAUSE 0xe1
#define SET1_BREAK 0x80
#define USAGE_PRINT_SCREEN 0x46
#define USAGE_PAUSE 0x48
// Those without a break code, the table's Hangul keys.
#define SCAN_NO_BREAK_FIRST 0xf0

// PS/2 LED bits, and their counterparts in the HID boot keyboard's report.
#define PS2_LED_SCROLL 0x01
#define PS2_LED_NUM 0x02
#define PS2_LED_CAPS 0x04
#define PS2_LEDS (PS2_LED_SCROLL | PS2_LED_NUM | PS2_LED_CAPS)
#define HID_LED_NUM 0x01
#define HID_LED_CAPS 0x02
#define HID_LED_SCROLL 0x04
#define HID_MOD_CTRL 0x11
#define HID_MOD_SHIFT 0x22
#define HID_MOD_ALT 0x44

#define AUX_SCALING_1_1 0xe6
#define AUX_SCALING_2_1 0xe7
#define AUX_RESOLUTION 0xe8
#define AUX_STATUS 0xe9
#define AUX_STREAM 0xea
#define AUX_READ_DATA 0xeb
#define AUX_WRAP_OFF 0xec
#define AUX_WRAP_ON 0xee
#define AUX_REMOTE 0xf0
#define AUX_IDENTIFY 0xf2
#define AUX_SAMPLE_RATE 0xf3
#define AUX_ENABLE 0xf4
#define AUX_DISABLE 0xf5
#define AUX_DEFAULTS 0xf6
#define AUX_RESEND 0xfe
#define AUX_RESET 0xff

#define AUX_EXPECT_COMMAND 0
#define AUX_EXPECT_RESOLUTION 1
#define AUX_EXPECT_SAMPLE_RATE 2

#define AUX_ID_STANDARD 0
#define AUX_ID_WHEEL 3
#define AUX_ID_EXPLORER 4
#define AUX_DEFAULT_RATE 100
#define AUX_DEFAULT_RESOLUTION 2

#define PACKET_ALWAYS 0x08
#define PACKET_X_SIGN 0x10
#define PACKET_Y_SIGN 0x20
#define PACKET_MOVE_MAX 255
#define PACKET_MOVE_MIN (-256)
#define PACKET_BUTTONS 0x07
#define PACKET_BUTTON_4 0x10
#define PACKET_BUTTON_5 0x20
#define BUTTON_4 0x08
#define BUTTON_5 0x10
#define STATUS_BUTTONS_LEFT 0x04
#define STATUS_BUTTONS_MIDDLE 0x02
#define STATUS_BUTTONS_RIGHT 0x01
#define STATUS_ENABLED 0x20
#define STATUS_SCALING 0x10
#define STATUS_REMOTE 0x40

#define QUEUE_SIZE 256
// What reports leave free of the queue, for the mouse's answers to the guest.
#define PACKET_ROOM 16

// The chipset's devices are polled as well, as their lines are masked while
// they are looked at, and oftener while they have bytes yet to take.
#define REAL_POLL_MS 100
#define REAL_SEND_POLL_MS 1
// How much of what they sent one look takes, the rest waiting for the next: a
// controller gone wrong could have more forever.
#define REAL_RECEIVE_MAX 16

#define MS_TSC(ms) (time_tsc_hz() / 1000 * (ms))

struct queue {
    uint8_t bytes[QUEUE_SIZE];
    size_t first;
    size_t count;
};

static struct spinlock lock;
static uint8_t ram[COMMAND_RAM_MASK + 1];
static uint8_t output;
static bool output_full;
static bool output_aux;
static bool keyboard_line;
static bool aux_line;
static bool last_was_command;
static uint8_t output_port = OUTPUT_RESET_HIGH | OUTPUT_A20;
static int expecting;
static size_t expected_ram;
static bool translate_break;
static struct queue controller_queue;
// What D3h asks to come back as the mouse's, whether the mouse is enabled or
// not.
static struct queue loopback_queue;
static struct queue keyboard_queue;
static struct queue aux_queue;

static bool real_keyboard;
static bool real_aux;

// The keyboard, emulated or watched on its way to the chipset's.
static int kbd_expecting;
static bool kbd_enabled = true;
static uint8_t kbd_set = 2;
static uint8_t kbd_leds_set;
static uint8_t kbd_typematic = KBD_TYPEMATIC_DEFAULT;
static uint8_t kbd_last;
static uint16_t kbd_repeating;
static uint8_t kbd_repeat_modifiers;
static struct hv_timer kbd_repeat;

// The mouse, likewise.
static int aux_expecting;
static bool aux_enabled;
static bool aux_remote;
static bool aux_wrap;
static bool aux_scaled;
static uint8_t aux_rate = AUX_DEFAULT_RATE;
static uint8_t aux_resolution = AUX_DEFAULT_RESOLUTION;
static uint8_t aux_id = AUX_ID_STANDARD;
static uint8_t aux_rates[3];
static bool aux_awaiting_id;
static uint8_t aux_last;
static int32_t aux_dx;
static int32_t aux_dy;
static int32_t aux_dz;
static uint32_t aux_buttons;
static bool aux_buttons_changed;

static struct hv_timer real_poll;

// Scan code set 2 to set 1, IBM's Figure 1, and through for what it reserves.
static const uint8_t translation[256] = {
    0xff, 0x43, 0x41, 0x3f, 0x3d, 0x3b, 0x3c, 0x58,
    0x64, 0x44, 0x42, 0x40, 0x3e, 0x0f, 0x29, 0x59,
    0x65, 0x38, 0x2a, 0x70, 0x1d, 0x10, 0x02, 0x5a,
    0x66, 0x71, 0x2c, 0x1f, 0x1e, 0x11, 0x03, 0x5b,
    0x67, 0x2e, 0x2d, 0x20, 0x12, 0x05, 0x04, 0x5c,
    0x68, 0x39, 0x2f, 0x21, 0x14, 0x13, 0x06, 0x5d,
    0x69, 0x31, 0x30, 0x23, 0x22, 0x15, 0x07, 0x5e,
    0x6a, 0x72, 0x32, 0x24, 0x16, 0x08, 0x09, 0x5f,
    0x6b, 0x33, 0x25, 0x17, 0x18, 0x0b, 0x0a, 0x60,
    0x6c, 0x34, 0x35, 0x26, 0x27, 0x19, 0x0c, 0x61,
    0x6d, 0x73, 0x28, 0x74, 0x1a, 0x0d, 0x62, 0x6e,
    0x3a, 0x36, 0x1c, 0x1b, 0x75, 0x2b, 0x63, 0x76,
    0x55, 0x56, 0x77, 0x78, 0x79, 0x7a, 0x0e, 0x7b,
    0x7c, 0x4f, 0x7d, 0x4b, 0x47, 0x7e, 0x7f, 0x6f,
    0x52, 0x53, 0x50, 0x4c, 0x4d, 0x48, 0x01, 0x45,
    0x57, 0x4e, 0x51, 0x4a, 0x37, 0x49, 0x46, 0x54,
    0x80, 0x81, 0x82, 0x41, 0x54, 0x85, 0x86, 0x87,
    0x88, 0x89, 0x8a, 0x8b, 0x8c, 0x8d, 0x8e, 0x8f,
    0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97,
    0x98, 0x99, 0x9a, 0x9b, 0x9c, 0x9d, 0x9e, 0x9f,
    0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
    0xa8, 0xa9, 0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf,
    0xb0, 0xb1, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7,
    0xb8, 0xb9, 0xba, 0xbb, 0xbc, 0xbd, 0xbe, 0xbf,
    0xc0, 0xc1, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7,
    0xc8, 0xc9, 0xca, 0xcb, 0xcc, 0xcd, 0xce, 0xcf,
    0xd0, 0xd1, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7,
    0xd8, 0xd9, 0xda, 0xdb, 0xdc, 0xdd, 0xde, 0xdf,
    0xe0, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7,
    0xe8, 0xe9, 0xea, 0xeb, 0xec, 0xed, 0xee, 0xef,
    0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7,
    0xf8, 0xf9, 0xfa, 0xfb, 0xfc, 0xfd, 0xfe, 0xff,
};

static bool queue_put(struct queue *q, uint8_t byte) {
    if (q->count == QUEUE_SIZE) {
        return false;
    }
    q->bytes[(q->first + q->count) % QUEUE_SIZE] = byte;
    q->count++;
    return true;
}

static bool queue_take(struct queue *q, uint8_t *byte) {
    if (q->count == 0) {
        return false;
    }
    *byte = q->bytes[q->first];
    q->first = (q->first + 1) % QUEUE_SIZE;
    q->count--;
    return true;
}

// The lines follow the output buffer, so that a byte read by polling leaves no
// request behind.
static void lines_update(void) {
    bool keyboard = output_full && !output_aux
        && (ram[0] & CCB_KBD_INTERRUPT) != 0;
    bool aux = output_full && output_aux && (ram[0] & CCB_AUX_INTERRUPT) != 0;
    if (keyboard != keyboard_line) {
        keyboard_line = keyboard;
        irq_isa_set(IRQ_KEYBOARD, keyboard);
    }
    if (aux != aux_line) {
        aux_line = aux;
        irq_isa_set(IRQ_AUX, aux);
    }
}

// Moves what comes next into the output buffer: the controller's answers
// first, as nothing else moves until the guest has them.
static void output_refill(void) {
    while (!output_full) {
        uint8_t byte;
        bool aux = false;
        if (queue_take(&loopback_queue, &byte)) {
            aux = true;
        } else if (!queue_take(&controller_queue, &byte)) {
            if ((ram[0] & CCB_KBD_DISABLED) == 0
             && queue_take(&keyboard_queue, &byte)) {
                if ((ram[0] & CCB_TRANSLATE) != 0) {
                    if (byte == SCAN_BREAK) {
                        translate_break = true;
                        continue;
                    }
                    byte = translation[byte]
                        | (translate_break ? SET1_BREAK : 0);
                    translate_break = false;
                }
            } else if ((ram[0] & CCB_AUX_DISABLED) == 0
                    && queue_take(&aux_queue, &byte)) {
                aux = true;
            } else {
                return;
            }
        }
        output = byte;
        output_full = true;
        output_aux = aux;
        lines_update();
    }
}

static void controller_answer(uint8_t byte) {
    queue_put(&controller_queue, byte);
    output_refill();
}

static uint8_t hid_leds(uint8_t ps2) {
    return (uint8_t)(((ps2 & PS2_LED_SCROLL) != 0 ? HID_LED_SCROLL : 0)
        | ((ps2 & PS2_LED_NUM) != 0 ? HID_LED_NUM : 0)
        | ((ps2 & PS2_LED_CAPS) != 0 ? HID_LED_CAPS : 0));
}

// Answers of the emulated devices: the chipset's give their own.
static void kbd_answer(uint8_t byte) {
    if (!real_keyboard) {
        kbd_last = byte;
        queue_put(&keyboard_queue, byte);
    }
}

static void aux_answer(uint8_t byte) {
    if (!real_aux) {
        aux_last = byte;
        queue_put(&aux_queue, byte);
    }
}

static void kbd_defaults(void) {
    kbd_set = 2;
    kbd_typematic = KBD_TYPEMATIC_DEFAULT;
    kbd_repeating = 0;
    hv_timer_set(&kbd_repeat, 0);
}

static void kbd_command(uint8_t byte) {
    int was = kbd_expecting;
    kbd_expecting = KBD_EXPECT_COMMAND;
    switch (was) {
        case KBD_EXPECT_LEDS: {
            if (byte < KBD_LEDS) {
                kbd_leds_set = byte & PS2_LEDS;
                kbd_answer(DEVICE_ACK);
                return;
            }
            break;
        }
        case KBD_EXPECT_SCAN_SET: {
            if (byte < KBD_LEDS) {
                kbd_answer(DEVICE_ACK);
                // Set 3 is an option real keyboards go without, which then
                // stay in set 2, as the guest finds out by asking.
                if (byte == 0) {
                    kbd_answer(kbd_set);
                } else if (byte == 1 || byte == 2) {
                    kbd_set = byte;
                }
                return;
            }
            break;
        }
        case KBD_EXPECT_TYPEMATIC: {
            if (byte < KBD_LEDS) {
                kbd_typematic = byte;
                kbd_answer(DEVICE_ACK);
                return;
            }
            break;
        }
        default: {
            break;
        }
    }
    switch (byte) {
        case KBD_LEDS: {
            kbd_expecting = KBD_EXPECT_LEDS;
            kbd_answer(DEVICE_ACK);
            break;
        }
        case KBD_ECHO: {
            kbd_answer(KBD_ECHO);
            break;
        }
        case KBD_SCAN_SET: {
            kbd_expecting = KBD_EXPECT_SCAN_SET;
            kbd_answer(DEVICE_ACK);
            break;
        }
        case KBD_IDENTIFY: {
            kbd_answer(DEVICE_ACK);
            kbd_answer(DEVICE_ID_KEYBOARD_1);
            kbd_answer(DEVICE_ID_KEYBOARD_2);
            break;
        }
        case KBD_TYPEMATIC: {
            kbd_expecting = KBD_EXPECT_TYPEMATIC;
            kbd_answer(DEVICE_ACK);
            break;
        }
        case KBD_ENABLE: {
            kbd_enabled = true;
            if (!real_keyboard) {
                keyboard_queue.count = 0;
            }
            kbd_answer(DEVICE_ACK);
            break;
        }
        case KBD_DISABLE: {
            kbd_defaults();
            kbd_enabled = false;
            kbd_leds_set = 0;
            if (!real_keyboard) {
                keyboard_queue.count = 0;
            }
            kbd_answer(DEVICE_ACK);
            break;
        }
        case KBD_DEFAULTS: {
            kbd_defaults();
            kbd_leds_set = 0;
            if (!real_keyboard) {
                keyboard_queue.count = 0;
            }
            kbd_answer(DEVICE_ACK);
            break;
        }
        case KBD_RESEND: {
            if (!real_keyboard) {
                queue_put(&keyboard_queue, kbd_last);
            }
            break;
        }
        case KBD_RESET: {
            kbd_defaults();
            kbd_enabled = true;
            kbd_leds_set = 0;
            if (!real_keyboard) {
                keyboard_queue.count = 0;
            }
            kbd_answer(DEVICE_ACK);
            kbd_answer(DEVICE_SELF_TEST_PASSED);
            break;
        }
        default: {
            // Set 3's per-key attributes, which set 2 does not heed.
            if (byte >= KBD_SET3_FIRST && byte <= KBD_SET3_LAST) {
                kbd_answer(DEVICE_ACK);
            } else {
                kbd_answer(DEVICE_RESEND);
            }
            break;
        }
    }
}

static void aux_defaults(void) {
    aux_enabled = false;
    aux_remote = false;
    aux_scaled = false;
    aux_rate = AUX_DEFAULT_RATE;
    aux_resolution = AUX_DEFAULT_RESOLUTION;
    aux_dx = 0;
    aux_dy = 0;
    aux_dz = 0;
    aux_buttons_changed = false;
}

static uint8_t aux_packet_buttons(void) {
    return (uint8_t)(aux_buttons & PACKET_BUTTONS);
}

// 2:1 scaling, which only stream mode's reports get.
static int32_t scaled(int32_t move) {
    static const int32_t low[] = { 0, 1, 1, 3, 6, 9 };
    int32_t magnitude = move < 0 ? -move : move;
    int32_t result = magnitude < 6 ? low[(size_t)magnitude] : magnitude * 2;
    return move < 0 ? -result : result;
}

static int32_t clamp_move(int32_t move) {
    if (move > PACKET_MOVE_MAX) {
        return PACKET_MOVE_MAX;
    }
    return move < PACKET_MOVE_MIN ? PACKET_MOVE_MIN : move;
}

static void aux_packet(bool stream) {
    int32_t dx = clamp_move(aux_dx);
    int32_t dy = clamp_move(aux_dy);
    aux_dx -= dx;
    aux_dy -= dy;
    int32_t dz = aux_dz > 7 ? 7 : aux_dz < -8 ? -8 : aux_dz;
    aux_dz -= dz;
    if (stream && aux_scaled) {
        dx = clamp_move(scaled(dx));
        dy = clamp_move(scaled(dy));
    }
    uint8_t flags = (uint8_t)(PACKET_ALWAYS | aux_packet_buttons()
        | (dx < 0 ? PACKET_X_SIGN : 0) | (dy < 0 ? PACKET_Y_SIGN : 0));
    queue_put(&aux_queue, flags);
    queue_put(&aux_queue, (uint8_t)dx);
    queue_put(&aux_queue, (uint8_t)dy);
    if (aux_id == AUX_ID_WHEEL) {
        queue_put(&aux_queue, (uint8_t)dz);
    } else if (aux_id == AUX_ID_EXPLORER) {
        uint8_t extra = (uint8_t)((dz & 0x0f)
            | ((aux_buttons & BUTTON_4) != 0 ? PACKET_BUTTON_4 : 0)
            | ((aux_buttons & BUTTON_5) != 0 ? PACKET_BUTTON_5 : 0));
        queue_put(&aux_queue, extra);
    }
    aux_buttons_changed = false;
}

// What has accumulated, as far as there is room: more goes once the guest
// reads.
static void aux_flush(void) {
    if (!aux_enabled || aux_remote || aux_wrap) {
        return;
    }
    while ((aux_dx != 0 || aux_dy != 0 || aux_dz != 0 || aux_buttons_changed)
        && aux_queue.count + PACKET_ROOM <= QUEUE_SIZE) {
        aux_packet(true);
    }
}

// The magic sequences of sample rates that switch a mouse to more bytes
// (Andries Brouwer's Keyboard scancodes v1.2g, 13.3).
static void aux_rate_set(uint8_t rate) {
    aux_rate = rate;
    aux_rates[0] = aux_rates[1];
    aux_rates[1] = aux_rates[2];
    aux_rates[2] = rate;
    if (aux_rates[0] == 200 && aux_rates[1] == 100 && aux_rates[2] == 80) {
        aux_id = AUX_ID_WHEEL;
    } else if (aux_rates[0] == 200 && aux_rates[1] == 200
            && aux_rates[2] == 80 && aux_id == AUX_ID_WHEEL) {
        aux_id = AUX_ID_EXPLORER;
    }
}

static void aux_command(uint8_t byte) {
    if (aux_wrap && byte != AUX_WRAP_OFF && byte != AUX_RESET) {
        aux_answer(byte);
        return;
    }
    int was = aux_expecting;
    aux_expecting = AUX_EXPECT_COMMAND;
    if (was == AUX_EXPECT_RESOLUTION) {
        aux_resolution = byte & 3;
        aux_answer(DEVICE_ACK);
        return;
    }
    if (was == AUX_EXPECT_SAMPLE_RATE) {
        aux_rate_set(byte);
        aux_answer(DEVICE_ACK);
        return;
    }
    switch (byte) {
        case AUX_SCALING_1_1: {
            aux_scaled = false;
            aux_answer(DEVICE_ACK);
            break;
        }
        case AUX_SCALING_2_1: {
            aux_scaled = true;
            aux_answer(DEVICE_ACK);
            break;
        }
        case AUX_RESOLUTION: {
            aux_expecting = AUX_EXPECT_RESOLUTION;
            aux_answer(DEVICE_ACK);
            break;
        }
        case AUX_STATUS: {
            uint8_t status = (uint8_t)((aux_remote ? STATUS_REMOTE : 0)
                | (aux_enabled ? STATUS_ENABLED : 0)
                | (aux_scaled ? STATUS_SCALING : 0)
                | ((aux_buttons & 1) != 0 ? STATUS_BUTTONS_LEFT : 0)
                | ((aux_buttons & 2) != 0 ? STATUS_BUTTONS_RIGHT : 0)
                | ((aux_buttons & 4) != 0 ? STATUS_BUTTONS_MIDDLE : 0));
            aux_answer(DEVICE_ACK);
            aux_answer(status);
            aux_answer(aux_resolution);
            aux_answer(aux_rate);
            break;
        }
        case AUX_STREAM: {
            aux_remote = false;
            aux_answer(DEVICE_ACK);
            break;
        }
        case AUX_READ_DATA: {
            aux_answer(DEVICE_ACK);
            if (!real_aux) {
                aux_packet(false);
            }
            break;
        }
        case AUX_WRAP_OFF: {
            aux_wrap = false;
            aux_answer(DEVICE_ACK);
            break;
        }
        case AUX_WRAP_ON: {
            aux_wrap = true;
            aux_answer(DEVICE_ACK);
            break;
        }
        case AUX_REMOTE: {
            aux_remote = true;
            aux_answer(DEVICE_ACK);
            break;
        }
        case AUX_IDENTIFY: {
            aux_awaiting_id = real_aux;
            aux_answer(DEVICE_ACK);
            aux_answer(aux_id);
            break;
        }
        case AUX_SAMPLE_RATE: {
            aux_expecting = AUX_EXPECT_SAMPLE_RATE;
            aux_answer(DEVICE_ACK);
            break;
        }
        case AUX_ENABLE: {
            aux_enabled = true;
            aux_answer(DEVICE_ACK);
            break;
        }
        case AUX_DISABLE: {
            aux_enabled = false;
            aux_answer(DEVICE_ACK);
            break;
        }
        case AUX_DEFAULTS: {
            aux_defaults();
            aux_answer(DEVICE_ACK);
            break;
        }
        case AUX_RESEND: {
            if (!real_aux) {
                queue_put(&aux_queue, aux_last);
            }
            break;
        }
        case AUX_RESET: {
            aux_defaults();
            aux_wrap = false;
            aux_id = AUX_ID_STANDARD;
            if (!real_aux) {
                aux_queue.count = 0;
            }
            aux_answer(DEVICE_ACK);
            aux_answer(DEVICE_SELF_TEST_PASSED);
            aux_answer(AUX_ID_STANDARD);
            break;
        }
        default: {
            aux_answer(DEVICE_RESEND);
            break;
        }
    }
}

static void output_port_write(uint8_t value) {
    output_port = value;
    a20_kbc_set((value & OUTPUT_A20) != 0);
    // Held in reset while it stays low, for which there is no coming back.
    if ((value & OUTPUT_RESET_HIGH) == 0) {
        power_reset(false);
    }
}

static uint8_t output_port_read(void) {
    uint8_t value = (output_port & (OUTPUT_RESET_HIGH | OUTPUT_A20))
        | OUTPUT_LINES_HIGH;
    if (output_full) {
        value |= output_aux ? OUTPUT_AUX_FULL : OUTPUT_KBD_FULL;
    }
    return value;
}

static void command_write(uint8_t value) {
    expecting = EXPECT_NOTHING;
    if (value >= COMMAND_READ_RAM_FIRST && value <= COMMAND_READ_RAM_LAST) {
        controller_answer(ram[value & COMMAND_RAM_MASK]);
        return;
    }
    if (value >= COMMAND_WRITE_RAM_FIRST && value <= COMMAND_WRITE_RAM_LAST) {
        expecting = EXPECT_RAM;
        expected_ram = value & COMMAND_RAM_MASK;
        return;
    }
    if (value >= COMMAND_PULSE_FIRST) {
        // Only the reset line reaches anything still.
        if ((value & OUTPUT_RESET_HIGH) == 0) {
            power_reset(false);
        }
        return;
    }
    switch (value) {
        case COMMAND_VERSION:
        case COMMAND_READ_TEST: {
            controller_answer(0);
            break;
        }
        case COMMAND_PASSWORD_TEST: {
            controller_answer(PASSWORD_NOT_INSTALLED);
            break;
        }
        case COMMAND_PASSWORD_LOAD: {
            expecting = EXPECT_PASSWORD;
            break;
        }
        case COMMAND_AUX_DISABLE: {
            ram[0] |= CCB_AUX_DISABLED;
            break;
        }
        case COMMAND_AUX_ENABLE: {
            ram[0] &= (uint8_t)~CCB_AUX_DISABLED;
            output_refill();
            break;
        }
        case COMMAND_AUX_TEST:
        case COMMAND_KBD_TEST: {
            controller_answer(INTERFACE_TEST_PASSED);
            break;
        }
        case COMMAND_SELF_TEST: {
            ram[0] |= CCB_SYSTEM;
            controller_answer(SELF_TEST_PASSED);
            break;
        }
        case COMMAND_KBD_DISABLE: {
            ram[0] |= CCB_KBD_DISABLED;
            break;
        }
        case COMMAND_KBD_ENABLE: {
            ram[0] &= (uint8_t)~CCB_KBD_DISABLED;
            output_refill();
            break;
        }
        case COMMAND_READ_INPUT: {
            controller_answer(INPUT_PORT);
            break;
        }
        case COMMAND_READ_OUTPUT: {
            controller_answer(output_port_read());
            break;
        }
        case COMMAND_WRITE_OUTPUT: {
            expecting = EXPECT_OUTPUT;
            break;
        }
        case COMMAND_WRITE_KBD_BUFFER: {
            expecting = EXPECT_KBD_BUFFER;
            break;
        }
        case COMMAND_WRITE_AUX_BUFFER: {
            expecting = EXPECT_AUX_BUFFER;
            break;
        }
        case COMMAND_WRITE_AUX: {
            expecting = EXPECT_AUX;
            break;
        }
        case COMMAND_A20_DISABLE: {
            output_port &= (uint8_t)~OUTPUT_A20;
            a20_kbc_set(false);
            break;
        }
        case COMMAND_A20_ENABLE: {
            output_port |= OUTPUT_A20;
            a20_kbc_set(true);
            break;
        }
        default: {
            break;
        }
    }
}

static void ccb_write(uint8_t value) {
    ram[0] = value;
    lines_update();
    output_refill();
}

static void real_send(bool aux, uint8_t byte) {
    i8042_send(aux, byte);
    if (i8042_send_more()) {
        hv_timer_set(&real_poll, rdtsc() + MS_TSC(REAL_SEND_POLL_MS));
    }
}

static void data_write(uint8_t value) {
    int was = expecting;
    expecting = EXPECT_NOTHING;
    switch (was) {
        case EXPECT_RAM: {
            if (expected_ram == 0) {
                ccb_write(value);
            } else {
                ram[expected_ram] = value;
            }
            break;
        }
        case EXPECT_OUTPUT: {
            output_port_write(value);
            break;
        }
        case EXPECT_KBD_BUFFER: {
            queue_put(&controller_queue, value);
            output_refill();
            break;
        }
        case EXPECT_AUX_BUFFER: {
            queue_put(&loopback_queue, value);
            output_refill();
            break;
        }
        case EXPECT_PASSWORD: {
            // A password never takes: the test says that none is installed.
            if (value != 0) {
                expecting = EXPECT_PASSWORD;
            }
            break;
        }
        case EXPECT_AUX: {
            // Sending data reenables the interface, as in QEMU 11.1.2's 8042.
            ram[0] &= (uint8_t)~CCB_AUX_DISABLED;
            aux_command(value);
            if (real_aux) {
                real_send(true, value);
            }
            output_refill();
            break;
        }
        default: {
            ram[0] &= (uint8_t)~CCB_KBD_DISABLED;
            kbd_command(value);
            if (real_keyboard) {
                real_send(false, value);
            }
            output_refill();
            break;
        }
    }
}

static void data_access(
    struct vcpu *v, void *ctx, uint16_t offset, size_t size, bool in,
    uint32_t *value
) {
    (void)v;
    (void)ctx;
    (void)offset;
    spinlock_acquire(&lock);
    if (in) {
        // Without anything new, the last byte reads again.
        uint8_t byte = output;
        output_full = false;
        lines_update();
        aux_flush();
        output_refill();
        *value = (*value & ~(uint32_t)0xff) | byte;
    } else {
        last_was_command = false;
        data_write((uint8_t)*value);
    }
    for (size_t i = 1; i < size; i++) {
        uint16_t port = (uint16_t)(PORT_DATA + i);
        if (in) {
            *value = (*value & ~((uint32_t)0xff << (8 * i)))
                | (uint32_t)inb(port) << (8 * i);
        } else {
            outb(port, (uint8_t)(*value >> (8 * i)));
        }
    }
    spinlock_release(&lock);
}

static void command_access(
    struct vcpu *v, void *ctx, uint16_t offset, size_t size, bool in,
    uint32_t *value
) {
    (void)v;
    (void)ctx;
    (void)offset;
    spinlock_acquire(&lock);
    if (in) {
        uint8_t status = STATUS_NOT_INHIBITED | (ram[0] & CCB_SYSTEM)
            | (last_was_command ? STATUS_COMMAND : 0)
            | (output_full ? STATUS_OUTPUT_FULL : 0)
            | (output_full && output_aux ? STATUS_AUX_DATA : 0);
        *value = (*value & ~(uint32_t)0xff) | status;
    } else {
        last_was_command = true;
        command_write((uint8_t)*value);
    }
    for (size_t i = 1; i < size; i++) {
        uint16_t port = (uint16_t)(PORT_COMMAND + i);
        if (in) {
            *value = (*value & ~((uint32_t)0xff << (8 * i)))
                | (uint32_t)inb(port) << (8 * i);
        } else {
            outb(port, (uint8_t)(*value >> (8 * i)));
        }
    }
    spinlock_release(&lock);
}

// A key's bytes in the scan code set the keyboard is in, make or break.
static void key_bytes(uint16_t usage, bool pressed, uint8_t modifiers) {
    bool set1 = kbd_set == 1;
    struct queue *q = &keyboard_queue;
    if (usage == USAGE_PAUSE) {
        if (!pressed) {
            return;
        }
        static const uint8_t pause_set1[] = {
            SCAN_PAUSE, 0x1d, 0x45, SCAN_PAUSE, 0x9d, 0xc5
        };
        static const uint8_t pause_set2[] = {
            SCAN_PAUSE, 0x14, 0x77, SCAN_PAUSE, SCAN_BREAK, 0x14, SCAN_BREAK,
            0x77
        };
        static const uint8_t break_set1[] = {
            SCAN_EXTENDED, 0x46, SCAN_EXTENDED, 0xc6
        };
        static const uint8_t break_set2[] = {
            SCAN_EXTENDED, 0x7e, SCAN_EXTENDED, SCAN_BREAK, 0x7e
        };
        bool ctrl = (modifiers & HID_MOD_CTRL) != 0;
        const uint8_t *bytes = ctrl ? (set1 ? break_set1 : break_set2)
            : (set1 ? pause_set1 : pause_set2);
        size_t count = ctrl ? (set1 ? sizeof(break_set1) : sizeof(break_set2))
            : (set1 ? sizeof(pause_set1) : sizeof(pause_set2));
        for (size_t i = 0; i < count; i++) {
            queue_put(q, bytes[i]);
        }
        return;
    }
    struct kbc_key key = usage < KBC_KEYS ? kbc_keys[usage]
        : (struct kbc_key){ 0 };
    uint8_t code = set1 ? key.set1 : key.set2;
    if (code == 0) {
        return;
    }
    if (usage == USAGE_PRINT_SCREEN) {
        // Alt makes it SysRq, and Ctrl or Shift drop its fake shift
        // (Microsoft's Keyboard Scan Code Specification 1.3a).
        if ((modifiers & HID_MOD_ALT) != 0) {
            if (!pressed && !set1) {
                queue_put(q, SCAN_BREAK);
            }
            queue_put(q, set1 ? (pressed ? 0x54 : 0xd4) : 0x84);
            return;
        }
        if ((modifiers & (HID_MOD_CTRL | HID_MOD_SHIFT)) == 0 && pressed) {
            queue_put(q, SCAN_EXTENDED);
            queue_put(q, set1 ? 0x2a : 0x12);
        }
    }
    if (code >= SCAN_NO_BREAK_FIRST && !pressed) {
        return;
    }
    if (key.extended) {
        queue_put(q, SCAN_EXTENDED);
    }
    if (pressed) {
        queue_put(q, code);
    } else if (set1) {
        queue_put(q, code | SET1_BREAK);
    } else {
        queue_put(q, SCAN_BREAK);
        queue_put(q, code);
    }
    if (usage == USAGE_PRINT_SCREEN && !pressed
     && (modifiers & (HID_MOD_CTRL | HID_MOD_SHIFT)) == 0) {
        queue_put(q, SCAN_EXTENDED);
        if (set1) {
            queue_put(q, 0xaa);
        } else {
            queue_put(q, SCAN_BREAK);
            queue_put(q, 0x12);
        }
    }
}

// The typematic byte's delay and rate (Andries Brouwer's Keyboard scancodes
// v1.2g, 12.1): (2 ^ B) * (D + 8) / 240 seconds between characters.
static uint64_t typematic_delay(void) {
    return MS_TSC(((uint64_t)((kbd_typematic >> 5) & 3) + 1) * 250);
}

static uint64_t typematic_period(void) {
    uint64_t b = (kbd_typematic >> 3) & 3;
    uint64_t d = kbd_typematic & 7;
    return ((uint64_t)1 << b) * (d + 8) * time_tsc_hz() / 240;
}

static void kbd_repeat_fired(void *ctx) {
    (void)ctx;
    spinlock_acquire(&lock);
    if (kbd_repeating != 0 && kbd_enabled) {
        key_bytes(kbd_repeating, true, kbd_repeat_modifiers);
        output_refill();
        hv_timer_set(&kbd_repeat, rdtsc() + typematic_period());
    }
    spinlock_release(&lock);
}

void kbc_key(uint16_t usage, bool pressed, uint8_t modifiers) {
    spinlock_acquire(&lock);
    if (kbd_enabled) {
        key_bytes(usage, pressed, modifiers);
        output_refill();
    }
    if (pressed && usage != USAGE_PAUSE) {
        kbd_repeating = usage;
        kbd_repeat_modifiers = modifiers;
        hv_timer_set(&kbd_repeat, rdtsc() + typematic_delay());
    } else if (!pressed && usage == kbd_repeating) {
        kbd_repeating = 0;
        hv_timer_set(&kbd_repeat, 0);
    }
    spinlock_release(&lock);
}

void kbc_pointer(int32_t dx, int32_t dy, int32_t wheel, uint32_t buttons) {
    spinlock_acquire(&lock);
    if (aux_enabled || aux_remote) {
        aux_dx += dx;
        // PS/2's grows upwards, and its wheel towards the user.
        aux_dy -= dy;
        aux_dz -= wheel;
    }
    if (buttons != aux_buttons) {
        aux_buttons = buttons;
        aux_buttons_changed = true;
    }
    aux_flush();
    output_refill();
    spinlock_release(&lock);
}

uint8_t kbc_leds(void) {
    spinlock_acquire(&lock);
    uint8_t leds = hid_leds(kbd_leds_set);
    spinlock_release(&lock);
    return leds;
}

// What the chipset's devices sent.
static void real_receive(void) {
    bool aux;
    uint8_t byte;
    spinlock_acquire(&lock);
    for (size_t i = 0; i < REAL_RECEIVE_MAX && i8042_receive(&aux, &byte);
         i++) {
        if (aux) {
            // The chipset's mouse may not take what it was asked to, which the
            // USB one's reports need to know.
            if (aux_awaiting_id && byte != DEVICE_ACK) {
                aux_id = byte;
                aux_awaiting_id = false;
            }
            queue_put(&aux_queue, byte);
        } else {
            queue_put(&keyboard_queue, byte);
        }
    }
    output_refill();
    spinlock_release(&lock);
}

static void real_interrupt(void *ctx) {
    (void)ctx;
    real_receive();
}

static void real_poll_fired(void *ctx) {
    (void)ctx;
    real_receive();
    spinlock_acquire(&lock);
    bool more = i8042_send_more();
    spinlock_release(&lock);
    uint64_t wait = MS_TSC(more ? REAL_SEND_POLL_MS : REAL_POLL_MS);
    hv_timer_set(&real_poll, rdtsc() + wait);
}

void kbc_init(void) {
    ram[0] = CCB_KBD_INTERRUPT | CCB_SYSTEM | CCB_TRANSLATE;
    i8042_init();
    real_keyboard = i8042_keyboard();
    real_aux = i8042_aux();

    kbd_repeat.fired = kbd_repeat_fired;
    hv_timer_add(&kbd_repeat);
    if (real_keyboard || real_aux) {
        if (real_keyboard) {
            irq_take_host_line(
                irq_isa_gsi(IRQ_KEYBOARD), real_interrupt, NULL
            );
        }
        if (real_aux) {
            irq_take_host_line(irq_isa_gsi(IRQ_AUX), real_interrupt, NULL);
        }
        real_poll.fired = real_poll_fired;
        hv_timer_add(&real_poll);
        hv_timer_set(&real_poll, rdtsc() + MS_TSC(REAL_POLL_MS));
    }

    struct io_handler data = { PORT_DATA, 1, data_access, NULL };
    io_register(&data);
    struct io_handler command = { PORT_COMMAND, 1, command_access, NULL };
    io_register(&command);
}
