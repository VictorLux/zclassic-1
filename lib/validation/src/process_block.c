/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "validation/process_block.h"
#include "validation/check_block.h"
#include "validation/connect_block.h"
#include "validation/validationinterface.h"
#include "chain/checkpoints.h"
#include "chain/pow.h"
#include "consensus/upgrades.h"
#include "coins/undo.h"
#include "core/serialize.h"
#include "storage/disk_block_io.h"
#include "storage/txdb.h"
#include "storage/block_index_db.h"
#include "core/serialize.h"
#include "wallet/wallet.h"
#include "validation/txmempool.h"
#include "core/utiltime.h"
#include "controllers/sync_controller.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "validation/main_constants.h"
static int g_last_block_file = -1;
static unsigned int g_last_block_file_size = 0;

/* Periodic coins cache flushing to prevent UTXO corruption on crash.
 * Mirrors the C++ FlushStateToDisk() logic. */
#define COINS_FLUSH_INTERVAL_SECS 3600   /* flush to LevelDB every hour */
#define COINS_FLUSH_MAX_ENTRIES  500000  /* flush if cache exceeds this many entries */

static int64_t g_last_coins_flush = 0;

static bool flush_coins_if_needed(struct coins_view_cache *coins_tip,
                                  bool force)
{
    int64_t now = GetTime();
    if (g_last_coins_flush == 0)
        g_last_coins_flush = now;

    bool time_flush = (now - g_last_coins_flush) > COINS_FLUSH_INTERVAL_SECS;
    bool size_flush = coins_tip->cache_coins.size > COINS_FLUSH_MAX_ENTRIES;

    if (!force && !time_flush && !size_flush)
        return true;

    bool ok = coins_view_cache_flush(coins_tip);
    if (ok) {
        g_last_coins_flush = now;
        printf("flush_coins: wrote %s (entries flushed, cache cleared)\n",
               force ? "forced" : time_flush ? "periodic" : "cache-full");
    } else {
        printf("flush_coins: FAILED to flush coins cache to disk\n");
    }
    return ok;
}

static bool find_block_pos(struct disk_block_pos *pos, unsigned int block_size,
                            const char *datadir)
{
    if (g_last_block_file < 0) {
        /* Scan existing block files to find the last one */
        g_last_block_file = 0;
        for (int i = 0; i < 99999; i++) {
            char path[512];
            struct disk_block_pos probe = { .nFile = i, .nPos = 0 };
            get_block_pos_filename(path, sizeof(path), datadir, &probe, "blk");
            struct stat st;
            if (stat(path, &st) != 0)
                break;
            g_last_block_file = i;
            g_last_block_file_size = (unsigned int)st.st_size;
        }
    }

    /* Move to next file if current one is too large */
    if (g_last_block_file_size + block_size + 8 > MAX_BLOCKFILE_SIZE) {
        g_last_block_file++;
        g_last_block_file_size = 0;
    }

    pos->nFile = g_last_block_file;
    pos->nPos = g_last_block_file_size;
    return true;
}

static struct block_index *add_to_block_index(struct main_state *ms,
                                               const struct block_header *header)
{
    struct uint256 hash;
    block_header_get_hash(header, &hash);

    struct block_index *pindex = calloc(1, sizeof(struct block_index));
    if (!pindex)
        return NULL;
    block_index_init(pindex);

    pindex->nVersion = header->nVersion;
    pindex->hashMerkleRoot = header->hashMerkleRoot;
    pindex->hashFinalSaplingRoot = header->hashFinalSaplingRoot;
    pindex->nTime = header->nTime;
    pindex->nBits = header->nBits;
    pindex->nNonce = header->nNonce;
    memcpy(pindex->nSolution, header->nSolution, header->nSolutionSize);
    pindex->nSolutionSize = header->nSolutionSize;

    if (!block_map_insert(&ms->map_block_index, &hash, pindex)) {
        free(pindex);
        return block_map_find(&ms->map_block_index, &hash);
    }

    /* phashBlock points into the block_map_entry's hash storage */
    struct block_index *found = block_map_find(&ms->map_block_index, &hash);
    if (found) {
        const struct uint256 *stored = block_map_find_hash(
            &ms->map_block_index, &hash);
        if (stored)
            found->phashBlock = stored;
    }

    /* Link to previous block */
    struct block_index *pprev = block_map_find(&ms->map_block_index,
                                                &header->hashPrevBlock);
    if (pprev) {
        pindex->pprev = pprev;
        pindex->nHeight = pprev->nHeight + 1;
        block_index_build_skip(pindex);

        /* Chain work = prev + work for this block */
        struct arith_uint256 block_proof = GetBlockProof(pindex);
        arith_uint256_add(&pindex->nChainWork, &pprev->nChainWork, &block_proof);
    } else {
        pindex->nHeight = 0;
        pindex->nChainWork = GetBlockProof(pindex);
    }

    return pindex;
}

