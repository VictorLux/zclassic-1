/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php.
 *
 * Chain-selection and active-tip child discovery helpers that survived
 * deletion of the legacy process_new_block / connect_tip /
 * activate_best_chain engine. */

#include "platform/time_compat.h"
#include <assert.h>
#include <limits.h>
#include <signal.h>
#include <sqlite3.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "util/ar_step_readonly.h"
#include "validation/process_block.h"
#include "validation/main_logic.h"
#include "validation/check_block.h"
#include "validation/connect_block.h"
#include "validation/mirror_consensus.h"
#include "coins/utxo_commitment.h"
#include "net/download.h"
#include "validation/validationinterface.h"
#include "chain/checkpoints.h"
#include "chain/pow.h"
#include "consensus/upgrades.h"
#include "coins/undo.h"
#include "core/core_io.h"
#include "core/serialize.h"
#include "rpc/legacy_rpc_client.h"
#include "storage/disk_block_io.h"
#include "storage/txdb.h"
#include "storage/block_index_db.h"
#include "wallet/wallet.h"
#include "validation/txmempool.h"
#include "core/utiltime.h"
#include "event/event.h"
#include "config/runtime.h"
#include "util/log_macros.h"
#include "validation/checkpoint.h"
#include "chain/mmr.h"
#include "chain/mmb.h"
#include "util/trace.h"
#include "validation/main_constants.h"
#include "validation/process_block_internals.h"
#include "storage/coins_view_sqlite.h"
#include "util/result.h"
#include "util/safe_alloc.h"

#include "process_block_internal.h"

/* ── Disk-verify helpers ─────────────────────────────────────── */
bool process_block_verify_active_tip_child_on_disk(
    const struct block_index *candidate,
    const struct block_index *tip,
    const char *datadir)
{
    if (!candidate || !tip || !tip->phashBlock || !candidate->phashBlock ||
        !datadir || !datadir[0])
        return false;
    if (!(candidate->nStatus & BLOCK_HAVE_DATA) ||
        candidate->nFile < 0 || candidate->nDataPos == 0)
        return false;

    struct block blk;
    block_init(&blk);
    if (!read_block_from_disk_index(&blk, candidate, datadir)) {
        block_free(&blk);
        return false;
    }

    struct uint256 disk_hash;
    block_header_get_hash(&blk.header, &disk_hash);
    bool ok = uint256_eq(&disk_hash, candidate->phashBlock) &&
              uint256_eq(&blk.header.hashPrevBlock, tip->phashBlock);
    block_free(&blk);
    return ok;
}

/* returns true iff the pprev chain from pindex_prev can be
 * walked back through the retarget window and the median-time context
 * used at the far edge of that window. Each step must satisfy
 *   cursor->pprev != NULL && cursor->nHeight == cursor->pprev->nHeight + 1
 * Used to detect the post-FlyClient-snapshot tail where block_index
 * entries for the 193-block region between chain-restore backfill end
 * and fast-sync tip exist on disk but have no valid pprev linkage. */
static bool process_block_pow_window_complete(
    const struct block_index *pindex_prev,
    int pow_window)
{
    const struct block_index *cursor = pindex_prev;
    if (!cursor || pow_window <= 0)
        return true;
    for (int i = 0; i < pow_window; i++) {
        if (!cursor->pprev)
            return false;
        if (cursor->nTime == 0)
            return false;
        if (cursor->nHeight != cursor->pprev->nHeight + 1)
            return false;
        cursor = cursor->pprev;
    }

    /* GetNextWorkRequired() walks `pow_window` entries, then calls
     * block_index_get_median_time_past() on the cursor left just before
     * that window. A metadata-only import anchor often has pprev=NULL
     * and nTime=0; letting the retarget code use that sparse anchor makes
     * honest headers fail with bad-diffbits. */
    for (int i = 0; i < MEDIAN_TIME_SPAN; i++) {
        if (!cursor || cursor->nTime == 0)
            return false;
        if (i + 1 < MEDIAN_TIME_SPAN) {
            if (!cursor->pprev)
                return false;
            if (cursor->nHeight != cursor->pprev->nHeight + 1)
                return false;
        }
        cursor = cursor->pprev;
    }
    return true;
}

