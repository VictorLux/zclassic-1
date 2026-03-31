/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php.
 *
 * update_coins — applies a transaction's effects to the UTXO set.
 * Spends inputs (removes UTXOs), creates outputs (adds UTXOs).
 * Maintains undo data for chain reorgs.
 *
 * PEDANTIC: every error path logs loudly and returns false.
 * Silent failures here cause UTXO set corruption. */

#include "validation/update_coins.h"
#include "coins/utxo_commitment.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool update_coins_with_undo(const struct transaction *tx,
                            struct coins_view_cache *inputs,
                            struct tx_undo *txundo,
                            int nHeight)
{
    if (!transaction_is_coinbase(tx)) {
        if (!tx_undo_alloc(txundo, tx->num_vin)) {
            fprintf(stderr, "update_coins: tx_undo_alloc FAILED "
                    "num_vin=%zu h=%d\n", tx->num_vin, nHeight);
            return false;
        }
        for (size_t i = 0; i < tx->num_vin; i++) {
            struct coins_cache_entry *entry =
                coins_view_cache_modify(inputs, &tx->vin[i].prevout.hash);
            if (!entry) {
                char hex[65];
                uint256_get_hex(&tx->vin[i].prevout.hash, hex);
                fprintf(stderr, "update_coins: coins_modify FAILED "
                        "input[%zu]=%s h=%d\n", i, hex, nHeight);
                return false;
            }
            unsigned int nPos = tx->vin[i].prevout.n;

            /* Grow vout array if needed */
            if (nPos >= entry->coins.num_vout) {
                size_t new_size = nPos + 1;
                struct tx_out *nv = realloc(entry->coins.vout,
                    new_size * sizeof(struct tx_out));
                if (!nv) {
                    fprintf(stderr, "update_coins: realloc FAILED "
                            "new_size=%zu h=%d\n", new_size, nHeight);
                    return false;
                }
                for (size_t k = entry->coins.num_vout; k < new_size; k++)
                    tx_out_set_null(&nv[k]);
                entry->coins.vout = nv;
                entry->coins.num_vout = new_size;
            }
            if (tx_out_is_null(&entry->coins.vout[nPos])) {
                char hex[65];
                uint256_get_hex(&tx->vin[i].prevout.hash, hex);
                fprintf(stderr, "update_coins: spending NULL output "
                        "%s:%u at h=%d (double-spend or missing UTXO)\n",
                        hex, nPos, nHeight);
                return false;
            }

            /* Validate output value before spending */
            if (entry->coins.vout[nPos].value < 0 ||
                entry->coins.vout[nPos].value > 2100000000000000LL) {
                fprintf(stderr, "update_coins: CORRUPT output value %lld "
                        "at h=%d\n",
                        (long long)entry->coins.vout[nPos].value, nHeight);
                return false;
            }

            /* Remove spent UTXO from commitment */
            utxo_commitment_remove(&inputs->commitment,
                                    tx->vin[i].prevout.hash.data, nPos,
                                    entry->coins.vout[nPos].value,
                                    entry->coins.height);

            txundo->vprevout[i].txout = entry->coins.vout[nPos];
            coins_spend(&entry->coins, nPos);

            if (coins_is_pruned(&entry->coins)) {
                txundo->vprevout[i].height = (unsigned int)entry->coins.height;
                txundo->vprevout[i].coinbase = entry->coins.is_coinbase;
                txundo->vprevout[i].version = entry->coins.version;
            }
        }
    }

    /* Create new outputs */
    struct coins_cache_entry *new_entry =
        coins_view_cache_modify_new(inputs, &tx->hash);
    if (!new_entry) {
        fprintf(stderr, "update_coins: modify_new FAILED at h=%d\n", nHeight);
        return false;
    }
    coins_from_transaction(&new_entry->coins, tx, nHeight);

    /* Validate new output values before adding to commitment */
    for (size_t vi = 0; vi < new_entry->coins.num_vout; vi++) {
        if (!tx_out_is_null(&new_entry->coins.vout[vi])) {
            if (new_entry->coins.vout[vi].value < 0 ||
                new_entry->coins.vout[vi].value > 2100000000000000LL) {
                fprintf(stderr, "update_coins: new output[%zu] value %lld "
                        "out of range at h=%d\n", vi,
                        (long long)new_entry->coins.vout[vi].value, nHeight);
                return false;
            }
            utxo_commitment_add(&inputs->commitment,
                                 tx->hash.data, (uint32_t)vi,
                                 new_entry->coins.vout[vi].value,
                                 nHeight);
        }
    }

    return true;
}

void update_coins(const struct transaction *tx,
                  struct coins_view_cache *inputs,
                  int nHeight)
{
    struct tx_undo txundo;
    tx_undo_init(&txundo);
    update_coins_with_undo(tx, inputs, &txundo, nHeight);
    tx_undo_free(&txundo);
}