static bool chain_has_all_data(struct block_index *pindex,
                               struct block_index *tip)
{
    while (pindex && pindex != tip) {
        if (!(pindex->nStatus & BLOCK_HAVE_DATA))
            return false;
        pindex = pindex->pprev;
    }
    return true;
}

static struct block_index *find_most_work_chain(struct main_state *ms)
{
    struct block_index *best = active_chain_tip(&ms->chain_active);

    size_t iter = 0;
    struct block_index *pindex;
    while (block_map_next(&ms->map_block_index, &iter, NULL, &pindex)) {
        if (!pindex)
            continue;

        if (!(pindex->nStatus & BLOCK_HAVE_DATA))
            continue;
        if (pindex->nStatus & BLOCK_FAILED_MASK)
            continue;
        if (!block_index_is_valid(pindex, BLOCK_VALID_TRANSACTIONS))
            continue;
        if (pindex->nChainTx == 0)
            continue;

        if (!best || arith_uint256_compare(&pindex->nChainWork,
                                            &best->nChainWork) > 0) {
            if (chain_has_all_data(pindex, best))
                best = pindex;
        }
    }

    return best;
}

static void update_tip(struct main_state *ms, struct block_index *pindex_new)
{
    active_chain_set_tip(&ms->chain_active, pindex_new);
    ms->pindex_best_header = pindex_new;

    char hex[65];
    if (pindex_new->phashBlock)
        uint256_get_hex(pindex_new->phashBlock, hex);
    else
        snprintf(hex, sizeof(hex), "(null)");
    printf("UpdateTip: new best=%s  height=%d  tx=%u  date=%lld\n",
           hex, pindex_new->nHeight, pindex_new->nTx,
           (long long)pindex_new->nTime);
}

bool accept_block_header(const struct block_header *header,
                         struct validation_state *state,
                         struct main_state *ms,
                         const struct chain_params *params,
                         struct block_index **ppindex)
{
    struct uint256 hash;
    block_header_get_hash(header, &hash);

    struct block_index *pindex = block_map_find(&ms->map_block_index, &hash);
    if (pindex) {
        if (ppindex)
            *ppindex = pindex;
        if (pindex->nStatus & BLOCK_FAILED_MASK)
            return validation_state_invalid(state, false, 0, "duplicate", NULL);
        return true;
    }

    if (!check_block_header(header, state, params, true))
        return false;

    /* Get prev block index */
    struct block_index *pindex_prev = NULL;
    if (uint256_cmp(&hash, &params->consensus.hashGenesisBlock) != 0) {
        pindex_prev = block_map_find(&ms->map_block_index,
                                      &header->hashPrevBlock);
        if (!pindex_prev)
            return validation_state_dos(state, 10, false, 0, "bad-prevblk",
                                        false, NULL);
        if (pindex_prev->nStatus & BLOCK_FAILED_MASK)
            return validation_state_dos(state, 100, false, REJECT_INVALID,
                                        "bad-prevblk", false, NULL);
    }

    if (pindex_prev &&
        !contextual_check_block_header(header, state, params, pindex_prev,
                                        ms->fCheckpointsEnabled))
        return false;

    pindex = add_to_block_index(ms, header);
    if (!pindex)
        return validation_state_error(state, "add-to-block-index-failed");

    if ((pindex->nStatus & BLOCK_VALID_MASK) < BLOCK_VALID_TREE)
        pindex->nStatus = (pindex->nStatus & ~BLOCK_VALID_MASK) |
                           BLOCK_VALID_TREE;