bool process_block_should_skip_contextual_header(
    const struct main_state *ms,
    const struct block_index *pindex_prev,
    const struct consensus_params *consensus)
{
    if (!pindex_prev)
        return false;

    int tip_h = active_chain_height(&ms->chain_active);

    /* Case (a): pre-existing old-IBD / scrambled-height slack. */
    if (tip_h > 100000 && pindex_prev->nHeight < tip_h - 1000)
        return true;

    /* Case (b) — post-FlyClient-snapshot tail. If the PoW
     * averaging window cannot be walked contiguously, GetNextWorkRequired
     * would return nProofOfWorkLimit (weakest-allowed) and every honest
     * peer's real nBits would mismatch. Skip contextual check in that
     * case; full validation runs later in connect_block(). */
    int pow_window = consensus ? (int)consensus->nPowAveragingWindow : 17;
    if (pow_window > 0 &&
        !process_block_pow_window_complete(pindex_prev, pow_window))
        return true;

    return false;
}

/* add_to_block_index() relocated to lib/validation/src/accept_block_header.c
 * (its sole caller) in the single-engine swap so the runtime in-memory
 * block_index producer survives the eventual process_block_core.c deletion.
 * Behavior-preserving code motion; the declaration remains in
 * process_block_internal.h. */

struct block_index *find_most_work_chain(struct main_state *ms)
{
    struct block_index *best = active_chain_tip(&ms->chain_active);
    int skipped_no_chaintx = 0;
    int skipped_failed = 0;
    int skipped_invalid = 0;


    size_t iter = 0;
    struct block_index *pindex;
    while (block_map_next(&ms->map_block_index, &iter, NULL, &pindex)) {
        if (!pindex)
            continue;

        /* Skip failed blocks and their children (typed: PERMANENT,
         * DEPENDENCY, and TRANSIENT all gate selection here; the
         * eventual retry-budget for TRANSIENT lands in a separate
         * path that re-validates without involving this scan). */
        if (block_has_any_failure(pindex)) {
            skipped_failed++;
            continue;
        }

        /* Must have at least header validation */
        if (!block_index_is_valid(pindex, BLOCK_VALID_TREE)) {
            skipped_invalid++;
            continue;
        }

        /* Only consider chains where the candidate block has data available.
         * Prefer nChainTx > 0 (cumulative tx count — means block AND all
         * ancestors have data), but also accept BLOCK_HAVE_DATA (block
         * data exists on disk from zclassicd import / scan, even if
         * nChainTx wasn't propagated yet). connect_block will fully
         * validate before committing. Without the HAVE_DATA fallback,
         * imported blocks with data but nChainTx==0 are invisible to
         * chain selection, causing sync stalls. */
        if (pindex->nChainTx == 0 &&
            !(pindex->nStatus & BLOCK_HAVE_DATA)) {
            skipped_no_chaintx++;
            continue;
        }

        if (!best || arith_uint256_compare(&pindex->nChainWork,
                                            &best->nChainWork) > 0) {
            /* Check ancestry for failed blocks */
            bool chain_ok = true;
            struct block_index *check = pindex;
            int tip_h = best ? best->nHeight : -1;
            while (check && check->nHeight > tip_h) {
                if (block_has_any_failure(check)) {
                    chain_ok = false;
                    break;
                }
                if (!check->pprev && check->nHeight > 0) {
                    /* pprev not linked — stop walking ancestry.
                     * This is normal after LDB import where block_index
                     * entries have nChainTx set (from the import) but
                     * pprev pointers aren't fully resolved yet. The
                     * nChainTx > 0 check above already ensures data
                     * availability — don't reject the chain just
                     * because we can't walk pprev to genesis. */
                    break;
                }
                check = check->pprev;
            }
            if (chain_ok)
                best = pindex;
        }
    }

    if (skipped_no_chaintx > 0 && !best) {
        printf("find_most_work_chain: WARNING: %d blocks skipped "
               "(no data, nChainTx==0)\n", skipped_no_chaintx);
    }

