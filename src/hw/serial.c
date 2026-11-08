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
#include <hw/serial.h>
#include <sys/x86.h>

#define UART_THR 0
#define UART_DLL 0
#define UART_IER 1
#define UART_DLM 1
#define UART_FCR 2
#define UART_LCR 3
#define UART_MCR 4
#define UART_LSR 5
#define UART_LCR_DLAB 0x80
#define UART_LCR_8N1 0x03
#define UART_LSR_THRE 0x20

#define UART_CLOCK 115200

bool serial_init(uint16_t port, uint32_t baud) {
    // Below 100h are the motherboard's fixed ports, which the probe writes.
    if (port < 0x100 || port > 0xfff8) {
        return false;
    }
    // An absent port floats, where any 8250-compatible UART's divisor latch
    // reads back what was written.
    outb(port + UART_LCR, UART_LCR_DLAB);
    outb(port + UART_DLL, 0xa5);
    outb(port + UART_DLM, 0x5a);
    bool answered = inb(port + UART_DLL) == 0xa5 && inb(port + UART_DLM) == 0x5a;
    outb(port + UART_LCR, 0);
    if (!answered) {
        return false;
    }

    uint32_t divisor = baud == 0 ? 1 : UART_CLOCK / baud;
    if (divisor == 0) {
        divisor = 1;
    }

    // The IER shares its port with the DLM while LCR.DLAB is set.
    outb(port + UART_IER, 0);
    outb(port + UART_LCR, UART_LCR_DLAB);
    outb(port + UART_DLL, divisor & 0xff);
    outb(port + UART_DLM, (divisor >> 8) & 0xff);
    outb(port + UART_LCR, UART_LCR_8N1);
    outb(port + UART_FCR, 0x07);
    outb(port + UART_MCR, 0x03);
    return true;
}

void serial_putc(uint16_t port, char c) {
    // Bounded, so that a port that stops draining cannot hang the caller.
    for (size_t i = 0; i < 100000; i++) {
        if ((inb(port + UART_LSR) & UART_LSR_THRE) != 0) {
            break;
        }
        cpu_relax();
    }
    outb(port + UART_THR, (uint8_t)c);
}