    if (ppindex)
        *ppindex = pindex;

    return true;
}

bool accept_block(struct block *block,
                  struct validation_state *state,
                  struct main_state *ms,
                  const struct chain_params *params,
                  struct block_index **ppindex,
                  bool requested,
                  const char *datadir)
{
    struct block_index *pindex = NULL;
    if (!accept_block_header(&block->header, state, ms, params, &pindex))
        return false;
    if (ppindex)
        *ppindex = pindex;

    bool already_have = (pindex->nStatus & BLOCK_HAVE_DATA) != 0;
    if (already_have)
        return true;

    struct block_index *tip = active_chain_tip(&ms->chain_active);
    bool has_more_work = tip ?
        arith_uint256_compare(&pindex->nChainWork, &tip->nChainWork) >= 0 :
        true;
    bool too_far_ahead = pindex->nHeight >
        active_chain_height(&ms->chain_active) + MIN_BLOCKS_TO_KEEP;

    if (!requested) {
        if (pindex->nTx != 0)
            return true;
        if (!has_more_work)
            return true;
        if (too_far_ahead)
            return true;
    }

    if (!check_block(block, state, params, true, true, true) ||
        !contextual_check_block(block, state, params, pindex->pprev)) {
        if (validation_state_is_invalid(state) && !state->corruption_possible) {
            pindex->nStatus |= BLOCK_FAILED_VALID;
        }
        return false;
    }

    /* Write block to disk */
    struct byte_stream blk_stream;
    stream_init(&blk_stream, 4096);
    if (!block_serialize(block, &blk_stream)) {
        stream_free(&blk_stream);
        return validation_state_error(state, "failed-to-serialize-block");
    }

    struct disk_block_pos block_pos;
    disk_block_pos_init(&block_pos);
    if (!find_block_pos(&block_pos, (unsigned int)blk_stream.size, datadir)) {
        stream_free(&blk_stream);
        return validation_state_error(state, "failed-to-find-block-pos");
    }
    stream_free(&blk_stream);

    if (!write_block_to_disk(block, &block_pos, datadir,
                             params->pchMessageStart))
        return validation_state_error(state, "failed-to-write-block");

    /* Update file size tracker: pos->nPos now points past magic+size header,
     * so total = nPos + block_data_size */
    {
        char path[512];
        get_block_pos_filename(path, sizeof(path), datadir, &block_pos, "blk");
        struct stat st;
        if (stat(path, &st) == 0)
            g_last_block_file_size = (unsigned int)st.st_size;
    }

    /* Mark block as having data and valid transactions */
    pindex->nStatus |= BLOCK_HAVE_DATA;
    pindex->nStatus = (pindex->nStatus & ~BLOCK_VALID_MASK) |
                       BLOCK_VALID_TRANSACTIONS;
    pindex->nFile = block_pos.nFile;
    pindex->nDataPos = block_pos.nPos;
    pindex->nTx = (unsigned int)block->num_vtx;
    pindex->nChainTx = (pindex->pprev ? pindex->pprev->nChainTx : 0) +
                        pindex->nTx;

    return true;
}

