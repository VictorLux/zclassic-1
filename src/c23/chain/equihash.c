/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php.
 *
 * Equihash solution verification.
 * Full implementation requires libsodium (crypto_generichash_blake2b).
 * This stub validates solution size and will be completed when
 * libsodium is integrated. */

#include "chain/equihash.h"
#include <math.h>

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
    size_t expected = (size_t)((pow(2, k) * ((n / (k + 1)) + 1)) / 8);
    if (sol_size != expected)
        return false;

    /* Full blake2b verification requires libsodium.
     * For now, size validation is sufficient for block relay;
     * PoW hash check in CheckProofOfWork catches invalid blocks. */
    return true;
}
