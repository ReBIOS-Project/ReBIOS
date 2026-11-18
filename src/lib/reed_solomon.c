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

// Parts of this file are derived from reed_solomon.c, encode_rs.c, and
// decode_rs.c, in Linux 7.3's lib/reed_solomon/, for the one code that ReBIOS
// uses, without erasures:
//
// Copyright 2002, Phil Karn, KA9Q
//     May be used under the terms of the GNU General Public License (GPL)
//
// Copyright (C) 2004 Thomas Gleixner (tglx@kernel.org)

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <lib/reed_solomon.h>
#include <memory.h>

// The field's nonzero elements, and in index form, the logarithm of zero.
#define NN 255
#define POLY 0x11d
#define ROOTS REED_SOLOMON_PARITY

static uint8_t alpha_to[NN + 1];
static uint8_t index_of[NN + 1];
// The generator polynomial's coefficients, in index form.
static uint8_t genpoly[ROOTS + 1];

static unsigned int mod_nn(unsigned int x) {
    while (x >= NN) {
        x -= NN;
        x = (x >> 8) + (x & NN);
    }
    return x;
}

// BEGIN Linux 7.3 lib/reed_solomon/reed_solomon.c
void reed_solomon_init(void) {
    index_of[0] = NN;
    alpha_to[NN] = 0;
    unsigned int sr = 1;
    for (size_t i = 0; i < NN; i++) {
        index_of[sr] = (uint8_t)i;
        alpha_to[i] = (uint8_t)sr;
        sr <<= 1;
        if ((sr & 0x100) != 0) {
            sr ^= POLY;
        }
        sr &= NN;
    }
    // The generator's roots are alpha^0 to alpha^(ROOTS - 1).
    uint8_t poly[ROOTS + 1];
    poly[0] = 1;
    for (size_t i = 0; i < ROOTS; i++) {
        poly[i + 1] = 1;
        for (size_t j = i; j > 0; j--) {
            if (poly[j] != 0) {
                poly[j] = poly[j - 1] ^ alpha_to[mod_nn(index_of[poly[j]] + (unsigned int)i)];
            } else {
                poly[j] = poly[j - 1];
            }
        }
        poly[0] = alpha_to[mod_nn(index_of[poly[0]] + (unsigned int)i)];
    }
    for (size_t i = 0; i <= ROOTS; i++) {
        genpoly[i] = index_of[poly[i]];
    }
}
// END Linux 7.3 lib/reed_solomon/reed_solomon.c

// BEGIN Linux 7.3 lib/reed_solomon/encode_rs.c
void reed_solomon_encode(
    const uint8_t *data,
    size_t size,
    uint8_t parity[static REED_SOLOMON_PARITY]
) {
    memset(parity, 0, ROOTS);
    for (size_t i = 0; i < size; i++) {
        unsigned int feedback = index_of[data[i] ^ parity[0]];
        if (feedback != NN) {
            for (size_t j = 1; j < ROOTS; j++) {
                parity[j] ^= alpha_to[mod_nn(feedback + genpoly[ROOTS - j])];
            }
        }
        memmove(&parity[0], &parity[1], ROOTS - 1);
        if (feedback != NN) {
            parity[ROOTS - 1] = alpha_to[mod_nn(feedback + genpoly[0])];
        } else {
            parity[ROOTS - 1] = 0;
        }
    }
}
// END Linux 7.3 lib/reed_solomon/encode_rs.c

