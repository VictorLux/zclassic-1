/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "validation/update_coins.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

void update_coins_with_undo(const struct transaction *tx,
                            struct coins_view_cache *inputs,
                            struct tx_undo *txundo,
                            int nHeight)
{
    if (!transaction_is_coinbase(tx)) {
        if (!tx_undo_alloc(txundo, tx->num_vin))
            return;
        for (size_t i = 0; i < tx->num_vin; i++) {
            struct coins_cache_entry *entry =
                coins_view_cache_modify(inputs, &tx->vin[i].prevout.hash);
            if (!entry) return;
            unsigned int nPos = tx->vin[i].prevout.n;

            assert(nPos < entry->coins.num_vout &&
                   !tx_out_is_null(&entry->coins.vout[nPos]));

            txundo->vprevout[i].txout = entry->coins.vout[nPos];
            coins_spend(&entry->coins, nPos);

            if (coins_is_pruned(&entry->coins)) {
                txundo->vprevout[i].height = (unsigned int)entry->coins.height;
                txundo->vprevout[i].coinbase = entry->coins.is_coinbase;
                txundo->vprevout[i].version = entry->coins.version;
            }
        }
    }

    struct coins_cache_entry *new_entry =
        coins_view_cache_modify_new(inputs, &tx->hash);
    if (!new_entry) return;
    coins_from_transaction(&new_entry->coins, tx, nHeight);
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
