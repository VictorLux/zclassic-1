/* Copyright (c) 2016 Jack Grigg
 * Copyright (c) 2016 The Zcash developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php.
 *
 * Equihash proof-of-work — pure C23 implementation.
 * Based on: Biryukov & Khovratovich, "Equihash: Asymmetric Proof-of-Work
 * Based on the Generalized Birthday Problem", NDSS 2016. */

#ifndef ZCL_CRYPTO_EQUIHASH_H
#define ZCL_CRYPTO_EQUIHASH_H

#include "crypto/blake2b.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint32_t eh_index;
typedef uint8_t eh_trunc;

struct equihash_params {
    unsigned int N;
    unsigned int K;
    size_t indices_per_hash_output;  /* 512/N */
    size_t hash_output;              /* IndicesPerHashOutput * N/8 */
    size_t collision_bit_length;     /* N/(K+1) */
    size_t collision_byte_length;    /* (CollisionBitLength+7)/8 */
    size_t hash_length;              /* (K+1)*CollisionByteLength */
    size_t final_full_width;         /* 2*CollisionByteLength + sizeof(eh_index)*(1<<K) */
    size_t solution_width;           /* (1<<K)*(CollisionBitLength+1)/8 */
};

void equihash_params_init(struct equihash_params *p,
                          unsigned int N, unsigned int K);

int equihash_initialise_state(const struct equihash_params *p,
                              struct blake2b_ctx *state);

bool equihash_is_valid_solution(const struct equihash_params *p,
                                const struct blake2b_ctx *base_state,
                                const unsigned char *soln, size_t soln_len);

void eh_expand_array(const unsigned char *in, size_t in_len,
                     unsigned char *out, size_t out_len,
                     size_t bit_len, size_t byte_pad);

void eh_compress_array(const unsigned char *in, size_t in_len,
                       unsigned char *out, size_t out_len,
                       size_t bit_len, size_t byte_pad);

void eh_index_to_array(eh_index i, unsigned char *array);
eh_index eh_array_to_index(const unsigned char *array);

size_t eh_get_indices_from_minimal(const unsigned char *minimal,
                                   size_t minimal_len,
                                   size_t collision_bit_len,
                                   eh_index *indices_out,
                                   size_t max_indices);

size_t eh_get_minimal_from_indices(const eh_index *indices,
                                   size_t num_indices,
                                   size_t collision_bit_len,
                                   unsigned char *minimal_out,
                                   size_t max_len);

#endif