    /* refuse to return a candidate BELOW the current tip.
     * The tip is canonical. A "fork tip" at a lower height with higher
     * nChainWork can appear from old import data with incorrect work
     * accounting, but reorging backwards 17 k blocks because of it is
     * never the right answer — activate_best_chain would hit the
     * finality guard anyway, log "below_finality_depth" every second,
     * and the chain would never advance. Treat below-tip best as "no work
     * pending" and let gap-fill close the headers-vs-bodies window. */
    {
        struct block_index *tip = active_chain_tip(&ms->chain_active);
        if (tip && best && best != tip && best->nHeight < tip->nHeight) {
            static time_t g_last_stale_log = 0;
            time_t now_log = platform_time_wall_time_t();
            if (now_log - g_last_stale_log >= 60) {
                g_last_stale_log = now_log;
                printf("find_most_work_chain: ignoring stale fork tip "
                       "h=%d (tip h=%d, depth=%d) — returning tip\n",
                       best->nHeight, tip->nHeight,
                       tip->nHeight - best->nHeight);
            }
            best = tip;
        }
    }

    /* Diagnostic: when activate_best_chain will silent-return
     * (because we picked the current tip as best), emit a
     * rate-limited log line naming the filter counters. This is how
     * the canary identifies which shortlisted cause is keeping
     * production stuck without another investigative round-trip.
     *
     * also kick the gap-fill service so it requests the
     * missing bodies for headers above the tip. Without this kick,
     * gap_fill only wakes every GAPFILL_TICK_SECS=5s and the headers
     * gap closes slowly; an explicit kick from chain selection
     * accelerates convergence whenever activation runs. */
    {
        struct block_index *tip = active_chain_tip(&ms->chain_active);
        if (tip && best == tip) {
            int header_h = ms->pindex_best_header
                         ? ms->pindex_best_header->nHeight : 0;
            if (header_h > tip->nHeight + 1) {
                process_block_kick_gap_fill();
            }
            if (header_h > tip->nHeight + 100) {
                static time_t g_last_stuck_log = 0;
                time_t now_log = platform_time_wall_time_t();
                if (now_log - g_last_stuck_log >= 60) {
                    g_last_stuck_log = now_log;
                    printf("find_most_work_chain: STUCK at tip h=%d "
                           "(best_header h=%d, gap=%d) "
                           "skipped[failed=%d invalid=%d no_data=%d]\n",
                           tip->nHeight, header_h,
                           header_h - tip->nHeight,
                           skipped_failed, skipped_invalid,
                           skipped_no_chaintx);
                }
            }
        }
    }

    return best;
}

struct block_index *find_best_active_tip_child(struct main_state *ms,
                                               struct block_index *tip,
                                               const char *datadir)
{
    struct block_index *best = NULL;
    bool best_has_continuation = false;
    size_t iter = 0;
    struct block_index *candidate;

    if (!ms || !tip || !tip->phashBlock)
        return NULL;

    while (block_map_next(&ms->map_block_index, &iter, NULL, &candidate)) {
        if (!candidate || candidate == tip)
            continue;
        if (!candidate->pprev || !candidate->pprev->phashBlock)
            continue;
        if (!uint256_eq(candidate->pprev->phashBlock, tip->phashBlock))
            continue;
        if (block_has_any_failure(candidate))
            continue;
        if (!block_index_is_valid(candidate, BLOCK_VALID_TREE))
            continue;
        if (!(candidate->nStatus & BLOCK_HAVE_DATA))
            continue;
        if (candidate->nFile < 0 || candidate->nDataPos == 0)
            continue;
        if (!process_block_verify_active_tip_child_on_disk(
                candidate, tip, datadir)) {
            fprintf(stderr, // obs-ok:pre-existing-diagnostic
                    "activate_best_chain: skipping stale active-tip child "
                    "h=%d file=%d pos=%u; local block bytes do not verify "
                    "against index hash and current tip\n",
                    candidate->nHeight, candidate->nFile,
                    candidate->nDataPos);
            candidate->nStatus &= ~(unsigned)BLOCK_HAVE_DATA;
            candidate->nFile = -1;
            candidate->nDataPos = 0;
            continue;
        }

        if (candidate->nHeight != tip->nHeight + 1) {
            candidate->nHeight = tip->nHeight + 1;
            block_index_build_skip(candidate);
            struct arith_uint256 proof = GetBlockProof(candidate);
            arith_uint256_add(&candidate->nChainWork,
                              &tip->nChainWork, &proof);
        }

        /* Continuation scoring is a small-index tie breaker. On imported
         * mainnet indexes it is O(candidates * block_map_size) before RPC
         * starts, so keep large boots bounded and let the verified unlinked
         * fallback repair any stale pprev edge after direct-child selection. */
        bool has_continuation = false;
        if (datadir && datadir[0] && candidate->phashBlock &&
            ms->map_block_index.size < 500000) {
            size_t ci = 0;
            struct block_index *child;
            while (block_map_next(&ms->map_block_index, &ci, NULL, &child)) {
                if (!child || child == candidate)
                    continue;
                if (child->nHeight != candidate->nHeight + 1)
                    continue;
                if (!(child->nStatus & BLOCK_HAVE_DATA))
                    continue;
                if (child->nFile < 0 || child->nDataPos == 0)
                    continue;

                struct disk_block_pos pos;
                disk_block_pos_init(&pos);
                pos.nFile = child->nFile;
                pos.nPos = child->nDataPos;

                struct block blk;
                block_init(&blk);
                if (!read_block_from_disk_pread(&blk, &pos, datadir)) {
                    block_free(&blk);
                    continue;
                }
                has_continuation =
                    uint256_eq(&blk.header.hashPrevBlock,
                               candidate->phashBlock);
                block_free(&blk);
                if (has_continuation)
                    break;
            }
        }

        if (!best ||
            (has_continuation && !best_has_continuation) ||
            (has_continuation == best_has_continuation &&
             arith_uint256_compare(&candidate->nChainWork,
                                   &best->nChainWork) > 0)) {
            best = candidate;
            best_has_continuation = has_continuation;
        }
    }

    return best;
}

