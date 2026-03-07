/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "mining/miner.h"
#include "validation/check_block.h"
#include "validation/check_transaction.h"
#include "validation/connect_block.h"
#include "validation/process_block.h"
#include "validation/sigops.h"
#include "validation/update_coins.h"
#include "validation/main_constants.h"
#include "chain/subsidy.h"
#include "chain/pow.h"
#include "consensus/upgrades.h"
#include "bloom/merkle.h"
#include "core/random.h"
#include "script/script.h"
#include "script/script_flags.h"
#include "util/timedata.h"
#include <string.h>
#include <stdlib.h>

static void block_compute_merkle_root(struct block *b)
{
    struct uint256 *txids = malloc(b->num_vtx * sizeof(struct uint256));
    if (!txids)
        return;
    for (size_t i = 0; i < b->num_vtx; i++)
        txids[i] = b->vtx[i].hash;
    b->header.hashMerkleRoot = compute_merkle_root(txids, b->num_vtx);
    free(txids);
}

void block_template_init(struct block_template *bt)
{
    block_init(&bt->block);
    bt->tx_fees = NULL;
    bt->tx_sig_ops = NULL;
    bt->num_entries = 0;
}

void block_template_free(struct block_template *bt)
{
    block_free(&bt->block);
    free(bt->tx_fees);
    free(bt->tx_sig_ops);
    bt->tx_fees = NULL;
    bt->tx_sig_ops = NULL;
    bt->num_entries = 0;
}

struct block_template *create_new_block(const struct script *coinbase_script,
                                         struct main_state *ms,
                                         struct coins_view_cache *coins_tip,
                                         struct tx_mempool *mempool,
                                         const struct chain_params *params)
{
    struct block_template *bt = malloc(sizeof(struct block_template));
    if (!bt)
        return NULL;
    block_template_init(bt);

    struct block_index *pindex_prev = active_chain_tip(&ms->chain_active);
    if (!pindex_prev) {
        block_template_free(bt);
        free(bt);
        return NULL;
    }

    int height = pindex_prev->nHeight + 1;

    /* Collect transactions from mempool */
    size_t max_txs = 1 + mempool->num_entries;
    bt->block.vtx = calloc(max_txs, sizeof(struct transaction));
    if (!bt->block.vtx) {
        block_template_free(bt);
        free(bt);
        return NULL;
    }
    bt->tx_fees = calloc(max_txs, sizeof(int64_t));
    bt->tx_sig_ops = calloc(max_txs, sizeof(unsigned int));
    if (!bt->tx_fees || !bt->tx_sig_ops) {
        block_template_free(bt);
        free(bt);
        return NULL;
    }

    /* Placeholder coinbase at index 0 */
    transaction_init(&bt->block.vtx[0]);
    bt->block.num_vtx = 1;
    bt->tx_fees[0] = 0;
    bt->tx_sig_ops[0] = 0;
    bt->num_entries = 1;

    int64_t total_fees = 0;
    unsigned int blk_sig_ops = 100;
    uint64_t block_size = 1000;

    /* Add mempool transactions (simple greedy by order) */
    for (size_t i = 0; i < mempool->num_entries; i++) {
        const struct transaction *tx = &mempool->entries[i].tx;

        if (transaction_is_coinbase(tx))
            continue;

        uint64_t tx_size = 250;
        if (block_size + tx_size >= MAX_BLOCK_SIZE)
            continue;

        unsigned int tx_sigops = (unsigned int)get_legacy_sig_op_count(
            tx, SCRIPT_VERIFY_P2SH);
        if (blk_sig_ops + tx_sigops >= MAX_BLOCK_SIGOPS)
            continue;

        if (!coins_view_cache_have_inputs(coins_tip, tx))
            continue;

        int64_t value_in = coins_view_cache_get_value_in(coins_tip, tx);
        int64_t value_out = transaction_get_value_out(tx);
        int64_t tx_fee = value_in - value_out;

        size_t idx = bt->block.num_vtx;
        transaction_init(&bt->block.vtx[idx]);
        transaction_copy(&bt->block.vtx[idx], tx);
        bt->tx_fees[idx] = tx_fee;
        bt->tx_sig_ops[idx] = tx_sigops;
        bt->block.num_vtx++;
        bt->num_entries++;

        total_fees += tx_fee;
        blk_sig_ops += tx_sigops;
        block_size += tx_size;
    }

    /* Create coinbase transaction */
    struct transaction *coinbase = &bt->block.vtx[0];
    transaction_free(coinbase);
    transaction_init(coinbase);

    coinbase->num_vin = 1;
    coinbase->vin = calloc(1, sizeof(struct tx_in));
    tx_in_init(&coinbase->vin[0]);
    uint256_set_null(&coinbase->vin[0].prevout.hash);
    coinbase->vin[0].prevout.n = 0xffffffff;

    /* Script: push 3-byte height, then OP_0 */
    coinbase->vin[0].script_sig.data[0] = 3;
    coinbase->vin[0].script_sig.data[1] = (uint8_t)(height & 0xff);
    coinbase->vin[0].script_sig.data[2] = (uint8_t)((height >> 8) & 0xff);
    coinbase->vin[0].script_sig.data[3] = (uint8_t)((height >> 16) & 0xff);
    coinbase->vin[0].script_sig.data[4] = OP_0;
    coinbase->vin[0].script_sig.size = 5;

    coinbase->num_vout = 1;
    coinbase->vout = calloc(1, sizeof(struct tx_out));
    tx_out_set_null(&coinbase->vout[0]);
    coinbase->vout[0].script_pub_key = *coinbase_script;
    coinbase->vout[0].value = get_block_subsidy(height, &params->consensus) +
                               total_fees;

    /* Set version appropriate for current epoch */
    if (consensus_network_upgrade_active(&params->consensus, height, UPGRADE_SAPLING)) {
        coinbase->version = SAPLING_TX_VERSION;
        coinbase->version_group_id = SAPLING_VERSION_GROUP_ID;
    } else if (consensus_network_upgrade_active(&params->consensus, height, UPGRADE_OVERWINTER)) {
        coinbase->version = OVERWINTER_TX_VERSION;
        coinbase->version_group_id = OVERWINTER_VERSION_GROUP_ID;
    }
    coinbase->expiry_height = 0;

    transaction_compute_hash(coinbase);

    bt->tx_fees[0] = -total_fees;

    /* Fill in header */
    bt->block.header.hashPrevBlock = *pindex_prev->phashBlock;
    bt->block.header.nTime = (uint32_t)GetAdjustedTime();
    bt->block.header.nBits = GetNextWorkRequired(pindex_prev,
                                                  &bt->block.header, &params->consensus);

    block_compute_merkle_root(&bt->block);

    return bt;
}