bool connect_tip(struct validation_state *state,
                 struct main_state *ms,
                 struct coins_view_cache *coins_tip,
                 struct block_index *pindex_new,
                 struct block *pblock,
                 const struct chain_params *params,
                 const char *datadir)
{
    struct block local_block;
    block_init(&local_block);

    if (!pblock) {
        if (!read_block_from_disk_index(&local_block, pindex_new, datadir)) {
            printf("connect_tip: failed to read block at height %d\n",
                   pindex_new->nHeight);
            block_free(&local_block);
            return validation_state_error(state, "failed-to-read-block");
        }
        pblock = &local_block;
    }

    /* Apply the block to the chain state */
    {
        struct coins_view_cache view;
        struct coins_view backing;
        coins_view_cache_as_view(&backing, coins_tip);
        coins_view_cache_init(&view, &backing);

        bool rv = connect_block(pblock, state, pindex_new, &view, params, false);
        if (!rv) {
            printf("connect_tip: connect_block failed at height %d: %s\n",
                   pindex_new->nHeight,
                   state->reject_reason[0] ? state->reject_reason : "unknown");
            if (validation_state_is_invalid(state)) {
                pindex_new->nStatus |= BLOCK_FAILED_VALID;
            }
            if (pblock == &local_block)
                block_free(&local_block);
            coins_view_cache_free(&view);
            return false;
        }

        coins_view_cache_flush(&view);
        coins_view_cache_free(&view);
    }

    /* Update chain tip */
    update_tip(ms, pindex_new);
    pindex_new->nStatus = (pindex_new->nStatus & ~BLOCK_VALID_MASK) |
                           BLOCK_VALID_SCRIPTS;

    /* Persist block_index entry to LevelDB */
    extern struct block_tree_db *g_active_block_tree;
    if (g_active_block_tree) {
        struct disk_block_index dbi;
        disk_block_index_init(&dbi);
        if (pindex_new->pprev && pindex_new->pprev->phashBlock)
            dbi.hashPrev = *pindex_new->pprev->phashBlock;
        dbi.nHeight = pindex_new->nHeight;
        dbi.nStatus = pindex_new->nStatus;
        dbi.nTx = pindex_new->nTx;
        dbi.nFile = pindex_new->nFile;
        dbi.nDataPos = pindex_new->nDataPos;
        dbi.nUndoPos = pindex_new->nUndoPos;
        dbi.nCachedBranchId = pindex_new->nCachedBranchId;
        dbi.nVersion = pindex_new->nVersion;
        dbi.hashMerkleRoot = pindex_new->hashMerkleRoot;
        dbi.hashFinalSaplingRoot = pindex_new->hashFinalSaplingRoot;
        dbi.nTime = pindex_new->nTime;
        dbi.nBits = pindex_new->nBits;
        dbi.nNonce = pindex_new->nNonce;
        memcpy(dbi.nSolution, pindex_new->nSolution, pindex_new->nSolutionSize);
        dbi.nSolutionSize = pindex_new->nSolutionSize;
        block_tree_db_write_block_index(g_active_block_tree, &dbi);
    }

    /* Write transaction index if enabled */
    if (g_active_block_tree && ms->fTxIndex && pblock->num_vtx > 0) {
        struct uint256 *txids = malloc(pblock->num_vtx * sizeof(struct uint256));
        struct disk_tx_pos *positions = malloc(
            pblock->num_vtx * sizeof(struct disk_tx_pos));
        if (txids && positions) {
            size_t header_size = BLOCK_HEADER_SIZE +
                compact_size_sizeof(pblock->header.nSolutionSize) +
                pblock->header.nSolutionSize;
            unsigned int offset = (unsigned int)(header_size +
                compact_size_sizeof(pblock->num_vtx));

            for (size_t i = 0; i < pblock->num_vtx; i++) {
                txids[i] = pblock->vtx[i].hash;
                positions[i].block_pos.nFile = pindex_new->nFile;
                positions[i].block_pos.nPos = pindex_new->nDataPos;
                positions[i].nTxOffset = offset;

                struct byte_stream ts;
                stream_init(&ts, 1024);
                transaction_serialize(&pblock->vtx[i], &ts);
                offset += (unsigned int)ts.size;
                stream_free(&ts);
            }
            block_tree_db_write_tx_index(g_active_block_tree,
                                          txids, positions, pblock->num_vtx);
        }
        free(txids);
        free(positions);
    }

    /* Notify wallet of transactions in the connected block */
    extern struct wallet *g_active_wallet;
    if (g_active_wallet) {
        for (size_t i = 0; i < pblock->num_vtx; i++) {
            wallet_sync_transaction(g_active_wallet, &pblock->vtx[i],
                                    pindex_new);
            /* Trial-decrypt Sapling shielded outputs for our wallet */
            if (pblock->vtx[i].num_shielded_output > 0 &&
                g_active_wallet->sapling_keys.num_keys > 0) {
                struct transaction *tx =
                    (struct transaction *)&pblock->vtx[i];
                transaction_compute_hash(tx);
                wallet_try_sapling_decrypt(g_active_wallet, tx,
                                           &tx->hash);
            }
            /* Mark spent nullifiers */
            if (pblock->vtx[i].num_shielded_spend > 0)
                wallet_mark_sapling_nullifiers_spent(
                    g_active_wallet,
                    (struct transaction *)&pblock->vtx[i]);
        }
        g_active_wallet->best_block_height = pindex_new->nHeight;
    }

    /* Remove confirmed transactions from mempool */
    {
        extern struct tx_mempool *g_active_mempool;
        if (g_active_mempool)
            tx_mempool_remove_for_block(g_active_mempool,
                pblock->vtx, pblock->num_vtx,
                (unsigned int)pindex_new->nHeight);
    }

    /* Sync block to SQLite database */
    {
        extern struct node_db *g_active_node_db;
        if (g_active_node_db) {
            node_db_sync_connect_block(g_active_node_db, pblock, pindex_new);
            if (g_active_wallet) {
                for (size_t i = 0; i < pblock->num_vtx; i++)
                    node_db_sync_wallet_tx(g_active_node_db,
                        &pblock->vtx[i], g_active_wallet,
                        pindex_new->nHeight);
            }
        }
    }

    /* Periodically flush coins cache to LevelDB to prevent UTXO corruption */
    flush_coins_if_needed(coins_tip, false);

    if (pblock == &local_block)
        block_free(&local_block);
    return true;
}

