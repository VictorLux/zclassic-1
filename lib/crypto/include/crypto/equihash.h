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

/* Map a serialized Equihash solution length to its (N, K) parameter set.
 * Recognised sizes: 1344 -> (200,9), 400 -> (192,7), 68 -> (96,5),
 * 36 -> (48,5). Returns true on a recognised size (and writes *n and *k);
 * false otherwise, including when n or k is NULL. */
bool equihash_solution_params(size_t solution_len,
                              unsigned int *n, unsigned int *k);

int equihash_initialise_state(const struct equihash_params *p,
                              struct blake2b_ctx *state);

bool equihash_is_valid_solution(const struct equihash_params *p,
                                const struct blake2b_ctx *base_state,
                                const unsigned char *soln, size_t soln_len);

/* Generic Wagner-algorithm Equihash solver (BasicSolve).
 *
 * Given the personalised BLAKE2b base_state (already initialised via
 * equihash_initialise_state() and fed the header pre-solution bytes +
 * nonce — exactly the input equihash_is_valid_solution() verifies
 * against), search for ONE valid Equihash answer for the (N,K) in `p`.
 *
 * On success writes the minimal on-wire solution (p->solution_width
 * bytes) to soln_out and returns true. soln_out must have room for at
 * least p->solution_width bytes. Returns false when this challenge has
 * no solution reachable by the basic algorithm (the caller advances the
 * nonce and retries) or on allocation failure.
 *
 * The produced bytes are the same encoding equihash_is_valid_solution()
 * consumes; a true return guarantees that verifier accepts soln_out for
 * the same base_state. This is intended for the small regtest/testnet
 * parameter sets (e.g. (48,5)); it is a correctness-first reference
 * solver, not the optimised mainnet miner in lib/crypto/equihash_solver.c. */
bool equihash_basic_solve(const struct equihash_params *p,
                          const struct blake2b_ctx *base_state,
                          unsigned char *soln_out, size_t soln_out_len);

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
