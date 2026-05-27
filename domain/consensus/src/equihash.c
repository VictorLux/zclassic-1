/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2016 Jack Grigg
 * Copyright (c) 2016 The Zcash developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Pure Equihash solution verification. Replays from (header, params)
 * alone. No clock, RNG, allocation (in the domain function itself), or
 * I/O. The underlying crypto primitive equihash_is_valid_solution
 * allocates a fixed-size scratch buffer proportional to 1<<K — that is
 * a deliberate choice of the crypto primitive, confined behind the
 * crypto/ port. Domain replays deterministically from inputs.
 *
 * Mirrors zclassicd src/crypto/equihash.cpp::IsValidSolution + the
 * solution-size → (N,K) demux that historically lived inline in
 * src/main.cpp::CheckEquihashSolution.
 */

#include "domain/consensus/equihash.h"

#include "crypto/blake2b.h"
#include "crypto/equihash.h"
#include "primitives/block.h"

#include <stdint.h>
#include <string.h>

/* Map a serialized solution length to its (N, K) parameter set. The
 * mapping matches the legacy lib/chain/src/equihash.c demux exactly:
 *
 *   1344 bytes -> (200, 9)   ZClassic mainnet
 *     36 bytes -> ( 48, 5)   short regtest
 *     68 bytes -> ( 96, 5)   testnet small
 *    400 bytes -> (192, 7)   testnet large
 *
 * Returns true on a recognised size; false otherwise. */
static bool map_solution_size(size_t sol_size,
                              unsigned int *out_n,
                              unsigned int *out_k)
{
    switch (sol_size) {
        case 1344: *out_n = 200; *out_k = 9; return true;
        case 36:   *out_n =  48; *out_k = 5; return true;
        case 68:   *out_n =  96; *out_k = 5; return true;
        case 400:  *out_n = 192; *out_k = 7; return true;
        default:   return false;
    }
}

/* Little-endian helpers — the Equihash challenge serialization is
 * defined by the consensus rules to be little-endian. We avoid the
 * heap-allocating byte_stream primitive in lib/core/ to keep the
 * domain function observably allocation-free. */
static void le32_into(uint8_t buf[4], uint32_t v)
{
    buf[0] = (uint8_t)(v        & 0xff);
    buf[1] = (uint8_t)((v >>  8) & 0xff);
    buf[2] = (uint8_t)((v >> 16) & 0xff);
    buf[3] = (uint8_t)((v >> 24) & 0xff);
}

struct zcl_result domain_consensus_verify_equihash_solution(
        const struct block_header *header,
        const struct consensus_params *params,
        bool *out_valid)
{
    (void)params; /* Reserved for future activation-gated algorithm switches. */

    if (!header)
        return ZCL_ERR(DOMAIN_CONSENSUS_EQUIHASH_ERR_NULL_HEADER,
                       "verify_equihash_solution: null header");
    if (!out_valid)
        return ZCL_ERR(DOMAIN_CONSENSUS_EQUIHASH_ERR_NULL_OUT,
                       "verify_equihash_solution: null out_valid");

    unsigned int n = 0, k = 0;
    if (!map_solution_size(header->nSolutionSize, &n, &k))
        return ZCL_ERR(DOMAIN_CONSENSUS_EQUIHASH_ERR_BAD_SOL_SIZE,
                       "verify_equihash_solution: unrecognised solution size %zu",
                       header->nSolutionSize);

    /* Re-derive the BLAKE2b ZcashPoW personalised state. */
    struct equihash_params ep;
    equihash_params_init(&ep, n, k);

    struct blake2b_ctx state;
    equihash_initialise_state(&ep, &state);

    /* Feed the header pre-nonce bytes into the state in the canonical
     * little-endian on-wire order. Doing the writes directly (instead
     * of via core/byte_stream) keeps the domain function observably
     * allocation-free. */
    uint8_t scratch[4];

    le32_into(scratch, (uint32_t)header->nVersion);
    blake2b_update(&state, scratch, sizeof(scratch));

    blake2b_update(&state, header->hashPrevBlock.data,        32);
    blake2b_update(&state, header->hashMerkleRoot.data,       32);
    blake2b_update(&state, header->hashFinalSaplingRoot.data, 32);

    le32_into(scratch, header->nTime);
    blake2b_update(&state, scratch, sizeof(scratch));

    le32_into(scratch, header->nBits);
    blake2b_update(&state, scratch, sizeof(scratch));

    /* Append the 32-byte raw nonce. */
    blake2b_update(&state, header->nNonce.data, 32);

    /* The crypto primitive consumes the state-by-value (it copies it
     * before each generate_hash) so we can pass our local state. */
    *out_valid = equihash_is_valid_solution(&ep, &state,
                                            header->nSolution,
                                            header->nSolutionSize);
    return ZCL_OK;
}