// BEGIN Linux 7.3 lib/reed_solomon/decode_rs.c
bool reed_solomon_decode(
    uint8_t *data,
    size_t size,
    uint8_t parity[static REED_SOLOMON_PARITY],
    size_t *corrected
) {
    unsigned int pad = NN - ROOTS - (unsigned int)size;
    unsigned int syn[ROOTS];
    unsigned int lambda[ROOTS + 1];
    unsigned int b[ROOTS + 1];
    unsigned int t[ROOTS + 1];
    unsigned int omega[ROOTS + 1];
    unsigned int root[ROOTS];
    unsigned int reg[ROOTS + 1];
    unsigned int loc[ROOTS];

    // The syndromes: the received word at the generator's roots.
    for (size_t i = 0; i < ROOTS; i++) {
        syn[i] = data[0];
    }
    for (size_t j = 1; j < size + ROOTS; j++) {
        unsigned int symbol = j < size ? data[j] : parity[j - size];
        for (size_t i = 0; i < ROOTS; i++) {
            if (syn[i] == 0) {
                syn[i] = symbol;
            } else {
                syn[i] = symbol ^ alpha_to[mod_nn(index_of[syn[i]] + (unsigned int)i)];
            }
        }
    }
    unsigned int syn_error = 0;
    for (size_t i = 0; i < ROOTS; i++) {
        syn_error |= syn[i];
        syn[i] = index_of[syn[i]];
    }
    if (syn_error == 0) {
        return true;
    }

    // Berlekamp-Massey, for the error locator polynomial.
    memset(lambda, 0, sizeof(lambda));
    lambda[0] = 1;
    for (size_t i = 0; i <= ROOTS; i++) {
        b[i] = index_of[lambda[i]];
    }
    size_t el = 0;
    for (size_t r = 1; r <= ROOTS; r++) {
        unsigned int discr = 0;
        for (size_t i = 0; i < r; i++) {
            if (lambda[i] != 0 && syn[r - i - 1] != NN) {
                discr ^= alpha_to[mod_nn(index_of[lambda[i]] + syn[r - i - 1])];
            }
        }
        discr = index_of[discr];
        if (discr == NN) {
            memmove(&b[1], b, ROOTS * sizeof(unsigned int));
            b[0] = NN;
            continue;
        }
        t[0] = lambda[0];
        for (size_t i = 0; i < ROOTS; i++) {
            if (b[i] != NN) {
                t[i + 1] = lambda[i + 1] ^ alpha_to[mod_nn(discr + b[i])];
            } else {
                t[i + 1] = lambda[i + 1];
            }
        }
        if (2 * el <= r - 1) {
            el = r - el;
            for (size_t i = 0; i <= ROOTS; i++) {
                if (lambda[i] == 0) {
                    b[i] = NN;
                } else {
                    b[i] = mod_nn(index_of[lambda[i]] + NN - discr);
                }
            }
        } else {
            memmove(&b[1], b, ROOTS * sizeof(unsigned int));
            b[0] = NN;
        }
        memcpy(lambda, t, sizeof(lambda));
    }
    size_t deg_lambda = 0;
    for (size_t i = 0; i <= ROOTS; i++) {
        lambda[i] = index_of[lambda[i]];
        if (lambda[i] != NN) {
            deg_lambda = i;
        }
    }
    if (deg_lambda == 0) {
        return false;
    }

    // Chien search, for the locator's roots, and the errors' places.
    memcpy(&reg[1], &lambda[1], ROOTS * sizeof(unsigned int));
    size_t count = 0;
    unsigned int k = 0;
    for (size_t i = 1; i <= NN; i++, k = mod_nn(k + 1)) {
        unsigned int q = 1;
        for (size_t j = deg_lambda; j > 0; j--) {
            if (reg[j] != NN) {
                reg[j] = mod_nn(reg[j] + (unsigned int)j);
                q ^= alpha_to[reg[j]];
            }
        }
        if (q != 0) {
            continue;
        }
        // An error where the shortened code has no symbol.
        if (k < pad) {
            return false;
        }
        root[count] = (unsigned int)i;
        loc[count] = k;
        if (++count == deg_lambda) {
            break;
        }
    }
    if (count != deg_lambda) {
        return false;
    }

    // The error evaluator polynomial, the syndromes times the locator, modulo
    // x^ROOTS, in index form.
    size_t deg_omega = deg_lambda - 1;
    for (size_t i = 0; i <= deg_omega; i++) {
        unsigned int tmp = 0;
        for (size_t j = i + 1; j-- > 0; ) {
            if (syn[i - j] != NN && lambda[j] != NN) {
                tmp ^= alpha_to[mod_nn(syn[i - j] + lambda[j])];
            }
        }
        omega[i] = index_of[tmp];
    }

    // Forney, for the errors' values, kept in b.
    size_t num_corrected = 0;
    for (size_t j = count; j-- > 0; ) {
        unsigned int num1 = 0;
        for (size_t i = deg_omega + 1; i-- > 0; ) {
            if (omega[i] != NN) {
                num1 ^= alpha_to[mod_nn(omega[i] + (unsigned int)i * root[j])];
            }
        }
        if (num1 == 0) {
            b[j] = 0;
            continue;
        }
        unsigned int num2 = alpha_to[mod_nn(NN - root[j])];
        // The locator's formal derivative is in its odd terms.
        unsigned int den = 0;
        size_t last = deg_lambda < ROOTS - 1 ? deg_lambda : ROOTS - 1;
        for (size_t e = last / 2 + 1; e-- > 0; ) {
            size_t i = 2 * e;
            if (lambda[i + 1] != NN) {
                den ^= alpha_to[mod_nn(lambda[i + 1] + (unsigned int)i * root[j])];
            }
        }
        b[j] = alpha_to[mod_nn(index_of[num1] + index_of[num2] + NN - index_of[den])];
        num_corrected++;
    }

    // The errors found must give the syndromes that were received.
    for (size_t i = 0; i < ROOTS; i++) {
        unsigned int tmp = 0;
        for (size_t j = 0; j < count; j++) {
            if (b[j] == 0) {
                continue;
            }
            unsigned int power = (unsigned int)i * (NN - loc[j] - 1);
            tmp ^= alpha_to[mod_nn(index_of[b[j]] + power)];
        }
        if (tmp != alpha_to[syn[i]]) {
            return false;
        }
    }

    for (size_t i = 0; i < count; i++) {
        if (loc[i] < NN - ROOTS) {
            data[loc[i] - pad] ^= (uint8_t)b[i];
        } else {
            parity[loc[i] - pad - size] ^= (uint8_t)b[i];
        }
    }
    *corrected += num_corrected;
    return true;
}
// END Linux 7.3 lib/reed_solomon/decode_rs.c

void reed_solomon_encode_blocks(const uint8_t *data, uint8_t *parity, size_t from, size_t end) {
    for (size_t block = from / REED_SOLOMON_BLOCK; block * REED_SOLOMON_BLOCK < end; block++) {
        reed_solomon_encode(
            data + block * REED_SOLOMON_BLOCK, REED_SOLOMON_BLOCK, parity + block * ROOTS
        );
    }
}

void reed_solomon_decode_blocks(
    uint8_t *data,
    uint8_t *parity,
    size_t count,
    size_t *corrected,
    size_t *failed
) {
    for (size_t block = 0; block < count; block++) {
        uint8_t *at = data + block * REED_SOLOMON_BLOCK;
        if (!reed_solomon_decode(at, REED_SOLOMON_BLOCK, parity + block * ROOTS, corrected)) {
            (*failed)++;
        }
    }
}
