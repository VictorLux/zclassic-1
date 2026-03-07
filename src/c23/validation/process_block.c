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
#include <string.h>
#include <stdlib.h>

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
        /* The block_map stores the hash — point to it */
        for (size_t i = 0; i < ms->map_block_index.size; i++) {
            if (uint256_cmp(&ms->map_block_index.entries[i].hash, &hash) == 0) {
                found->phashBlock = &ms->map_block_index.entries[i].hash;
                break;
            }
        }
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

static struct block_index *find_most_work_chain(struct main_state *ms)
{
    struct block_index *best = active_chain_tip(&ms->chain_active);

    for (size_t i = 0; i < ms->map_block_index.size; i++) {
        struct block_index *pindex = ms->map_block_index.entries[i].index;
        if (!pindex)
            continue;

        /* Must have data and not be invalid */
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
            best = pindex;
        }
    }

    return best;
}

static void update_tip(struct main_state *ms, struct block_index *pindex_new)
{
    active_chain_set_tip(&ms->chain_active, pindex_new);
    ms->pindex_best_header = pindex_new;
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
    struct disk_block_pos block_pos;
    disk_block_pos_init(&block_pos);
    if (!write_block_to_disk(block, &block_pos, datadir,
                             params->pchMessageStart))
        return validation_state_error(state, "failed-to-write-block");

    /* Mark block as having data */
    pindex->nStatus |= BLOCK_HAVE_DATA;
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
            block_free(&local_block);
            return validation_state_error(state, "failed-to-read-block");
        }
        pblock = &local_block;
    }

    /* Apply the block to the chain state */
    {
        struct coins_view_cache view;
        struct coins_view backing;
        backing.vtable = NULL;
        backing.impl = coins_tip;
        coins_view_cache_init(&view, &backing);

        bool rv = connect_block(pblock, state, pindex_new, &view, params, false);
        if (!rv) {
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
        backing.vtable = NULL;
        backing.impl = coins_tip;
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

        /* Connect blocks from fork to most-work tip */
        /* Build path from most_work back to current tip */
        struct block_index *connect_path[2048];
        int path_len = 0;
        struct block_index *iter = pindex_most_work;
        struct block_index *current_tip = active_chain_tip(&ms->chain_active);
        while (iter && iter != current_tip && path_len < 2048) {
            connect_path[path_len++] = iter;
            iter = iter->pprev;
        }

        /* Connect in forward order (reverse of path) */
        for (int i = path_len - 1; i >= 0; i--) {
            struct block *use_block = NULL;
            if (pblock && i == 0) {
                /* Use provided block for the final (most-work) tip */
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
                return false;
            }
        }

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
    if (!checked)
        return false;

    struct block_index *pindex = NULL;
    bool requested = force_processing;

    if (!accept_block(pblock, state, ms, params, &pindex, requested, datadir))
        return false;

    if (!activate_best_chain(state, ms, coins_tip, params, pblock, datadir))
        return false;

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
    backing.vtable = NULL;
    backing.impl = coins_tip;
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
