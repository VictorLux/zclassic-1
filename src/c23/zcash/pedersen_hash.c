/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Pedersen hash for Sapling Merkle tree — pure C23 implementation.
 * Windowed scalar multiplication on Jubjub generators with BLS12-381 Fr. */

#include "zcash/pedersen_hash.h"
#include "zcash/fr.h"
#include <string.h>

#define PEDERSEN_CHUNKS_PER_GENERATOR 63
#define PEDERSEN_NUM_GENERATORS 5

/* Pre-computed generators derived via group_hash("Zcash_PH", segment_index).
 * Each is a compressed Jubjub point (y-coordinate + sign bit). */
static const uint8_t PEDERSEN_GENERATORS[PEDERSEN_NUM_GENERATORS][32] = {
    /* Generator 0 (counter=5) */
    {0xca,0x3c,0x24,0x32,0xd4,0xab,0xbf,0x77,0x32,0x46,0x4e,0xc0,0x8b,0x2e,0x47,0xf9,
     0x5e,0xdc,0x7e,0x83,0x6b,0x16,0xc9,0x79,0x57,0x1b,0x52,0xd3,0xa2,0x87,0x9e,0xa8},
    /* Generator 1 (counter=0) */
    {0x91,0x18,0xbf,0x4e,0x3c,0xc5,0x0d,0x7b,0xe8,0xd3,0xfa,0x98,0xeb,0xbe,0x3a,0x1f,
     0x25,0xd9,0x01,0xc0,0x42,0x11,0x89,0xf7,0x33,0xfe,0x43,0x5b,0x7f,0x8c,0x5d,0x01},
    /* Generator 2 (counter=0) */
    {0x57,0xd4,0x93,0x97,0x2c,0x50,0xed,0x80,0x98,0xb4,0x84,0x17,0x7f,0x2a,0xb2,0x8b,
     0x53,0xe8,0x8c,0x8e,0x6c,0xa4,0x00,0xe0,0x9e,0xee,0x4e,0xd2,0x00,0x15,0x2e,0xb6},
    /* Generator 3 (counter=0) */
    {0xe9,0x70,0x35,0xa3,0xec,0x4b,0x71,0x84,0x85,0x6a,0x1f,0xa1,0xa1,0xaf,0x03,0x51,
     0xb7,0x47,0xd9,0xd8,0xcb,0x0a,0x07,0x91,0xd8,0xca,0x56,0x4b,0x0c,0xe4,0x7e,0x2f},
    /* Generator 4 (counter=0) */
    {0xef,0x8a,0x65,0xc3,0x99,0x82,0x96,0x99,0x4c,0xd1,0x59,0x58,0x09,0xd8,0xb9,0xb3,
     0xe5,0xc9,0x06,0x14,0x38,0x32,0x78,0x39,0x0a,0x9d,0xab,0x03,0x21,0xc5,0x4b,0xc9},
};

static struct jub_point cached_generators[PEDERSEN_NUM_GENERATORS];
static bool generators_loaded = false;

static void ensure_generators(void)
{
    if (generators_loaded) return;
    for (int i = 0; i < PEDERSEN_NUM_GENERATORS; i++)
        jub_from_bytes(&cached_generators[i], PEDERSEN_GENERATORS[i]);
    generators_loaded = true;
}


void pedersen_merkle_hash(size_t depth,
                           const uint8_t a[32],
                           const uint8_t b[32],
                           uint8_t result[32])
{
    ensure_generators();

    /* Extract bits: 6 personalization + 255 from a + 255 from b = 516 bits */
    uint8_t bits[516];
    int nbits = 0;

    /* Personalization: depth as 6 LE bits */
    for (int i = 0; i < 6; i++)
        bits[nbits++] = (depth >> i) & 1;

    /* a: 255 bits, LE (bit 0 of byte 0 first) */
    for (int i = 0; i < 255; i++) {
        int byte_idx = i / 8;
        int bit_idx = i % 8;
        bits[nbits++] = (a[byte_idx] >> bit_idx) & 1;
    }

    /* b: 255 bits, LE */
    for (int i = 0; i < 255; i++) {
        int byte_idx = i / 8;
        int bit_idx = i % 8;
        bits[nbits++] = (b[byte_idx] >> bit_idx) & 1;
    }

    struct jub_point result_pt;
    jub_identity(&result_pt);

    int bit_pos = 0;
    for (int seg = 0; seg < PEDERSEN_NUM_GENERATORS && bit_pos < nbits; seg++) {
        /* Accumulate scalar in Fs (Jubjub scalar field order), NOT Fr.
         * The Rust code uses E::Fs for this accumulation. */
        struct fs acc, cur, tmp;
        fs_zero(&acc);
        fs_one(&cur);

        bool encountered = false;
        for (int chunk = 0; chunk < PEDERSEN_CHUNKS_PER_GENERATOR; chunk++) {
            if (bit_pos >= nbits) break;
            encountered = true;

            uint8_t a_bit = bits[bit_pos++];
            uint8_t b_bit = (bit_pos < nbits) ? bits[bit_pos++] : 0;
            uint8_t c_bit = (bit_pos < nbits) ? bits[bit_pos++] : 0;

            /* tmp = cur */
            tmp = cur;
            /* if a: tmp += cur (so tmp = 2*cur) */
            if (a_bit) fs_add(&tmp, &tmp, &cur);
            /* cur *= 2 */
            fs_add(&cur, &cur, &cur);
            /* if b: tmp += cur */
            if (b_bit) fs_add(&tmp, &tmp, &cur);
            /* if c: negate */
            if (c_bit) fs_neg(&tmp, &tmp);
            /* acc += tmp */
            fs_add(&acc, &acc, &tmp);

            /* Between chunks (not last): cur *= 8 (three doublings) */
            if (chunk < PEDERSEN_CHUNKS_PER_GENERATOR - 1) {
                fs_add(&cur, &cur, &cur);
                fs_add(&cur, &cur, &cur);
                fs_add(&cur, &cur, &cur);
            }
        }

        if (!encountered) break;

        /* Scalar multiply generator by acc */
        if (!fs_is_zero(&acc)) {
            uint8_t scalar_bytes[32];
            fs_to_bytes(scalar_bytes, &acc);

            struct jub_point scaled;
            jub_scalar_mul(&scaled, &cached_generators[seg], scalar_bytes);
            jub_add(&result_pt, &result_pt, &scaled);
        }
    }

    /* Extract x-coordinate */
    struct fr x_coord;
    jub_get_x(&x_coord, &result_pt);
    fr_to_bytes(result, &x_coord);
}

void sapling_uncommitted(uint8_t out[32])
{
    memset(out, 0, 32);
    out[0] = 1;
}
