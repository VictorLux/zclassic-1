/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php.
 *
 * Equihash solution verification for block headers.
 * Uses the pure C23 blake2b-based equihash implementation. */

#include "chain/equihash.h"
#include "crypto/equihash.h"
#include "core/serialize.h"
#include <string.h>

bool check_equihash_solution(const struct block_header *header,
                             const struct chain_params *params)
{
    size_t sol_size = header->nSolutionSize;
    if (sol_size == 0)
        return false;

    unsigned int n, k;
    if (sol_size == 1344) {
        n = 200; k = 9;
    } else if (sol_size == 36) {
        n = 48; k = 5;
    } else if (sol_size == 68) {
        n = 96; k = 5;
    } else if (sol_size == 400) {
        n = 192; k = 7;
    } else {
        return false;
    }

    (void)params;

    struct equihash_params ep;
    equihash_params_init(&ep, n, k);

    struct blake2b_ctx state;
    equihash_initialise_state(&ep, &state);

    /* Hash I||V where I = block header minus nonce and solution,
     * V = nonce. We serialize the header fields that come before
     * the nonce, then append the nonce. */
    struct byte_stream s;
    stream_init(&s, 256);
    stream_write_i32_le(&s, header->nVersion);
    stream_write_bytes(&s, header->hashPrevBlock.data, 32);
    stream_write_bytes(&s, header->hashMerkleRoot.data, 32);
    stream_write_bytes(&s, header->hashFinalSaplingRoot.data, 32);
    stream_write_u32_le(&s, header->nTime);
    stream_write_u32_le(&s, header->nBits);

    blake2b_update(&state, s.data, s.size);
    stream_free(&s);

    /* Append nonce (32 bytes, raw) */
    blake2b_update(&state, header->nNonce.data, 32);

    bool valid = equihash_is_valid_solution(&ep, &state,
                                            header->nSolution,
                                            header->nSolutionSize);
    return valid;
}
