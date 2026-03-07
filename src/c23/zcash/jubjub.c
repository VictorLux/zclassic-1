/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php.
 *
 * Jubjub scalar field arithmetic for Sapling.
 * Implements 512-bit reduction modulo the Jubjub scalar field order. */

#include "zcash/jubjub.h"
#include <string.h>

/* Jubjub scalar field order r (little-endian bytes):
 * r = 0x0e7db4ea6533afa906673b0101343b00a6682093ccc81082d0970e5ed6f72cb7 */
static const unsigned char JUBJUB_R[32] = {
    0xb7, 0x2c, 0xf7, 0xd6, 0x5e, 0x0e, 0x97, 0xd0,
    0x82, 0x10, 0xc8, 0xcc, 0x93, 0x20, 0x68, 0xa6,
    0x00, 0x3b, 0x34, 0x01, 0x01, 0x3b, 0x67, 0x06,
    0xa9, 0xaf, 0x33, 0x65, 0xea, 0xb4, 0x7d, 0x0e
};

/* 288-bit big integer (9 x 32-bit limbs, little-endian) */
#define NL 9

struct bigint {
    uint32_t d[NL];
};

static void bi_zero(struct bigint *a)
{
    memset(a->d, 0, sizeof(a->d));
}

static void bi_from_bytes(struct bigint *a, const unsigned char *b, size_t n)
{
    bi_zero(a);
    for (size_t i = 0; i < n && i < NL * 4; i++)
        a->d[i / 4] |= (uint32_t)b[i] << (8 * (i % 4));
}

static void bi_to_bytes(const struct bigint *a, unsigned char *b, size_t n)
{
    for (size_t i = 0; i < n; i++)
        b[i] = (unsigned char)(a->d[i / 4] >> (8 * (i % 4)));
}

/* Compare: returns -1, 0, 1 */
static int bi_cmp(const struct bigint *a, const struct bigint *b)
{
    for (int i = NL - 1; i >= 0; i--) {
        if (a->d[i] < b->d[i]) return -1;
        if (a->d[i] > b->d[i]) return 1;
    }
    return 0;
}

/* a = a - b, assumes a >= b */
static void bi_sub(struct bigint *a, const struct bigint *b)
{
    int64_t borrow = 0;
    for (int i = 0; i < NL; i++) {
        int64_t diff = (int64_t)a->d[i] - (int64_t)b->d[i] - borrow;
        if (diff < 0) {
            diff += (int64_t)1 << 32;
            borrow = 1;
        } else {
            borrow = 0;
        }
        a->d[i] = (uint32_t)diff;
    }
}

/* a = a << 1, returns carry bit */
static uint32_t bi_shl1(struct bigint *a)
{
    uint32_t carry = 0;
    for (int i = 0; i < NL; i++) {
        uint32_t new_carry = a->d[i] >> 31;
        a->d[i] = (a->d[i] << 1) | carry;
        carry = new_carry;
    }
    return carry;
}

/* result = a mod r, where a is 512-bit LE and r is 256-bit.
 * Uses schoolbook shift-and-subtract. Process one bit at a time
 * from MSB of 'a' down, maintaining accumulator < 2*r. */
void jubjub_to_scalar(const unsigned char *input, unsigned char *result)
{
    struct bigint r;
    bi_from_bytes(&r, JUBJUB_R, 32);

    struct bigint acc;
    bi_zero(&acc);

    /* Process 512 bits from MSB (bit 511) to LSB (bit 0) */
    for (int bit = 511; bit >= 0; bit--) {
        /* acc = acc << 1 */
        bi_shl1(&acc);

        /* Add the current bit of input */
        int byte_idx = bit / 8;
        int bit_idx = bit % 8;
        if (input[byte_idx] & (1 << bit_idx))
            acc.d[0] |= 1;

        /* Reduce: if acc >= r, subtract r */
        if (bi_cmp(&acc, &r) >= 0)
            bi_sub(&acc, &r);
    }

    bi_to_bytes(&acc, result, 32);
}