bool disconnect_tip(struct validation_state *state,
                    struct main_state *ms,
                    struct coins_view_cache *coins_tip,
                    const char *datadir)
{
    struct block_index *pindex_delete = active_chain_tip(&ms->chain_active);
    if (!pindex_delete)
        return false;

    struct block block;
    block_init(&block);
    if (!read_block_from_disk_index(&block, pindex_delete, datadir)) {
        block_free(&block);
        return validation_state_error(state, "failed-to-read-block");
    }

    /* Read undo data */
    struct block_undo blockundo;
    block_undo_init(&blockundo);

    struct disk_block_pos undo_pos;
    undo_pos.nFile = pindex_delete->nFile;
    undo_pos.nPos = pindex_delete->nUndoPos;

    if (undo_pos.nPos > 0) {
        FILE *f = open_undo_file(datadir, &undo_pos, true);
        if (f) {
            /* Read undo data from file */
            uint8_t buf[4 * 1024 * 1024];
            size_t nread = fread(buf, 1, sizeof(buf), f);
            fclose(f);
            if (nread > 0) {
                struct byte_stream s;
                stream_init_from_data(&s, buf, nread);
                block_undo_deserialize(&blockundo, &s);
            }
        }
    }

    /* Apply disconnect */
    {
        struct coins_view_cache view;
        struct coins_view backing;
        coins_view_cache_as_view(&backing, coins_tip);
        coins_view_cache_init(&view, &backing);

        if (!disconnect_block(&block, state, pindex_delete, &view, &blockundo)) {
            block_free(&block);
            block_undo_free(&blockundo);
            coins_view_cache_free(&view);
            return false;
        }

        coins_view_cache_flush(&view);
        coins_view_cache_free(&view);
    }

    /* Sync disconnect to SQLite */
    {
        extern struct node_db *g_active_node_db;
        if (g_active_node_db)
            node_db_sync_disconnect_block(g_active_node_db,
                                          &block, pindex_delete);
    }

    update_tip(ms, pindex_delete->pprev);

    block_free(&block);
    block_undo_free(&blockundo);
    return true;
}