void increment_extra_nonce(struct block *pblock,
                           struct block_index *pindex_prev,
                           unsigned int *extra_nonce)
{
    static struct uint256 hash_prev_block;
    if (uint256_cmp(&hash_prev_block, &pblock->header.hashPrevBlock) != 0) {
        *extra_nonce = 0;
        hash_prev_block = pblock->header.hashPrevBlock;
    }
    (*extra_nonce)++;

    int height = pindex_prev->nHeight + 1;
    struct transaction *cb = &pblock->vtx[0];

    cb->vin[0].script_sig.data[0] = 3;
    cb->vin[0].script_sig.data[1] = (uint8_t)(height & 0xff);
    cb->vin[0].script_sig.data[2] = (uint8_t)((height >> 8) & 0xff);
    cb->vin[0].script_sig.data[3] = (uint8_t)((height >> 16) & 0xff);

    uint8_t en_bytes[4];
    en_bytes[0] = (uint8_t)(*extra_nonce & 0xff);
    en_bytes[1] = (uint8_t)((*extra_nonce >> 8) & 0xff);
    en_bytes[2] = (uint8_t)((*extra_nonce >> 16) & 0xff);
    en_bytes[3] = (uint8_t)((*extra_nonce >> 24) & 0xff);

    int en_len = 4;
    while (en_len > 1 && en_bytes[en_len - 1] == 0)
        en_len--;

    cb->vin[0].script_sig.data[4] = (uint8_t)en_len;
    memcpy(cb->vin[0].script_sig.data + 5, en_bytes, (size_t)en_len);
    cb->vin[0].script_sig.size = (uint16_t)(5 + en_len);

    transaction_compute_hash(cb);
    block_compute_merkle_root(pblock);
}

bool process_block_found(struct block *pblock,
                         struct main_state *ms,
                         struct coins_view_cache *coins_tip,
                         const struct chain_params *params,
                         const char *datadir)
{
    struct validation_state state;
    validation_state_init(&state);

    return process_new_block(&state, ms, coins_tip, params, pblock, true,
                             datadir);
}
