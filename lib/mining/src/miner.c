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
#include "services/chain_activation_controller.h"
#include "validation/sigops.h"
#include "validation/update_coins.h"
#include "validation/main_constants.h"
#include "chain/subsidy.h"
#include "chain/pow.h"
#include "consensus/upgrades.h"
#include "bloom/merkle.h"
#include "core/random.h"
#include "domain/consensus/coinbase.h"
#include "script/script.h"
#include "script/script_flags.h"
#include "util/timedata.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "util/safe_alloc.h"

static void block_compute_merkle_root(struct block *b)
{
    struct uint256 *txids = zcl_malloc(b->num_vtx * sizeof(struct uint256), "merkle_txids");
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
    struct block_template *bt = zcl_malloc(sizeof(struct block_template), "block_template");
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
    bt->block.vtx = zcl_calloc(max_txs, sizeof(struct transaction), "block_transactions");
    if (!bt->block.vtx) {
        block_template_free(bt);
        free(bt);
        return NULL;
    }
    bt->tx_fees = zcl_calloc(max_txs, sizeof(int64_t), "tx_fees");
    bt->tx_sig_ops = zcl_calloc(max_txs, sizeof(unsigned int), "tx_sig_ops");
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

    /* Create coinbase transaction. Allocation is an adapter concern
     * (lives here); shaping (scriptSig, vout value, version/group_id
     * per epoch, hash) is pure and lives in
     * domain/consensus/coinbase.c. */
    struct transaction *coinbase = &bt->block.vtx[0];
    transaction_free(coinbase);
    transaction_init(coinbase);
    if (!transaction_alloc(coinbase, 1, 1)) {
        block_template_free(bt);
        free(bt);
        return NULL;
    }
    struct domain_consensus_coinbase_inputs cb_in = {
        .n_height     = height,
        .subsidy      = get_block_subsidy(height, &params->consensus),
        .total_fees   = total_fees,
        .miner_script = coinbase_script,
        .params       = &params->consensus,
    };
    struct zcl_result cb_r = domain_consensus_coinbase_build(&cb_in, coinbase);
    if (!cb_r.ok) {
        fprintf(stderr, "[miner] coinbase_build failed at height %d: "
                "code=%d %s (%s:%d)\n", height, cb_r.code, cb_r.message,
                cb_r.source_file, cb_r.source_line);
        block_template_free(bt);
        free(bt);
        return NULL;
    }

    bt->tx_fees[0] = -total_fees;

    /* Fill in header */
    if (!pindex_prev->phashBlock) return NULL;
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

    /* scriptSig shaping is pure — delegate to domain layer. The
     * legacy semantics (3-byte BIP34 height push followed by a
     * minimal-length extra-nonce push, zero nonce -> single zero byte)
     * are preserved exactly. */
    struct zcl_result sr = domain_consensus_coinbase_script_sig_with_extra_nonce(
            height, *extra_nonce, &cb->vin[0].script_sig);
    if (!sr.ok) {
        fprintf(stderr, "[miner] increment_extra_nonce script_sig failed "
                "at height %d: code=%d %s (%s:%d)\n", height, sr.code,
                sr.message, sr.source_file, sr.source_line);
        return;
    }

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

    /* mined-block intake: the synchronous reducer_ingest_block drives the
     * eight Wave-S stages and fills the validation_state. force=true mirrors
     * the locally-mined relay-pre-filter-skipping semantics
     * process_block_found already had. The verdict bool flows back to the
     * mining loop. The main_state / coins_tip / params / datadir args are
     * resolved inside the reducer from boot_activation_controller(). */
    (void)ms;
    (void)coins_tip;
    (void)params;
    (void)datadir;
    return reducer_ingest_block(boot_activation_controller(), pblock,
                                REDUCER_SRC_MINED, true, &state);
}