bool activate_best_chain(struct validation_state *state,
                         struct main_state *ms,
                         struct coins_view_cache *coins_tip,
                         const struct chain_params *params,
                         struct block *pblock,
                         const char *datadir)
{
    struct block_index *pindex_most_work;

    do {
        pindex_most_work = find_most_work_chain(ms);

        struct block_index *tip = active_chain_tip(&ms->chain_active);
        if (!pindex_most_work || pindex_most_work == tip)
            return true;
        printf("activate_best_chain: tip=%d most_work=%d\n",
               tip ? tip->nHeight : -1,
               pindex_most_work->nHeight);

        /* Check reorg length */
        if (tip) {
            /* Find fork point */
            struct block_index *fork = tip;
            while (fork && fork->nHeight > pindex_most_work->nHeight)
                fork = fork->pprev;
            struct block_index *walk = pindex_most_work;
            while (walk && walk->nHeight > fork->nHeight)
                walk = walk->pprev;
            while (fork && walk && fork != walk) {
                fork = fork->pprev;
                walk = walk->pprev;
            }

            if (tip->nHeight - (fork ? fork->nHeight : -1) > MAX_REORG_LENGTH)
                return false;

            /* Disconnect blocks from current tip to fork point */
            while (active_chain_tip(&ms->chain_active) != fork) {
                if (!disconnect_tip(state, ms, coins_tip, datadir))
                    return false;
            }
        }

        /* Connect blocks from fork to most-work tip.
         * Count total depth, then allocate dynamically. */
        struct block_index *current_tip = active_chain_tip(&ms->chain_active);
        int total_depth = 0;
        for (struct block_index *w = pindex_most_work;
             w && w != current_tip; w = w->pprev)
            total_depth++;

        struct block_index **connect_path = malloc(
            (size_t)total_depth * sizeof(struct block_index *));
        if (!connect_path)
            return false;

        int path_len = 0;
        for (struct block_index *w = pindex_most_work;
             w && w != current_tip && path_len < total_depth;
             w = w->pprev)
            connect_path[path_len++] = w;

        /* Connect in forward order (reverse of path) */
        for (int i = path_len - 1; i >= 0; i--) {
            struct block *use_block = NULL;
            if (pblock && i == 0) {
                struct uint256 block_hash;
                block_header_get_hash(&pblock->header, &block_hash);
                if (connect_path[0]->phashBlock &&
                    uint256_cmp(&block_hash, connect_path[0]->phashBlock) == 0)
                    use_block = pblock;
            }

            if (!connect_tip(state, ms, coins_tip, connect_path[i],
                            use_block, params, datadir)) {
                if (validation_state_is_invalid(state)) {
                    validation_state_init(state);
                }
                free(connect_path);
                return false;
            }
        }
        free(connect_path);

    } while (pindex_most_work != active_chain_tip(&ms->chain_active));

    return true;
}

bool process_new_block(struct validation_state *state,
                       struct main_state *ms,
                       struct coins_view_cache *coins_tip,
                       const struct chain_params *params,
                       struct block *pblock,
                       bool force_processing,
                       const char *datadir)
{
    bool checked = check_block(pblock, state, params, true, true, true);
    if (!checked) {
        printf("process_new_block: check_block failed: %s\n",
               state->reject_reason[0] ? state->reject_reason : "unknown");
        return false;
    }

    struct block_index *pindex = NULL;
    bool requested = force_processing;

    if (!accept_block(pblock, state, ms, params, &pindex, requested, datadir)) {
        printf("process_new_block: accept_block failed: %s\n",
               state->reject_reason[0] ? state->reject_reason : "unknown");
        return false;
    }

    if (!activate_best_chain(state, ms, coins_tip, params, pblock, datadir)) {
        printf("process_new_block: activate_best_chain failed: %s\n",
               state->reject_reason[0] ? state->reject_reason : "unknown");
        return false;
    }

    return true;
}

bool test_block_validity(struct validation_state *state,
                         const struct chain_params *params,
                         struct coins_view_cache *coins_tip,
                         const struct block *block,
                         struct block_index *pindex_prev)
{
    struct coins_view_cache view;
    struct coins_view backing;
    coins_view_cache_as_view(&backing, coins_tip);
    coins_view_cache_init(&view, &backing);

    struct block_index index_dummy;
    block_index_init(&index_dummy);
    index_dummy.pprev = pindex_prev;
    index_dummy.nHeight = pindex_prev->nHeight + 1;

    if (!contextual_check_block_header(&block->header, state, params,
                                        pindex_prev, true)) {
        coins_view_cache_free(&view);
        return false;
    }
    if (!check_block(block, state, params, true, true, true)) {
        coins_view_cache_free(&view);
        return false;
    }
    if (!contextual_check_block(block, state, params, pindex_prev)) {
        coins_view_cache_free(&view);
        return false;
    }
    if (!connect_block(block, state, &index_dummy, &view, params, true)) {
        coins_view_cache_free(&view);
        return false;
    }

    coins_view_cache_free(&view);
    return true;
}