struct block_index *find_verified_unlinked_active_tip_child(
    struct main_state *ms,
    struct block_index *tip,
    const char *datadir)
{
    struct block_index *best = NULL;
    size_t iter = 0;
    struct block_index *candidate;

    if (!ms || !tip || !tip->phashBlock || !datadir)
        return NULL;

    while (block_map_next(&ms->map_block_index, &iter, NULL, &candidate)) {
        if (!candidate || candidate == tip)
            continue;
        if (!candidate->phashBlock)
            continue;
        if (candidate->pprev == tip)
            continue;
        if (candidate->nHeight != tip->nHeight + 1)
            continue;
        if (block_has_any_failure(candidate))
            continue;
        if (!(candidate->nStatus & BLOCK_HAVE_DATA))
            continue;
        if (candidate->nFile < 0 || candidate->nDataPos == 0)
            continue;

        struct block blk;
        block_init(&blk);
        if (!read_block_from_disk_index(&blk, candidate, datadir)) {
            block_free(&blk);
            continue;
        }

        struct uint256 disk_hash;
        block_header_get_hash(&blk.header, &disk_hash);
        bool matches_index =
            uint256_eq(&disk_hash, candidate->phashBlock);
        bool extends_tip =
            uint256_eq(&blk.header.hashPrevBlock, tip->phashBlock);
        block_free(&blk);

        if (!matches_index || !extends_tip)
            continue;

        candidate->pprev = tip;
        block_index_build_skip(candidate);
        struct arith_uint256 proof = GetBlockProof(candidate);
        arith_uint256_add(&candidate->nChainWork,
                          &tip->nChainWork, &proof);
        if (candidate->nChainTx == 0 && candidate->nTx > 0)
            candidate->nChainTx = tip->nChainTx + candidate->nTx;

        if (!best ||
            arith_uint256_compare(&candidate->nChainWork,
                                  &best->nChainWork) > 0)
            best = candidate;
    }

    if (best) {
        fprintf(stderr, // obs-ok:pre-existing-diagnostic
            "activate_best_chain: repaired unlinked active-tip child "
            "h=%d from disk-verified prev hash\n",
            best->nHeight);
    }
    return best;
}

/* accept_block_header()   moved to lib/validation/src/accept_block_header.c
 * accept_block()          DELETED with the legacy validation engine.
 * connect_tip()           DELETED with the legacy validation engine.
 * disconnect_tip()        DELETED with the legacy validation engine.
 * activate_best_chain()   DELETED with the legacy validation engine.
 * process_new_block()     DELETED with the legacy validation engine.
 *
 * The reducer (reducer_ingest_block / reducer_kick, app/services + app/jobs)
 * is the sole block-connect engine; every ingest call site routes through it.
 * Selection helpers remain here. Tip publication lives in
 * process_block_tip_publish.c, block-index hydration lives in
 * process_block_index.c, and failed-child propagation lives in
 * process_block_failed_child.c. */
