/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php.
 *
 * accept_block — full-block acceptance (header + body + disk write).
 *
 * Extracted from process_block_core.c (WS-6 phase 1, file-level split).
 * Pure code motion; function body is byte-identical to its prior site. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "validation/process_block.h"
#include "validation/main_logic.h"
#include "validation/check_block.h"
#include "validation/mirror_consensus.h"
#include "primitives/block.h"
#include "primitives/transaction.h"
#include "chain/pow.h"
#include "core/arith_uint256.h"
#include "core/core_io.h"
#include "core/serialize.h"
#include "core/uint256.h"
#include "storage/disk_block_io.h"
#include "event/event.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"

#include "process_block_internal.h"

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
        LOG_FAIL("validation", "accept_block_header failed in accept_block");

    /* A full block carries the authoritative header for its own hash.
     * Refresh even when the block was not explicitly requested and even
     * when BLOCK_HAVE_DATA is already set. Otherwise a placeholder
     * imported from stale metadata can keep nBits/time/version at zero,
     * accept the body as "already have", and later block connect_tip()
     * forever. The block hash was just matched by accept_block_header(),
     * so these fields are bound by PoW/hash, not by external metadata. */
    block_index_refresh_header(pindex, &block->header);
    if (pindex->pprev) {
        pindex->nHeight = pindex->pprev->nHeight + 1;
        block_index_build_skip(pindex);
        struct arith_uint256 proof = GetBlockProof(pindex);
        arith_uint256_add(&pindex->nChainWork,
                          &pindex->pprev->nChainWork, &proof);
        if ((pindex->nStatus & BLOCK_VALID_MASK) < BLOCK_VALID_TREE)
            pindex->nStatus = (pindex->nStatus & ~BLOCK_VALID_MASK) |
                               BLOCK_VALID_TREE;
    }
    if (ppindex)
        *ppindex = pindex;

    bool already_have = (pindex->nStatus & BLOCK_HAVE_DATA) != 0;
    if (already_have) {
        /* Defensive verify: BLOCK_HAVE_DATA can be set on an index
         * entry whose on-disk position is actually empty — observed
         * 2026-04-22 post-reindex: block_index entries for heights
         * above the reindex tip kept a stale BLOCK_HAVE_DATA flag
         * (loaded from the flat block_index file), so accept_block
         * skipped the write, find_most_work_chain picked them as
         * candidates, connect_tip later failed to read them, chain
         * stalled.  Verify the block can actually be read before
         * accepting the flag.  If it can't, clear HAVE_DATA + nFile/
         * nDataPos and fall through to the normal write path —
         * which will persist the bytes we just received. */
        if (pindex->nFile >= 0 && pindex->phashBlock) {
            struct disk_block_pos verify_pos;
            disk_block_pos_init(&verify_pos);
            verify_pos.nFile = pindex->nFile;
            verify_pos.nPos = pindex->nDataPos;
            struct block verify_blk;
            block_init(&verify_blk);
            bool data_ok = read_block_from_disk(
                &verify_blk, &verify_pos, datadir);
            if (data_ok) {
                /* Also verify the hash matches — if the file was
                 * rewritten in place (blk file compaction, block
                 * replacement), the bytes may be valid but for a
                 * different block. */
                struct uint256 disk_hash;
                block_header_get_hash(&verify_blk.header, &disk_hash);
                if (uint256_cmp(&disk_hash, pindex->phashBlock) != 0)
                    data_ok = false;
            }
            block_free(&verify_blk);
            if (!data_ok) {
                fprintf(stderr, // obs-ok:pre-existing-diagnostic
                    "accept_block: BLOCK_HAVE_DATA set at h=%d but "
                    "disk data missing/mismatched — clearing flag "
                    "and re-persisting from P2P (file=%d pos=%u)\n",
                    pindex->nHeight, pindex->nFile,
                    pindex->nDataPos);
                event_emitf(EV_BLOCK_REJECTED, 0,
                    "HAVE_DATA_STALE h=%d file=%d pos=%u",
                    pindex->nHeight, pindex->nFile,
                    pindex->nDataPos);
                pindex->nStatus &= ~(unsigned)BLOCK_HAVE_DATA;
                pindex->nFile = -1;
                pindex->nDataPos = 0;
                already_have = false;
                /* fall through to the write path below */
            }
        } else {
            /* Flag set but no file/hash — clearly bogus, clear it. */
            fprintf(stderr, // obs-ok:pre-existing-diagnostic
                "accept_block: BLOCK_HAVE_DATA set at h=%d with "
                "nFile=%d / hash=%p — clearing as bogus\n",
                pindex->nHeight, pindex->nFile,
                (void *)pindex->phashBlock);
            pindex->nStatus &= ~(unsigned)BLOCK_HAVE_DATA;
            pindex->nFile = -1;
            pindex->nDataPos = 0;
            already_have = false;
        }
    }
    if (already_have) {
        /* Blocks from LDB import may have BLOCK_HAVE_DATA but nChainTx==0.
         * Without nChainTx, find_most_work_chain skips them and the chain
         * never advances.  Fix: set nChainTx for already-have blocks that
         * are missing it.  Use nTx if set, otherwise default to 1 (the
         * block exists so it has at least one tx — the coinbase). */
        if (pindex->nChainTx == 0) {
            unsigned int ntx = pindex->nTx > 0 ? pindex->nTx : 1;
            pindex->nChainTx = (pindex->pprev ? pindex->pprev->nChainTx : 0) + ntx;
        }
        return true;
    }

    struct block_index *tip = active_chain_tip(&ms->chain_active);

    /* Defensive nChainWork repair: flat-block-index load + partial
     * reindex can leave pindex entries with stale/zero nChainWork
     * even when the height is correct.  Symptom observed
     * 2026-04-22: block at h=3,085,975 arriving with
     * nChainWork=0000... while tip at h=3,084,369 had the real
     * chain work, making has_more_work=false and silently
     * skipping every incoming block at the wrong work.
     *
     * If this pindex is UPSTREAM of tip (nHeight > tip->nHeight)
     * but its nChainWork compares as less-or-equal, recompute
     * from pprev.  This is the same algorithm accept_block_header
     * uses for wrong-height entries — just triggered on a
     * different invariant (work vs height). */
    if (tip && pindex->pprev && pindex->nHeight > tip->nHeight &&
        arith_uint256_compare(&pindex->nChainWork, &tip->nChainWork) < 0) {
        /* Walk up to find first ancestor with sane work */
        struct block_index *stack[4096];
        int depth = 0;
        struct block_index *cur = pindex;
        /* monotonicity guard. */
        while (cur->pprev && depth < 4096 &&
               cur->pprev->nHeight < cur->nHeight &&
               arith_uint256_compare(&cur->nChainWork,
                                     &tip->nChainWork) < 0 &&
               cur->nHeight > tip->nHeight) {
            stack[depth++] = cur;
            cur = cur->pprev;
        }
        /* Propagate down with corrected work */
        for (int i = depth - 1; i >= 0; i--) {
            struct block_index *fix = stack[i];
            if (fix->pprev) {
                struct arith_uint256 proof = GetBlockProof(fix);
                arith_uint256_add(&fix->nChainWork,
                                  &fix->pprev->nChainWork, &proof);
            }
        }
        if (depth > 0) {
            char wh_fixed[65];
            arith_uint256_get_hex(&pindex->nChainWork, wh_fixed);
            event_emitf(EV_BLOCK_CHECK_PASSED, 0,
                "NCHAINWORK_REPAIR h=%d depth=%d new_work=%.16s",
                pindex->nHeight, depth, wh_fixed);
        }
    }

    bool has_more_work = tip ?
        arith_uint256_compare(&pindex->nChainWork, &tip->nChainWork) >= 0 :
        true;
    if (!requested) {
        /* Skip blocks we already have data for (nTx set AND data on disk).
         * During IBD, blocks whose BLOCK_HAVE_DATA was cleared (e.g. from
         * snapshot cleanup) may still have nTx set from the index — we
         * must NOT skip those, they need to be re-written to disk. */
        if (pindex->nTx != 0 && (pindex->nStatus & BLOCK_HAVE_DATA)) {
            /* Make this silent-skip path observable. If blocks arrive
             * over P2P and end up here, the UTXO/chain state can't
             * advance and the stall is invisible without this event. */
            event_emitf(EV_BLOCK_REJECTED, 0,
                "ACCEPT_SKIP_NTX_AND_HAVE_DATA h=%d ntx=%u",
                pindex->nHeight, pindex->nTx);
            return true;
        }
        if (!has_more_work) {
            /* Visibility: block arrived but doesn't advance the chain
             * from this node's perspective.  During healthy IBD this
             * fires only for sidechain / fork blocks; during a stall
             * it tells us the pindex's nChainWork wasn't populated
             * correctly (e.g. block_index flat loader gap). */
            char w_block[65], w_tip[65];
            arith_uint256_get_hex(&pindex->nChainWork, w_block);
            arith_uint256_get_hex(&tip->nChainWork, w_tip);
            event_emitf(EV_BLOCK_REJECTED, 0,
                "ACCEPT_SKIP_NO_MORE_WORK h=%d tip_h=%d "
                "work=%.16s tip_work=%.16s",
                pindex->nHeight, tip ? tip->nHeight : -1,
                w_block, w_tip);
            return true;
        }
    }

    if (!check_block(block, state, params, true, true, true) ||
        !contextual_check_block(block, state, params, pindex->pprev)) {
        if (validation_state_is_invalid(state) &&
                   !state->corruption_possible) {
            pindex->nStatus |= BLOCK_FAILED_VALID;
            mirror_consensus_record_blocker(
                state->reject_reason[0] ? state->reject_reason
                                        : "accept-block-consensus");
            LOG_FAIL("validation",
                     "check_block or contextual_check_block failed at height %d",
                     pindex->nHeight);
        } else {
            mirror_consensus_record_blocker(
                state->reject_reason[0] ? state->reject_reason
                                        : "accept-block-consensus");
            LOG_FAIL("validation",
                     "check_block or contextual_check_block failed at height %d",
                     pindex->nHeight);
        }
    }

    event_emitf(EV_BLOCK_CHECK_PASSED, 0,
                "height=%d ntx=%zu checks=header,merkle,tx,contextual",
                pindex->nHeight, block->num_vtx);

    /* Write block to disk — validate serialized size first */
    struct byte_stream blk_stream;
    stream_init(&blk_stream, 4096);
    if (!block_serialize(block, &blk_stream)) {
        stream_free(&blk_stream);
        return validation_state_error(state, "failed-to-serialize-block");
    }

    /* Reject blocks larger than MAX_BLOCK_SIZE before persisting.
     * This catches oversized blocks that passed earlier checks
     * (e.g. check_block only checks vtx count, not serialized size). */
    if (blk_stream.size > 2000000) {
        fprintf(stderr, // obs-ok:pre-existing-diagnostic
                "accept_block: serialized size %zu exceeds "
                "MAX_BLOCK_SIZE at height %d\n",
                blk_stream.size, pindex->nHeight);
        stream_free(&blk_stream);
        return validation_state_dos(state, 100, false, REJECT_INVALID,
                                    "bad-blk-length", false, NULL);
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

    /* Mark block as having data only after a successful read-back. */
    if (!block_index_set_have_data_verified(pindex, &block_pos, datadir))
        return validation_state_error(state, "failed-to-verify-block-readback");
    pindex->nStatus = (pindex->nStatus & ~BLOCK_VALID_MASK) |
                       BLOCK_VALID_TRANSACTIONS;
    pindex->nTx = (unsigned int)block->num_vtx;
    pindex->nChainTx = (pindex->pprev ? pindex->pprev->nChainTx : 0) +
                        pindex->nTx;

    /* Propagate nChainTx forward to children that arrived out-of-order.
     * During parallel IBD, block N+1 may arrive before block N. When
     * block N is stored, block N+1 already has BLOCK_HAVE_DATA but its
     * nChainTx was wrong (computed with pprev->nChainTx == 0).
     * Walk forward through the block index and fix any chain that
     * leads from this block. Uses a queue for BFS. */
    {
        struct block_index **queue = zcl_malloc(4096 * sizeof(struct block_index *), "chaintx_queue");
        if (!queue) {
            fprintf(stderr, // obs-ok:pre-existing-diagnostic
                    "process_block: nChainTx propagation skipped "
                    "— malloc(4096) failed\n");
        }
        if (queue) {
            size_t qlen = 0, qcap = 4096;
            queue[qlen++] = pindex;

            while (qlen > 0) {
                struct block_index *parent = queue[--qlen];
                /* Scan all block_index entries for children of parent */
                size_t iter2 = 0;
                struct block_index *child;
                while (block_map_next(&ms->map_block_index, &iter2,
                                       NULL, &child)) {
                    if (!child || child->pprev != parent) continue;
                    if (!(child->nStatus & BLOCK_HAVE_DATA)) continue;

                    unsigned int expected = parent->nChainTx + child->nTx;
                    if (child->nChainTx != expected) {
                        child->nChainTx = expected;
                        /* Queue child to propagate further */
                        if (qlen >= qcap && qcap < 65536) {
                            size_t nc = qcap * 2;
                            struct block_index **nq = zcl_realloc(queue,
                                nc * sizeof(struct block_index *), "chaintx_queue_grow");
                            if (nq) { queue = nq; qcap = nc; }
                        }
                        if (qlen < qcap)
                            queue[qlen++] = child;
                    }
                }
            }
            free(queue);
        }
    }

    return true;
}
