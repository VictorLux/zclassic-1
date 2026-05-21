/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php.
 *
 * Core consensus paths split out of process_block.c: chain selection,
 * accept_block_header / accept_block, connect_tip / disconnect_tip,
 * activate_best_chain, process_new_block, test_block_validity.
 *
 * Pure code motion. Function bodies are byte-identical to the
 * original lib/validation/src/process_block.c. */

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
#include "controllers/blockchain_controller.h"
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
#include "controllers/sync_controller.h"
#include "event/event.h"
#include "models/database.h"
#include "config/runtime.h"
#include "util/log_macros.h"
#include "services/snapshot_sync_service.h"
#include "services/chain_advance_coordinator.h"
#include "services/chain_restore_service.h"
#include "services/chain_activation_controller.h"
#include "services/chain_evidence_controller.h"
#include "services/chain_state_repository.h"
#include "services/gap_fill_service.h"
#include "services/chain_tip.h"
#include "validation/checkpoint.h"
#include "models/tx_index.h"
#include "chain/mmr.h"
#include "chain/mmb.h"
#include "util/trace.h"
#include "validation/main_constants.h"
#include "validation/process_block_internals.h"
#include "storage/coins_view_sqlite.h"
#include "util/safe_alloc.h"

#include "process_block_internal.h"

/* ── File-local state owned by the disk write path ───────────── */
static int g_last_block_file = -1;
static unsigned int g_last_block_file_size = 0;

/* ── Disk-verify helpers ─────────────────────────────────────── */
static bool process_block_verify_active_tip_child_on_disk(
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

/* ── block-index helpers ─────────────────────────────────────── */
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

    /* Safety: cap file number to prevent runaway file creation.
     * 10000 files * 128MB each = 1.28TB which is more than enough. */
    if (g_last_block_file > 9999) {
        fprintf(stderr, "find_block_pos: file number %d exceeds max (9999)\n",
                g_last_block_file);
        return false;
    }

    pos->nFile = g_last_block_file;
    pos->nPos = g_last_block_file_size;
    return true;
}

struct block_index *add_to_block_index(struct main_state *ms,
                                       const struct block_header *header)
{
    struct uint256 hash;
    block_header_get_hash(header, &hash);

    struct block_index *pindex = zcl_calloc(1, sizeof(struct block_index), "process_block_index");
    if (!pindex)
        return NULL;
    block_index_init(pindex);

    pindex->nVersion = header->nVersion;
    pindex->hashMerkleRoot = header->hashMerkleRoot;
    pindex->hashFinalSaplingRoot = header->hashFinalSaplingRoot;
    pindex->nTime = header->nTime;
    pindex->nBits = header->nBits;
    pindex->nNonce = header->nNonce;
    if (header->nSolutionSize > 0) {
        pindex->nSolution = zcl_malloc(header->nSolutionSize, "block_solution");
        if (pindex->nSolution)
            memcpy(pindex->nSolution, header->nSolution, header->nSolutionSize);
        pindex->nSolutionSize = pindex->nSolution ? header->nSolutionSize : 0;
    } else {
        pindex->nSolution = NULL;
        pindex->nSolutionSize = 0;
    }

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

static void block_index_refresh_header(struct block_index *pindex,
                                       const struct block_header *header)
{
    if (!pindex || !header)
        return;

    pindex->nVersion = header->nVersion;
    pindex->hashMerkleRoot = header->hashMerkleRoot;
    pindex->hashFinalSaplingRoot = header->hashFinalSaplingRoot;
    pindex->nTime = header->nTime;
    pindex->nBits = header->nBits;
    pindex->nNonce = header->nNonce;

    if (pindex->nSolution) {
        free(pindex->nSolution);
        pindex->nSolution = NULL;
        pindex->nSolutionSize = 0;
    }
    if (header->nSolutionSize == 0)
        return;

    pindex->nSolution = zcl_malloc(header->nSolutionSize,
                                   "block_solution_refresh");
    if (!pindex->nSolution)
        return;
    memcpy(pindex->nSolution, header->nSolution, header->nSolutionSize);
    pindex->nSolutionSize = header->nSolutionSize;
}

static bool block_index_hydrate_from_disk(struct block_index *pindex,
                                          const char *datadir)
{
    if (!pindex || !datadir || !pindex->phashBlock ||
        !(pindex->nStatus & BLOCK_HAVE_DATA) ||
        pindex->nFile < 0 || pindex->nDataPos == 0)
        return false;

    struct block disk_block;
    block_init(&disk_block);
    if (!read_block_from_disk_index(&disk_block, pindex, datadir)) {
        block_free(&disk_block);
        return false;
    }

    struct uint256 disk_hash;
    block_header_get_hash(&disk_block.header, &disk_hash);
    if (uint256_cmp(&disk_hash, pindex->phashBlock) != 0) {
        block_free(&disk_block);
        return false;
    }

    block_index_refresh_header(pindex, &disk_block.header);
    if (pindex->pprev) {
        pindex->nHeight = pindex->pprev->nHeight + 1;
        block_index_build_skip(pindex);
        struct arith_uint256 proof = GetBlockProof(pindex);
        arith_uint256_add(&pindex->nChainWork,
                          &pindex->pprev->nChainWork, &proof);
    }
    block_free(&disk_block);
    return pindex->nBits != 0;
}

#ifdef ZCL_TESTING
bool process_block_test_hydrate_index_from_disk(struct block_index *pindex,
                                                const char *datadir)
{
    return block_index_hydrate_from_disk(pindex, datadir);
}
#endif

static struct block_index *find_most_work_chain(struct main_state *ms)
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

        /* Skip failed blocks and their children */
        if (pindex->nStatus & BLOCK_FAILED_MASK) {
            skipped_failed++;
            continue;
        }
        if (mirror_consensus_scope_active() &&
            (!pindex->phashBlock ||
             !mirror_consensus_is_authorized(pindex->nHeight,
                                             pindex->phashBlock))) {
            skipped_invalid++;
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
                if (check->nStatus & BLOCK_FAILED_MASK) {
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
            time_t now_log = time(NULL);
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
                gap_fill_kick();
            }
            if (header_h > tip->nHeight + 100) {
                static time_t g_last_stuck_log = 0;
                time_t now_log = time(NULL);
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

static struct block_index *find_best_active_tip_child(struct main_state *ms,
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
        if (candidate->nStatus & BLOCK_FAILED_MASK)
            continue;
        if (mirror_consensus_scope_active() &&
            (!candidate->phashBlock ||
             !mirror_consensus_is_authorized(candidate->nHeight,
                                             candidate->phashBlock)))
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

static struct block_index *find_verified_unlinked_active_tip_child(
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
        if (candidate->nStatus & BLOCK_FAILED_MASK)
            continue;
        if (mirror_consensus_scope_active() &&
            (!candidate->phashBlock ||
             !mirror_consensus_is_authorized(candidate->nHeight,
                                             candidate->phashBlock)))
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

/* ── csr-migration helper ────────────────────────────────────
 * process_block.c mutates the chain tip from five different places
 * (forward extend, disconnect rollback, genesis connect without
 * disk, reorg recovery, no-fork reset). All of them route through
 * this helper so the cross-source validation and observability
 * live in one place. */
static bool process_block_header_ancestry_linked(const struct block_index *tip)
{
    if (!tip || !tip->phashBlock)
        return false;
    if (tip->nHeight == 0)
        return true;
    return tip->pprev && tip->pprev->phashBlock &&
           tip->pprev->nHeight == tip->nHeight - 1;
}

static bool process_block_chainwork_recomputed(const struct block_index *tip)
{
    struct arith_uint256 expected;
    struct arith_uint256 proof;

    if (!tip)
        return false;
    proof = GetBlockProof(tip);
    if (arith_uint256_is_zero(&proof))
        return false;
    expected = proof;
    if (tip->pprev)
        arith_uint256_add(&expected, &tip->pprev->nChainWork, &proof);
    return arith_uint256_compare(&expected, &tip->nChainWork) == 0 &&
           !arith_uint256_is_zero(&tip->nChainWork);
}

static bool process_block_tip_is_best_work(const struct main_state *ms,
                                           const struct block_index *tip)
{
    size_t iter = 0;
    struct block_index *candidate = NULL;

    if (!ms || !tip)
        return false;
    while (block_map_next(&ms->map_block_index, &iter, NULL, &candidate)) {
        if (!candidate || (candidate->nStatus & BLOCK_FAILED_MASK))
            continue;
        if (arith_uint256_compare(&candidate->nChainWork,
                                  &tip->nChainWork) <= 0)
            continue;
        if (block_index_get_ancestor(candidate, tip->nHeight) != tip)
            return false;
    }
    return true;
}

static struct chain_evidence_record process_block_verified_tip_evidence(
    const struct main_state *ms,
    const struct block_index *tip,
    bool block_bytes_hash_checked)
{
    struct chain_evidence_record evidence = {0};
    evidence.header_ancestry_linked =
        process_block_header_ancestry_linked(tip);
    evidence.chainwork_recomputed =
        process_block_chainwork_recomputed(tip);
    evidence.nakamoto_selected_best_work =
        process_block_tip_is_best_work(ms, tip);
    evidence.block_bytes_hash_checked = block_bytes_hash_checked;
    return evidence;
}

static bool process_block_commit_tip(struct main_state *ms,
                                      struct coins_view_cache *coins_tip,
                                      struct block_index *new_tip,
                                      const char *reason,
                                      bool update_header_tip,
                                      bool persist_coins_best,
                                      const struct chain_evidence_record *verified)
{
#ifndef ZCL_TESTING
    (void)coins_tip;
#endif
    struct trace_span *csr_span = trace_start("csr.commit_tip");
    trace_attr_int(csr_span, "height", new_tip ? new_tip->nHeight : -1);
    trace_attr_str(csr_span, "reason", reason ? reason : "");

    if (!new_tip || !new_tip->phashBlock) {
        trace_set_status(csr_span, TRACE_STATUS_ERROR);
        trace_attr_str(csr_span, "error", "null_tip");
        trace_end(csr_span);
        LOG_FAIL("validation", "commit_chain_state_tip called with null tip or null phashBlock");
    }

    if (verified && process_block_node_db_internal() && csr_instance()->initialized) {
        struct chain_evidence_controller authority;
        struct chain_evidence_controller_tip_request req = {
            .new_tip = new_tip,
            .utxo_max_height = new_tip->nHeight,
            .update_header_tip = update_header_tip,
            .reason = reason ? reason : "process_block.commit_tip",
            .verified = *verified,
        };
        chain_evidence_controller_init(&authority, process_block_node_db_internal(),
                                       csr_instance());
        enum chain_evidence_controller_result ar =
            chain_evidence_controller_promote_tip(&authority, &req);
        if (ar == CEC_OK) {
            trace_end(csr_span);
            return true;
        }
        fprintf(stderr, // obs-ok:pre-existing-diagnostic
                "process_block: evidence controller rejected tip "
                "promotion (%s) reason=%s h=%d\n",
                chain_evidence_controller_result_name(ar),
                reason ? reason : "", new_tip->nHeight);
        trace_set_status(csr_span, TRACE_STATUS_ERROR);
        trace_attr_str(csr_span, "error",
                       chain_evidence_controller_result_name(ar));
        trace_end(csr_span);
        return false;
    }

    struct chain_state_rollback_authorization rollback_auth = {
        .source = CSR_ROLLBACK_SOURCE_VALIDATION,
        .decision = POLICY_ALLOW,
        .from_height = ms ? active_chain_height(&ms->chain_active) : -1,
        .to_height = new_tip->nHeight,
        .max_depth = INT64_MAX,
        .evidence_class = "validation_path_vetted",
        .reason = reason ? reason : "process_block.commit_tip",
    };
    struct chain_state_commit commit = {
        .new_tip             = new_tip,
        .new_coins_best      = *new_tip->phashBlock,
        .expected_utxo_count = 0,
        .update_header_tip   = update_header_tip,
        .persist_coins_best  = persist_coins_best,
        .rollback_auth       = &rollback_auth,
        .wallet_scan_height  = -1,
        .reason              = reason,
    };

    enum csr_result rc = csr_commit_tip(csr_instance(), &commit);
    if (rc == CSR_OK) {
        trace_end(csr_span);
        return true;
    }

#ifdef ZCL_TESTING
    if (rc == CSR_REJECTED_NOT_INITIALIZED) {
        /* Test harness path: the singleton was never wired. Use the
         * canonical helper so events still fire. */
        chain_set_active_tip(ms, new_tip, TIP_FROM_CONNECT,
                             reason ? reason : "csr_uninit_fallback");
        if (update_header_tip) ms->pindex_best_header = new_tip;
        if (coins_tip) coins_view_cache_set_best_block(coins_tip,
                                                        new_tip->phashBlock);
        trace_attr_str(csr_span, "fallback", "raw_setters");
        trace_end(csr_span);
        return true;
    }
#endif

    /* Real validation failure. The csr has already emitted
     * EV_CHAIN_TIP_REJECTED; shout too so this shows up in the
     * node log even when events are disabled. */
    fprintf(stderr, // obs-ok:pre-existing-diagnostic
            "process_block: csr rejected tip commit (%s) reason=%s h=%d\n",
            csr_result_name(rc), reason, new_tip->nHeight);
    if (mirror_consensus_scope_active()) {
        if (rc == CSR_REJECTED_DB_BUSY)
            mirror_consensus_record_blocker("db-writer-busy");
        else if (rc == CSR_REJECTED_PERSIST)
            mirror_consensus_record_blocker("csr-persist-failed");
    }
    trace_set_status(csr_span, TRACE_STATUS_ERROR);
    trace_attr_str(csr_span, "error", csr_result_name(rc));
    trace_end(csr_span);
    return false;
}

/* propagate csr rejection to caller. Previously this function
 * was void — if process_block_commit_tip returned false (csr refused
 * the commit for coins_mismatch / tip_not_in_index / stale_index /
 * etc.), the failure was silently discarded. connect_tip kept
 * returning true while the in-memory chain tip stayed at the old
 * height, so every inbound block re-emitted EV_BLOCK_CONNECTED for
 * the same height forever. That is exactly the 2026-04-18 live
 * outage at h=3,081,601 — 43+ `val.block_connected h=3081601`
 * events per second until the download queue buffered the node to
 * 6 GB RSS and SIGABRT. Returning false here lets activate_best_chain
 * surface the failure so the caller stops treating the block as
 * accepted. */
bool update_tip(struct main_state *ms, struct block_index *pindex_new)
{
    if (pindex_new) {
        struct chain_evidence_record evidence =
            process_block_verified_tip_evidence(ms, pindex_new, true);
        /* coins_tip is NULL here on purpose: the pre-migration
         * update_tip never set coins_best_block (connect_block had
         * already done it while building the new tip), and the
         * fallback path should preserve that behaviour. */
        if (!process_block_commit_tip(ms, NULL, pindex_new,
                                      "process_block.update_tip", true,
                                      false,
                                      &evidence))
            return false;
    } else {
        /* Disconnect past genesis — empty the chain. No commit to
         * make, but still route the concrete publication through CSR
         * so active-tip clears use the same promotion boundary. */
        struct chain_state_rollback_authorization rollback_auth = {
            .source = CSR_ROLLBACK_SOURCE_VALIDATION,
            .decision = POLICY_ALLOW,
            .from_height = ms ? active_chain_height(&ms->chain_active) : -1,
            .to_height = -1,
            .max_depth = INT64_MAX,
            .evidence_class = "validation_disconnect_complete",
            .reason = "disconnect_past_genesis",
        };
        struct chain_state_clear_commit clear = {
            .rollback_auth = &rollback_auth,
            .reason = "disconnect_past_genesis",
        };
        enum csr_result rc = csr_clear_active_tip(csr_instance(), &clear);
#ifdef ZCL_TESTING
        if (rc == CSR_REJECTED_NOT_INITIALIZED) {
            chain_set_active_tip(ms, NULL, TIP_FROM_DISCONNECT,
                                 "disconnect_past_genesis");
        } else
#endif
        if (rc != CSR_OK) {
            fprintf(stderr,
                    "validation: csr rejected active-tip clear (%s)\n",
                    csr_result_name(rc));
            return false;
        }
    }

    char hex[65];
    if (pindex_new && pindex_new->phashBlock)
        uint256_get_hex(pindex_new->phashBlock, hex);
    else
        snprintf(hex, sizeof(hex), "(null)");

    event_emitf(EV_TIP_UPDATED, 0, "h=%d %s",
                pindex_new ? pindex_new->nHeight : -1, hex);

    /* Progress log every 10000 blocks with speed metric */
    if (pindex_new && pindex_new->nHeight % 10000 == 0 && pindex_new->nHeight > 0) {
        static int64_t last_log_time = 0;
        static int last_log_height = 0;
        int64_t now_log = GetTime();
        int64_t elapsed = now_log - last_log_time;
        int blocks_done = pindex_new->nHeight - last_log_height;
        double bps = elapsed > 0 ? (double)blocks_done / (double)elapsed : 0;
        printf("Chain: height=%d  %.0f blk/s\n",
               pindex_new->nHeight, bps);
        last_log_time = now_log;
        last_log_height = pindex_new->nHeight;
    }

    return true;
}

/* External wrapper for chain_advance.c. */
bool process_block_commit_tip_ext(struct main_state *ms,
                                  struct coins_view_cache *coins_tip,
                                  struct block_index *new_tip,
                                  const char *reason,
                                  bool update_header_tip)
{
    return process_block_commit_tip(ms, coins_tip, new_tip, reason,
                                    update_header_tip, false, NULL);
}

/* Regression surface: exposes the post-refactor update_tip so a
 * unit test can drive a real csr_instance through a rejecting input
 * and assert the caller observes false. Production callers go through
 * connect_tip / disconnect_tip; this wrapper must not grow any new
 * behaviour. */
bool process_block_test_update_tip(struct main_state *ms,
                                    struct block_index *pindex_new)
{
    return update_tip(ms, pindex_new);
}

/* stale-FAILED-mark clear.
 *
 * Previous logic rate-limited ALL clears of blocks below tip-100 to
 * one per 300s globally. For BLOCK_FAILED_CHILD (propagation-only)
 * marks that was wrong: when the root FAILED_VALID is cleared
 * (e.g. by a retry near tip, or a reorg-recovery sweep), the
 * descendant CHILD marks become stale and need to drain. At 300s
 * per block, draining 2,000 stale CHILD marks took 166 hours — long
 * enough to look like a permanent chain pin.
 *
 * Clearing a CHILD-only mark is cheap: it does not trigger a
 * connect_block retry by itself. connect_block runs only when
 * find_most_work_chain selects the block as a candidate tip, at
 * which point real validation gates the actual re-commit. So the
 * only cost of un-masking is a possible future validation run,
 * bounded by the selection logic.
 *
 * FAILED_VALID rate-limit stays in place — those are real
 * validation failures. Thrashing connect_block on a known-bad
 * block is the scenario we want to avoid. */
bool process_block_try_clear_stale_failed(struct block_index *pindex,
                                           int tip_h,
                                           time_t now,
                                           time_t *last_retry_clear)
{
    if (!pindex || !(pindex->nStatus & BLOCK_FAILED_MASK))
        return false;

    if (pindex->nHeight >= tip_h - 100) {
        pindex->nStatus &= ~BLOCK_FAILED_MASK;
        return true;
    }

    if ((pindex->nStatus & BLOCK_FAILED_VALID) == 0) {
        pindex->nStatus &= ~BLOCK_FAILED_CHILD;
        return true;
    }

    if (last_retry_clear && now - *last_retry_clear >= 300) {
        pindex->nStatus &= ~BLOCK_FAILED_MASK;
        *last_retry_clear = now;
        return true;
    }

    return false;
}

/* BLOCK_FAILED_CHILD propagation with OOM-amplifier guards.
 *
 * History: the original connect_tip inlined a full block_map scan +
 * qsort on every failed connect_block. At a live tip of ~3M entries
 * that is ~24 MB of scratch + O(N log N) work per call. In the
 * 2026-04-19 BIP30 stall, a single stuck block was retried on every
 * FSM flap; the repeated propagation walk is what drove RSS to the
 * cgroup high-water mark in 2h51m (see
 * docs/archive/2026-04/2026-04-19-bip30-stall.md).
 *
 * Extracted verbatim first (RED), then gated by two cheap early
 * returns (GREEN). See the header for the full guard description. */
enum propagate_failed_child_result
process_block_propagate_failed_child(struct block_map *map,
                                      const struct block_index *pindex_root,
                                      time_t now_sec,
                                      time_t *last_propagate_sec,
                                      size_t *propagated_out)
{
    /* Guard A: parent already failed. A prior propagation
     * from the failed ancestor already covered this subtree, so the
     * descendant CHILD marks are already in place; walking the map
     * again is pure allocator + qsort amplification. */
    if (pindex_root && pindex_root->pprev &&
        (pindex_root->pprev->nStatus & BLOCK_FAILED_MASK))
        return PROPAGATE_FAILED_CHILD_SKIP_PARENT_FAILED;

    /* Guard B: per-retry rate limit. When the caller opts in
     * with a persistent timestamp, refuse back-to-back walks inside
     * PROPAGATE_FAILED_CHILD_MIN_INTERVAL_SEC.  The worst a flap can
     * do under this guard is one 24 MB + O(N log N) walk per
     * interval.  Callers that need an unconditional walk (tests,
     * explicit flush paths) pass NULL. */
    if (last_propagate_sec) {
        if (now_sec - *last_propagate_sec <
            PROPAGATE_FAILED_CHILD_MIN_INTERVAL_SEC)
            return PROPAGATE_FAILED_CHILD_SKIP_RATE_LIMITED;
        *last_propagate_sec = now_sec;
    }

    size_t map_sz = block_map_count(map);
    struct block_index **all = zcl_malloc(
        map_sz * sizeof(struct block_index *), "failed_child_all");
    if (!all)
        return PROPAGATE_FAILED_CHILD_MALLOC_FAILED;

    size_t n = 0, iter = 0;
    struct block_index *ch;
    while (block_map_next(map, &iter, NULL, &ch)) {
        if (ch && ch->nHeight > pindex_root->nHeight)
            all[n++] = ch;
    }
    /* Sort by height ascending — parents before children. */
    qsort(all, n, sizeof(struct block_index *),
          block_index_cmp_height);
    /* Single pass: if parent is failed, child is failed. */
    size_t propagated = 0;
    for (size_t i = 0; i < n; i++) {
        if (!all[i]->pprev) continue;
        if (all[i]->nStatus & BLOCK_FAILED_MASK) continue;
        if (all[i]->pprev->nStatus & BLOCK_FAILED_MASK) {
            all[i]->nStatus |= BLOCK_FAILED_CHILD;
            propagated++;
        }
    }
    free(all);
    if (propagated_out) *propagated_out = propagated;
    return PROPAGATE_FAILED_CHILD_OK;
}

/* accept_block_header() moved to lib/validation/src/accept_block_header.c
 * during WS-6 phase 1 file-level split. */

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

    /* Mark block as having data and valid transactions */
    pindex->nStatus |= BLOCK_HAVE_DATA;
    pindex->nStatus = (pindex->nStatus & ~BLOCK_VALID_MASK) |
                       BLOCK_VALID_TRANSACTIONS;
    pindex->nFile = block_pos.nFile;
    pindex->nDataPos = block_pos.nPos;
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

bool connect_tip(struct validation_state *state,
                 struct main_state *ms,
                 struct coins_view_cache *coins_tip,
                 struct block_index *pindex_new,
                 struct block *pblock,
                 const struct chain_params *params,
                 const char *datadir)
{
    /* refuse to connect a placeholder block_index.
     *
     * A block_index entry with nBits==0 is a chain_restore anchor
     * placeholder (created when coins_best_block hash wasn't yet
     * resolvable from disk headers). Connecting it as tip leaves the
     * chain at a header with version=0 time=0 bits=0, and the next
     * difficulty check sees prev_bits=0 and rejects every incoming
     * header with "bad-diffbits". Live evidence: 5 min of "bad-diffbits
     * at height N+1: prev_bits=0x00000000" right before the chain
     * stalled at h=3089926. */
    if (pindex_new && pindex_new->nHeight > 0 && pindex_new->nBits == 0 &&
        block_index_hydrate_from_disk(pindex_new, datadir)) {
        fprintf(stderr, // obs-ok:pre-existing-diagnostic
            "connect_tip: hydrated placeholder h=%d from verified disk "
            "block before connect\n", pindex_new->nHeight);
        event_emitf(EV_BLOCK_CHECK_PASSED, 0,
                    "connect_tip hydrated placeholder h=%d",
                    pindex_new->nHeight);
    }
    if (pindex_new && pindex_new->nHeight > 0 && pindex_new->nBits == 0) {
        fprintf(stderr, // obs-ok:pre-existing-diagnostic
            "connect_tip: REFUSING placeholder h=%d (nBits=0, no header "
            "data); chain remains at h=%d\n",
            pindex_new->nHeight,
            active_chain_height(&ms->chain_active));
        event_emitf(EV_BLOCK_REJECTED, 0,
                    "connect_tip placeholder h=%d nBits=0",
                    pindex_new->nHeight);
        return false;
    }
    struct trace_span *ct_span = trace_start("chain.connect_tip");
    trace_attr_int(ct_span, "height", pindex_new ? pindex_new->nHeight : -1);
    const int live_height = pindex_new ? pindex_new->nHeight : -1;
    const int64_t connect_tip_start_us = GetTimeMicros();
    int64_t stage_start_us = connect_tip_start_us;

    if (process_block_live_height(live_height)) {
        fprintf(stderr, // obs-ok:pre-existing-diagnostic
                "connect_tip: h=%d stage=start status=%u file=%d pos=%u "
                "tx=%zu pblock=%s\n",
                live_height, pindex_new ? pindex_new->nStatus : 0,
                pindex_new ? pindex_new->nFile : -1,
                pindex_new ? pindex_new->nDataPos : 0,
                pblock ? pblock->num_vtx : 0, pblock ? "provided" : "disk");
        fflush(stderr);
    }

    struct block local_block;
    block_init(&local_block);

        if (!pblock) {
        stage_start_us = GetTimeMicros();
        if (!read_block_from_disk_index(&local_block, pindex_new, datadir)) {
            /* Genesis block (height 0) may not be on disk (blk00000.dat
             * empty after legacy import). Genesis has only the unspendable
             * coinbase — safe to connect without block data. */
            if (pindex_new->nHeight == 0) {
                block_free(&local_block);
                pindex_new->nStatus |= BLOCK_HAVE_DATA;
                pindex_new->nStatus = (pindex_new->nStatus & ~BLOCK_VALID_MASK)
                                       | BLOCK_VALID_SCRIPTS;
                pindex_new->nTx = 1;
                pindex_new->nChainTx = 1;
                process_block_commit_tip(ms, coins_tip, pindex_new,
                    "process_block.connect_tip.genesis_no_disk", true,
                    false, NULL);
                printf("Genesis block: connected (no disk data needed)\n");
                trace_end(ct_span);
                return true;
            }
            /* Retry: pindex_new may be a stale copy (from mmap or header
             * processing) without disk position. Look up the canonical
             * block_index by hash which has the correct file/pos. */
            if (pindex_new->phashBlock) {
                struct block_index *canonical = block_map_find(
                    &ms->map_block_index, pindex_new->phashBlock);
                if (canonical && canonical != pindex_new &&
                    (canonical->nStatus & BLOCK_HAVE_DATA) &&
                    read_block_from_disk_index(&local_block, canonical,
                                               datadir)) {
                    pindex_new->nFile = canonical->nFile;
                    pindex_new->nDataPos = canonical->nDataPos;
                    pindex_new->nStatus |= BLOCK_HAVE_DATA;
                    goto block_read_ok;
                }
            }
            fprintf(stderr, // obs-ok:pre-existing-diagnostic
                    "connect_tip: failed to read block at height %d "
                    "file=%d pos=%u status=%u — clearing HAVE_DATA\n",
                    pindex_new->nHeight, pindex_new->nFile,
                    pindex_new->nDataPos, pindex_new->nStatus);
            pindex_new->nStatus &= ~BLOCK_HAVE_DATA;
            block_free(&local_block);
            trace_set_status(ct_span, TRACE_STATUS_ERROR);
            trace_end(ct_span);
            return validation_state_error(state, "failed-to-read-block");
        }
        block_read_ok:
        pblock = &local_block;
        process_block_log_live_stage(live_height, "read_block",
                                     GetTimeMicros() - stage_start_us);

        /* Verify block read from disk matches expected hash */
        struct uint256 disk_hash;
        block_header_get_hash(&pblock->header, &disk_hash);
        if (!pindex_new->phashBlock) {
            fprintf(stderr, // obs-ok:pre-existing-diagnostic
                    "connect_tip: block index at height %d has NULL "
                    "hash pointer — cannot verify disk block integrity\n",
                    pindex_new->nHeight);
            block_free(&local_block);
            trace_set_status(ct_span, TRACE_STATUS_ERROR);
            trace_end(ct_span);
            return validation_state_error(state, "block-index-no-hash");
        }
        if (uint256_cmp(&disk_hash, pindex_new->phashBlock) != 0) {
            char exp[65], got[65];
            uint256_get_hex(pindex_new->phashBlock, exp);
            uint256_get_hex(&disk_hash, got);
            fprintf(stderr, // obs-ok:pre-existing-diagnostic
                    "connect_tip: WRONG BLOCK at height %d!\n"
                    "  expected: %s\n  got:      %s\n"
                    "  file=%d pos=%u — clearing HAVE_DATA for re-download\n",
                   pindex_new->nHeight, exp, got,
                   pindex_new->nFile, pindex_new->nDataPos);
            /* Self-healing: clear BLOCK_HAVE_DATA so the download manager
             * re-requests this block from P2P. The stale disk position
             * was likely caused by a symlinked block file being modified
             * by another node (zclassicd). */
            pindex_new->nStatus &= ~(unsigned)BLOCK_HAVE_DATA;
            pindex_new->nFile = -1;
            pindex_new->nDataPos = 0;
            event_emitf(EV_BLOCK_REJECTED, 0,
                        "wrong-block-on-disk h=%d cleared-have-data",
                        pindex_new->nHeight);
            block_free(&local_block);
            trace_set_status(ct_span, TRACE_STATUS_ERROR);
            trace_end(ct_span);
            return validation_state_error(state, "wrong-block-on-disk");
        }

        /* Redundant: verify transaction count matches header.
         * Catches truncated block reads from disk corruption. */
        if (pblock->num_vtx == 0) {
            fprintf(stderr, // obs-ok:pre-existing-diagnostic
                    "connect_tip: empty block at h=%d "
                    "(deserialization or disk error)\n", pindex_new->nHeight);
            block_free(&local_block);
            trace_set_status(ct_span, TRACE_STATUS_ERROR);
            trace_end(ct_span);
            return validation_state_error(state, "empty-block-from-disk");
        }
    }

    /* Apply the block to the chain state, with self-healing UTXO recovery.
     * If connect_block fails because a UTXO is missing from the coins DB
     * (e.g. from a non-atomic chainstate import), we find the creating
     * transaction via the tx index, read it from disk, inject the UTXO
     * into the cache, and retry. This makes the node self-sufficient —
     * no manual --repair or zclassicd dependency needed. */
    {
        struct coins_view_cache view;
        struct coins_view backing;
        int recovery_attempts = 0;
        bool missing_utxo_unrecovered = false;

	retry_connect:
        coins_view_cache_as_view(&backing, coins_tip);
        coins_view_cache_init(&view, &backing);
        stage_start_us = GetTimeMicros();

        /* Set Sapling tree for connect_block to update + verify root.
         * The tree persists in ms->sapling_tree across blocks. */
        connect_block_set_sapling_tree(&ms->sapling_tree);

        bool rv = connect_block(pblock, state, pindex_new, &view, params, false);
        connect_block_set_sapling_tree(NULL); /* clear after use */
        process_block_log_live_stage(live_height, "connect_block",
                                     GetTimeMicros() - stage_start_us);
        if (!rv) {
            /* ── Self-healing: recover missing UTXO from block data ── */
	            if (state->has_missing_utxo &&
                strcmp(state->reject_reason, "bad-txns-inputs-missingorspent") == 0 &&
                recovery_attempts < 100 &&
	                g_active_block_tree != NULL) {
	                bool recovered = false;
	                char hex[65];
	                uint256_get_hex(&state->missing_txid, hex);
	                process_block_log_live_stage(live_height,
	                                             "self_heal_start",
	                                             GetTimeMicros() -
	                                                 connect_tip_start_us);

                if (!g_active_block_tree) {
                    fprintf(stderr, // obs-ok:pre-existing-diagnostic
                            "[self-heal] tx index not available "
                            "(no block tree DB)\n");
                } else {
                    struct disk_tx_pos txpos;
                    disk_tx_pos_init(&txpos);
                    if (!block_tree_db_read_tx_index(g_active_block_tree,
                                                     &state->missing_txid,
                                                     &txpos)) {
                        if (process_block_recover_missing_utxo_from_sqlite_tx_index(
                                ms, coins_tip, &state->missing_txid,
                                state->missing_vout, datadir,
                                recovery_attempts + 1)) {
                            recovered = true;
                        }
                    }

                    if (!recovered && txpos.block_pos.nFile < 0 &&
                        process_block_recover_missing_utxo_from_legacy_rpc(
                            coins_tip, &state->missing_txid,
                            state->missing_vout, recovery_attempts + 1)) {
                        recovered = true;
                    }

                    if (!recovered && txpos.block_pos.nFile < 0 &&
                        !process_block_self_heal_scan_enabled()) {
                        fprintf(stderr, // obs-ok:pre-existing-diagnostic
                                "[self-heal] tx %s is absent from "
                                "LevelDB and SQLite tx indexes; broad disk "
                                "scan is disabled by default "
                                "(set ZCL_SELF_HEAL_SCAN_ENABLE=1 for "
                                "operator-directed forensics). Requesting "
                                "chainstate repair instead.\n", hex);
                        atomic_fetch_add_explicit(
                            &g_self_heal_scan_exhausted, 1,
                            memory_order_relaxed);
                        event_emitf(EV_SELF_HEAL_SCAN_EXHAUSTED, 0,
                            "tx=%s tip_h=%d depth=0 disabled=true",
                            hex, active_chain_height(&ms->chain_active));
                    } else if (!recovered && txpos.block_pos.nFile < 0) {
                        fprintf(stderr, // obs-ok:pre-existing-diagnostic
                                "[self-heal] tx %s not in LevelDB tx "
                                "index and SQLite index was unavailable or "
                                "unverified — falling back to bounded-depth "
                                "chain scan\n", hex);
                        /* ── Scan fallback ( surgical coordinator
                         *    commit 2026-04-22 05:11, pre-landed ahead of
                         *    Agent-2's RED/factoring row).
                         *
                         * The tx index can be empty for this tx because
                         * LDB fast-sync imports UTXOs but doesn't
                         * populate block_tree_db's tx-offset entries.
                         * Before surrendering the block as
                         * BLOCK_FAILED_VALID, walk the active chain
                         * backward a bounded number of blocks and
                         * search each for the missing txid.  If found,
                         * inject its outputs into the coins cache AND
                         * backfill the tx_index entry so the next
                         * spend of the same tx is O(log N).
                         *
                         * 2026-05-10 stalls: live imports have needed
                         * UTXOs 150k-200k blocks behind tip after
                         * partial chainstate recovery.  Default to a
                         * deep bounded scan and keep
                         * ZCL_SELF_HEAL_SCAN_DEPTH as an operator
                         * override for deeper exceptional repairs.
                         * Lower values are ignored because they make
                         * the live recovery path fail open into a
                         * restart loop. */
                        int tip_h = active_chain_height(
                            &ms->chain_active);
                        int depth_limit =
                            process_block_self_heal_scan_depth_limit();
                        int scan_stop =
                            (tip_h - depth_limit < 0) ? 0
                                                      : tip_h - depth_limit;

                        bool scan_hit = false;
                        int scan_blocks_checked = 0;
                        int scan_hit_height = -1;
                        for (int h = tip_h;
                             h >= scan_stop && !scan_hit; h--) {
                            struct block_index *bi = active_chain_at(
                                &ms->chain_active, h);
                            if (!bi || !(bi->nStatus & BLOCK_HAVE_DATA))
                                continue;
                            scan_blocks_checked++;
                            struct block scan_b;
                            block_init(&scan_b);
                            if (!read_block_from_disk_index(
                                    &scan_b, bi, datadir)) {
                                block_free(&scan_b);
                                continue;
                            }
                            for (size_t ti = 0;
                                 ti < scan_b.num_vtx; ti++) {
                                if (!uint256_eq(&scan_b.vtx[ti].hash,
                                                 &state->missing_txid))
                                    continue;
                                if (process_block_inject_missing_utxo(
                                        coins_tip, &state->missing_txid,
                                        state->missing_vout,
                                        &scan_b.vtx[ti], h,
                                        "verified chain scan",
                                        recovery_attempts + 1)) {
                                    scan_hit = true;
                                    scan_hit_height = h;
                                    /* Backfill tx_index — on the next
                                     * spend of this tx we take the
                                     * fast O(log N) path instead of
                                     * re-scanning.  Not fatal if the
                                     * write fails; the recovery still
                                     * happened. */
                                    struct disk_tx_pos tx_new;
                                    disk_tx_pos_init(&tx_new);
                                    tx_new.block_pos.nFile = bi->nFile;
                                    tx_new.block_pos.nPos =
                                        bi->nDataPos;
                                    (void)block_tree_db_write_tx_index(
                                        g_active_block_tree,
                                        &state->missing_txid,
                                        &tx_new, 1);
                                }
                                break;
                            }
                            block_free(&scan_b);
                        }

                        if (scan_hit) {
                            atomic_fetch_add_explicit(
                                &g_self_heal_scan_hits, 1,
                                memory_order_relaxed);
                            atomic_fetch_add_explicit(
                                &g_self_heal_scan_blocks_checked_total,
                                (uint64_t)scan_blocks_checked,
                                memory_order_relaxed);
                            printf("[self-heal] RECOVERED UTXO %s via "
                                   "chain scan (hit_h=%d, depth=%d, "
                                   "blocks_checked=%d) — retry %d\n",
                                   hex, scan_hit_height,
                                   tip_h - scan_hit_height,
                                   scan_blocks_checked,
                                   recovery_attempts + 1);
                            fflush(stdout);
                            event_emitf(EV_SELF_HEAL_SCAN_HIT, 0,
                                "tx=%s h=%d depth=%d",
                                hex, scan_hit_height,
                                tip_h - scan_hit_height);
                            recovered = true;
                        } else {
                            atomic_fetch_add_explicit(
                                &g_self_heal_scan_exhausted, 1,
                                memory_order_relaxed);
                            atomic_fetch_add_explicit(
                                &g_self_heal_scan_blocks_checked_total,
                                (uint64_t)scan_blocks_checked,
                                memory_order_relaxed);
                            fprintf(stderr, // obs-ok:pre-existing-diagnostic
                                "[self-heal] scan exhausted "
                                "(tx=%s, tip_h=%d, depth_limit=%d, "
                                "blocks_checked=%d) — no match\n",
                                hex, tip_h, depth_limit,
                                scan_blocks_checked);
                            event_emitf(EV_SELF_HEAL_SCAN_EXHAUSTED, 0,
                                "tx=%s tip_h=%d depth=%d",
                                hex, tip_h, depth_limit);
                        }
                    } else if (!recovered && txpos.block_pos.nFile < 0) {
                        fprintf(stderr, // obs-ok:pre-existing-diagnostic
                                "[self-heal] tx %s nFile=%d "
                                "(tx index entry too small or corrupt)\n",
                                hex, txpos.block_pos.nFile);
                    } else if (!recovered) {
                        atomic_fetch_add_explicit(
                            &g_self_heal_tx_index_hits, 1,
                            memory_order_relaxed);
                        struct block src_block;
                        block_init(&src_block);
                        if (!read_block_from_disk(&src_block,
                                                  &txpos.block_pos, datadir)) {
                            fprintf(stderr, // obs-ok:pre-existing-diagnostic
                                    "[self-heal] failed to read block "
                                    "file=%d pos=%u for tx %s\n",
                                    txpos.block_pos.nFile,
                                    txpos.block_pos.nPos, hex);
                            recovered =
                                process_block_recover_missing_utxo_from_legacy_rpc(
                                    coins_tip, &state->missing_txid,
                                    state->missing_vout,
                                    recovery_attempts + 1);
                            block_free(&src_block);
                        } else {
                            for (size_t ti = 0; ti < src_block.num_vtx; ti++) {
                                if (uint256_eq(&src_block.vtx[ti].hash,
                                               &state->missing_txid)) {
                                    struct uint256 src_hash;
                                    block_get_hash(&src_block, &src_hash);
                                    struct block_index *src_idx =
                                        block_map_find(&ms->map_block_index,
                                                       &src_hash);
                                    int src_height = src_idx ?
                                        src_idx->nHeight : 0;

                                    recovered =
                                        process_block_inject_missing_utxo(
                                            coins_tip, &state->missing_txid,
                                            state->missing_vout,
                                            &src_block.vtx[ti], src_height,
                                            "LevelDB tx index",
                                            recovery_attempts + 1);
                                    break;
                                }
                            }
                            if (!recovered) {
                                recovered =
                                    process_block_recover_missing_utxo_from_legacy_rpc(
                                        coins_tip, &state->missing_txid,
                                        state->missing_vout,
                                        recovery_attempts + 1);
                            }
                            block_free(&src_block);
                        }
                    }
                }

	                if (recovered) {
	                    coins_view_cache_free(&view);
	                    memset(&view, 0, sizeof(view));
	                    recovery_attempts++;
	                    validation_state_init(state);
	                    process_block_log_live_stage(live_height,
	                                                 "self_heal_recovered",
	                                                 GetTimeMicros() -
	                                                     connect_tip_start_us);
	                    goto retry_connect;
	                }
                /* Recovery failed. A missing UTXO at live height is local
                 * chainstate/index corruption until proven otherwise, not
                 * consensus evidence that the block is invalid. Do not poison
                 * the block index with BLOCK_FAILED_VALID; leave the block
                 * retriable after a deeper self-heal scan or UTXO repair. */
                missing_utxo_unrecovered = true;
                fprintf(stderr, // obs-ok:pre-existing-diagnostic
                        "[self-heal] FAILED to recover tx %s:%u "
                        "— leaving block %d retriable\n",
                        hex, state->missing_vout, pindex_new->nHeight);
            }

            fprintf(stderr, "connect_tip: connect_block FAILED h=%d: %s\n", // obs-ok:pre-existing-diagnostic
                    pindex_new->nHeight,
                    state->reject_reason[0] ? state->reject_reason : "unknown");
            if (validation_state_is_invalid(state)) {
                if (missing_utxo_unrecovered) {
                    pindex_new->nStatus &= ~BLOCK_FAILED_MASK;
                    fprintf(stderr, // obs-ok:pre-existing-diagnostic
                            "connect_tip: NOT marking h=%d failed after "
                            "unrecovered missing UTXO; local chainstate "
                            "repair can retry this block\n",
                            pindex_new->nHeight);
                } else {
                    pindex_new->nStatus |= BLOCK_FAILED_VALID;
                    mirror_consensus_record_blocker(
                        state->reject_reason[0] ? state->reject_reason
                                                : "connect_block");
                }
                /* Don't propagate BLOCK_FAILED_CHILD for very early
                 * blocks (h<=10) during IBD. BIP30 failures at h=1 are
                 * typically caused by stale UTXO state (e.g. snapshot
                 * UTXOs at genesis), not genuinely invalid blocks.
                 * Propagating to 3M+ descendants is catastrophic and
                 * prevents any further syncing. The outer do-while loop
                 * will retry find_most_work_chain, which skips this one
                 * failed block. */
                if (missing_utxo_unrecovered) {
                    fprintf(stderr, // obs-ok:pre-existing-diagnostic
                            "connect_tip: NOT propagating "
                            "BLOCK_FAILED_CHILD at h=%d "
                            "(missing UTXO unrecovered)\n",
                            pindex_new->nHeight);
                } else if (pindex_new->nHeight <= 10) {
                    fprintf(stderr, // obs-ok:pre-existing-diagnostic
                            "connect_tip: NOT propagating "
                            "BLOCK_FAILED at h=%d (early block, likely "
                            "transient UTXO state issue)\n",
                            pindex_new->nHeight);
                } else {
                    /* delegate to helper with both OOM-amplifier
                     * guards enabled.  Static timestamp gives a single
                     * per-process rate-limit window — the flap amplifier
                     * shape is global, not per-block, so one bucket for
                     * the whole node is correct. */
                    static time_t last_propagate_sec = 0;
                    size_t propagated = 0;
                    enum propagate_failed_child_result rv =
                        process_block_propagate_failed_child(
                            &ms->map_block_index, pindex_new,
                            GetTime(), &last_propagate_sec, &propagated);
                    switch (rv) {
                    case PROPAGATE_FAILED_CHILD_OK:
                        if (propagated > 0)
                            printf("Propagated BLOCK_FAILED_CHILD to %zu "
                                   "descendants\n", propagated);
                        break;
                    case PROPAGATE_FAILED_CHILD_SKIP_PARENT_FAILED:
                        fprintf(stderr, // obs-ok:pre-existing-diagnostic
                                "connect_tip: NOT propagating "
                                "BLOCK_FAILED_CHILD at h=%d (parent h=%d "
                                "already in failed state — propagation "
                                "already done)\n",
                                pindex_new->nHeight,
                                pindex_new->pprev->nHeight);
                        break;
                    case PROPAGATE_FAILED_CHILD_SKIP_RATE_LIMITED:
                        fprintf(stderr, // obs-ok:pre-existing-diagnostic
                                "connect_tip: BLOCK_FAILED_CHILD "
                                "propagation rate-limited at h=%d "
                                "(last walk %lds ago, min %ds)\n",
                                pindex_new->nHeight,
                                (long)(GetTime() - last_propagate_sec),
                                PROPAGATE_FAILED_CHILD_MIN_INTERVAL_SEC);
                        break;
                    case PROPAGATE_FAILED_CHILD_MALLOC_FAILED:
                        fprintf(stderr, // obs-ok:pre-existing-diagnostic
                                "BLOCK_FAILED_CHILD: malloc failed "
                                "— propagation skipped!\n");
                        break;
                    }
                }
            }
            /* Clean up: free view first (may contain entries from update_coins),
             * then block. Zero view to prevent any double-free. */
            coins_view_cache_free(&view);
            memset(&view, 0, sizeof(view));
            if (pblock == &local_block)
                block_free(&local_block);
            trace_set_status(ct_span, TRACE_STATUS_ERROR);
            trace_end(ct_span);
            return false;
        }

	        process_block_check_crash_stage(PBCS_AFTER_CONNECT_BLOCK);

	        stage_start_us = GetTimeMicros();
	        if (!coins_view_cache_flush(&view)) {
	            fprintf(stderr, "connect_tip: FATAL coins flush failed h=%d\n", // obs-ok:pre-existing-diagnostic
	                    pindex_new->nHeight);
            coins_view_cache_free(&view);
            if (pblock == &local_block)
                block_free(&local_block);
            trace_set_status(ct_span, TRACE_STATUS_ERROR);
            trace_end(ct_span);
	            return validation_state_error(state, "coins-flush-failed");
	        }
	        process_block_log_live_stage(live_height, "coins_flush",
	                                     GetTimeMicros() - stage_start_us);
	        coins_view_cache_free(&view);
	        process_block_check_crash_stage(PBCS_AFTER_COINS_VIEW_FLUSH);
	    }

    /* ── Mandatory SHA3 UTXO checkpoint verification ──────────── */
    /* When we reach a hardcoded checkpoint height, flush all coins to
     * SQLite and verify the SHA3 hash matches the compiled-in constant.
     * This is a one-time O(n) check that guarantees UTXO set integrity.
     * If it fails, the node's data is corrupted and MUST NOT continue. */
    {
        const struct sha3_utxo_checkpoint *cp = get_sha3_utxo_checkpoint();
        if (cp && pindex_new->nHeight == cp->height) {
            /* Force full coins flush to SQLite */
            flush_coins_if_needed(coins_tip, true);

            struct node_db *ndb = process_block_node_db_internal();
            if (ndb && ndb->db) {
                uint8_t sha3[32];
                uint64_t count = 0;
                utxo_commitment_sha3_compute(ndb->db, sha3, &count);

                if (memcmp(sha3, cp->sha3_hash, 32) != 0) {
                    char exp[65], got[65];
                    for (int i = 0; i < 32; i++) {
                        snprintf(exp + i*2, 3, "%02x", cp->sha3_hash[i]);
                        snprintf(got + i*2, 3, "%02x", sha3[i]);
                    }
                    fprintf(stderr, // obs-ok:pre-existing-diagnostic
                        "\n*** SHA3 UTXO CHECKPOINT FAILED at height %d ***\n"
                        "Expected: %s\n"
                        "Computed: %s\n"
                        "Expected %lu UTXOs, computed %lu\n"
                        "Your UTXO set is corrupted. The node will shut down.\n"
                        "Fix: delete node.db and resync from scratch.\n\n",
                        cp->height, exp, got,
                        (unsigned long)cp->utxo_count,
                        (unsigned long)count);
                    fflush(stderr);
                    event_emitf(EV_UTXO_CHECKPOINT_FAIL, 0,
                                "height=%d expected=%s got=%s",
                                cp->height, exp, got);
                    trace_set_status(ct_span, TRACE_STATUS_ERROR);
                    trace_end(ct_span);
                    return validation_state_error(state,
                        "sha3-utxo-checkpoint-failed");
                }
                printf("SHA3 checkpoint PASSED at height %d (%lu UTXOs)\n",
                       cp->height, (unsigned long)count);
                fflush(stdout);
                event_emitf(EV_UTXO_CHECKPOINT_PASS, 0,
                            "height=%d count=%lu",
                            cp->height, (unsigned long)count);
            }
        }
    }

    /* Update chain tip. If csr rejects the commit (coins_mismatch,
     * tip_not_in_index, stale_index, ...) we MUST NOT keep going —
     * continuing would leave pindex_new marked BLOCK_VALID_SCRIPTS
     * while active_chain_tip still points at the previous tip,
     * which is the root cause (repeated `val.block_connected`
     * at the same height forever). Surface it as a system error so
     * activate_best_chain bubbles up to the caller.
     *
     * Save coins_tip's pre-update hash so we can roll back on csr
     * rejection. Otherwise coins_view_cache_flush above has already
     * advanced coins_tip's hash_block to pindex_new's hash, but the
     * csr commit failed — leaving coins ahead of active_chain. The
     * next incoming block extending our REAL tip then trips the
     * connect_block view/prev-block invariant
     * (view=rejected-block-hash != incoming-block.prev=our-real-tip-hash)
     * and the chain wedges. Observed live at h=1: active_chain at h=1
     * hash 0004b3... but coins_view.hash_block stuck at 0007e5c9...
     * (a rejected
     * sibling-fork h=2's pre-flush hash). */
        struct uint256 coins_hash_pre_commit;
        if (coins_tip) {
            coins_view_cache_get_best_block(coins_tip, &coins_hash_pre_commit);
        } else {
            memset(&coins_hash_pre_commit, 0, sizeof(coins_hash_pre_commit));
        }

	    stage_start_us = GetTimeMicros();
	    if (!update_tip(ms, pindex_new)) {
	        fprintf(stderr, // obs-ok:pre-existing-diagnostic
                "connect_tip: update_tip rejected h=%d — csr refused "
                "the commit (see `csr: REJECTED` above). Rolling back "
                "coins_tip.hash_block to the pre-commit value to keep "
                "the next view/prev-block check honest. UTXO map "
                "entries from the rejected block remain in the cache "
                "but their hash anchor is correct.\n",
                pindex_new->nHeight);
        if (coins_tip && pindex_new->pprev && pindex_new->pprev->phashBlock) {
            /* Restore coins_tip's anchor to the previous tip's hash —
             * matches active_chain's tip pointer (which did NOT advance). */
            coins_view_cache_set_best_block(coins_tip,
                                             pindex_new->pprev->phashBlock);
        }
        if (pblock == &local_block)
            block_free(&local_block);
        trace_set_status(ct_span, TRACE_STATUS_ERROR);
        trace_end(ct_span);
	        return validation_state_error(state, "csr-tip-commit-rejected");
	    }
        (void)coins_hash_pre_commit;  /* held for future structured rollback */
	    process_block_log_live_stage(live_height, "update_tip",
	                                 GetTimeMicros() - stage_start_us);
	    process_block_check_crash_stage(PBCS_AFTER_UPDATE_TIP);
    pindex_new->nStatus = (pindex_new->nStatus & ~BLOCK_VALID_MASK) |
                           BLOCK_VALID_SCRIPTS;

    /* Ordering invariant for crash-safe tip advance:
     *   coins.db (UTXOs + coins_best_block) COMMITted BEFORE
     *   LevelDB block_index is fsynced.
     *
     * If kill -9 fires between the two writes, the next boot sees
     * coins.db at N+1 but block_index at N. utxo_recovery_service's
     * forward-roll re-derives the block_index entry deterministically
     * from disk_block_io and the block payload — there's no UTXO
     * delta to recover, only the index row.
     *
     * The reverse direction (block_index ahead of coins.db) requires
     * re-running connect_block to recover UTXO state, which is the
     * 250k-block backward self_heal scan today. Eliminating that
     * direction by ordering eliminates the scan's load-bearing role.
     *
     * Skipped during IBD: bulk sync amortizes per-block fsync over
     * the lazy flush_coins_if_needed policy. At-tip ops force a
     * per-block coins flush so kill -9 at the tip never rewinds. */
    if (!is_initial_block_download(ms)) {
        flush_coins_if_needed(coins_tip, true);
    }
    process_block_check_crash_stage(PBCS_AFTER_COINS_DISK_FLUSH);

    /* Persist block_index entry to LevelDB */
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
        if (pindex_new->nSolution && pindex_new->nSolutionSize > 0)
            memcpy(dbi.nSolution, pindex_new->nSolution, pindex_new->nSolutionSize);
        dbi.nSolutionSize = pindex_new->nSolutionSize;
        /* Use the synchronous write so this block_index entry is
         * durable in LevelDB before connect_tip returns. Async writes
         * leave a window where kill -9 rewinds the block_index past
         * the durable coins.db tip (the 1-6 block rewind documented
         * in feedback_kill_restart_recovery_cost.md).
         *
         * Exception: during fast-sync body-pull / direct-import the
         * caller has explicitly opted into batched durability — coins.db
         * still commits per block (preserving the ordering invariant on
         * crash), but block_index goes async. */
        if (atomic_load_explicit(&g_body_pull_active,
                                  memory_order_relaxed)) {
            block_tree_db_write_block_index(g_active_block_tree, &dbi);
        } else {
            block_tree_db_write_block_index_sync(g_active_block_tree, &dbi);
        }
        process_block_check_crash_stage(PBCS_AFTER_BLOCK_INDEX_WRITE);

        /* Free nSolution after persisting to disk — saves 1344B per block
         * (4GB total for 3M entries). Serving code in msg_headers.c and
         * msg_blocks.c falls back to reading from disk when NULL. */
        free(pindex_new->nSolution);
        pindex_new->nSolution = NULL;
        pindex_new->nSolutionSize = 0;
    }

    /* Write transaction index if enabled */
    if (g_active_block_tree && ms->fTxIndex && pblock->num_vtx > 0) {
        struct uint256 *txids = zcl_malloc(pblock->num_vtx * sizeof(struct uint256), "connect_tip_txids");
        struct disk_tx_pos *positions = zcl_malloc(
            pblock->num_vtx * sizeof(struct disk_tx_pos), "connect_tip_txpos");
        if (!txids || !positions) {
            fprintf(stderr, // obs-ok:pre-existing-diagnostic
                    "connect_tip: tx index alloc failed at height %d "
                    "(%zu txs)\n", pindex_new->nHeight, pblock->num_vtx);
        }
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

    /* Notify wallet of transactions in the connected block.
     * Skipped during fast-sync body-pull: evidence-mode caller runs a
     * single wallet_rescan over the imported range at the end. */
    if (!atomic_load_explicit(&g_body_pull_active, memory_order_relaxed))
    {
        struct wallet *wallet = process_block_wallet();
        struct node_db *ndb = process_block_node_db_internal();
        if (wallet) {
            for (size_t i = 0; i < pblock->num_vtx; i++) {
                wallet_sync_transaction(wallet, &pblock->vtx[i],
                                        pindex_new);
                /* Trial-decrypt Sapling shielded outputs for our wallet */
                if (pblock->vtx[i].num_shielded_output > 0 &&
                    wallet->sapling_keys.num_keys > 0) {
                    struct transaction *tx =
                        (struct transaction *)&pblock->vtx[i];
                    transaction_compute_hash(tx);
                    size_t notes_before = wallet->num_sapling_notes;
                    wallet_try_sapling_decrypt(wallet, tx,
                                               &tx->hash);
                    /* Persist newly discovered notes to SQLite */
                    if (ndb && wallet->num_sapling_notes > notes_before) {
                        for (size_t ni = notes_before;
                             ni < wallet->num_sapling_notes; ni++) {
                            struct sapling_received_note *note =
                                &wallet->sapling_notes[ni];
                            node_db_sync_sapling_note(ndb,
                                note->txid.data, note->output_index,
                                (int64_t)note->value, note->rcm,
                                note->memo, 512, note->ivk,
                                note->diversifier, note->pk_d,
                                note->cm, note->nf,
                                pindex_new->nHeight);
                        }
                    }
                }
                /* Mark spent nullifiers */
                if (pblock->vtx[i].num_shielded_spend > 0)
                    wallet_mark_sapling_nullifiers_spent(
                        wallet,
                        (struct transaction *)&pblock->vtx[i]);
            }
            wallet->best_block_height = pindex_new->nHeight;
        }
    }

    /* Remove confirmed transactions from mempool */
    {
        struct tx_mempool *mempool = process_block_mempool();
        if (mempool)
            tx_mempool_remove_for_block(mempool,
                pblock->vtx, pblock->num_vtx,
                (unsigned int)pindex_new->nHeight);
    }

    /* Do not write derived SQLite projections from the consensus hot path.
     * The active chain, block index, and coins view above are authoritative.
     * The block/tx SQLite projection is repairable from verified block bytes,
     * while writing it here creates a second SQLite writer competing with
     * coins_view_sqlite during boot and at-tip activation. Explicit import /
     * catchup paths still use node_db_sync_connect_block() for projection
     * backfill under the DB service's write ownership. */
    {
        struct node_db *ndb = process_block_node_db_internal();
        if (ndb) {
            chain_advance_coordinator_note_projection_deferred(
                pindex_new->nHeight, "consensus_path");
            /* Wallet tx scan deferred to tip — expensive per-tx SQLite
             * queries slow down IBD and can corrupt heap (db_wallet_utxo_find
             * allocates per-call). Use rescanblockchain RPC after tip-sync. */

            /* coins_best_block is updated by coins_view_sqlite_batch_write
             * when the coins cache flushes to SQLite. Do NOT update it
             * per-block here — it creates a consistency gap where
             * coins_best_block points ahead of the actual flushed UTXO
             * set. On crash, the node would think UTXOs are current
             * when they're actually stale in the cache. */
        }
    }

    /* Append block hash to Merkle Mountain Range */
    if (pindex_new->phashBlock)
        rpc_blockchain_mmr_append(pindex_new->phashBlock->data);

    /* Append rich leaf to Merkle Mountain Belt (O(1) per block) */
    if (pindex_new->phashBlock) {
        struct mmb_leaf mmb_leaf;
        mmb_leaf_from_block(&mmb_leaf,
            pindex_new->phashBlock->data,
            pindex_new->nHeight, pindex_new->nTime, pindex_new->nBits,
            pindex_new->hashFinalSaplingRoot.data,
            (const uint8_t *)pindex_new->nChainWork.pn);
        rpc_blockchain_mmb_append(&mmb_leaf);
    }

    /* Deferred MMR verification: if this node received a UTXO snapshot
     * via fast sync, verify the offered MMR root matches our locally-built
     * MMR once we've synced headers to the snapshot height. This binds
     * the imported UTXO set to the PoW chain cryptographically. */
    {
        struct coins_view_sqlite *coins_sqlite_ptr =
            process_block_coins_sqlite_ptr();
        if (coins_sqlite_ptr && coins_sqlite_ptr->db) {
            static int32_t s_mmr_check_height = -1;
            static uint8_t s_mmr_expected[32];
            static bool s_mmr_loaded = false;
            static bool s_mmr_verified = false;

            if (!s_mmr_loaded && coins_sqlite_ptr->db) {
                sqlite3_stmt *qs = NULL;
                sqlite3_prepare_v2(coins_sqlite_ptr->db,
                    "SELECT value FROM node_state WHERE key='snapshot_mmr_height'",
                    -1, &qs, NULL);
                if (qs && AR_STEP_ROW_READONLY(qs) == SQLITE_ROW) {
                    const void *blob = sqlite3_column_blob(qs, 0);
                    if (blob && sqlite3_column_bytes(qs, 0) >= 4)
                        memcpy(&s_mmr_check_height, blob, 4);
                }
                if (qs) sqlite3_finalize(qs);

                if (s_mmr_check_height > 0) {
                    sqlite3_prepare_v2(coins_sqlite_ptr->db,
                        "SELECT value FROM node_state WHERE key='snapshot_mmr_root'",
                        -1, &qs, NULL);
                    if (qs && AR_STEP_ROW_READONLY(qs) == SQLITE_ROW) {
                        const void *blob = sqlite3_column_blob(qs, 0);
                        if (blob && sqlite3_column_bytes(qs, 0) >= 32)
                            memcpy(s_mmr_expected, blob, 32);
                    }
                    if (qs) sqlite3_finalize(qs);
                }
                s_mmr_loaded = true;
            }

            if (!s_mmr_verified && s_mmr_check_height > 0 &&
                pindex_new->nHeight == s_mmr_check_height) {
                struct mmr *m = rpc_blockchain_get_mmr();
                if (m && m->num_leaves > 0) {
                    uint8_t local_root[32];
                    mmr_root(m, local_root);
                    if (memcmp(local_root, s_mmr_expected, 32) == 0) {
                        printf("*** MMR VERIFICATION PASSED at height %d ***\n"
                               "    Snapshot UTXO set is cryptographically bound "
                               "to PoW chain (%llu blocks)\n",
                               s_mmr_check_height,
                               (unsigned long long)m->num_leaves);
                        event_emitf(EV_UTXO_CHECKPOINT_PASS, 0,
                                    "MMR verified at h=%d leaves=%llu",
                                    s_mmr_check_height,
                                    (unsigned long long)m->num_leaves);
                        /* Clear the deferred check — it passed */
                        sqlite3_exec(coins_sqlite_ptr->db,
                            "DELETE FROM node_state WHERE key IN "
                            "('snapshot_mmr_root','snapshot_mmr_height')",
                            NULL, NULL, NULL);
                    } else {
                        char exp_hex[65], got_hex[65];
                        for (int i = 0; i < 32; i++) {
                            sprintf(exp_hex + i*2, "%02x", s_mmr_expected[i]);
                            sprintf(got_hex + i*2, "%02x", local_root[i]);
                        }
                        fprintf(stderr, // obs-ok:pre-existing-diagnostic
                            "*** MMR VERIFICATION FAILED at height %d ***\n"
                            "  Expected: %s\n"
                            "  Got:      %s\n"
                            "  The imported UTXO snapshot does NOT match the "
                            "PoW chain! This node may have received tampered data.\n",
                            s_mmr_check_height, exp_hex, got_hex);
                        event_emitf(EV_UTXO_CHECKPOINT_FAIL, 0,
                                    "MMR FAILED at h=%d expected=%s got=%s",
                                    s_mmr_check_height, exp_hex, got_hex);
                    }
                    s_mmr_verified = true;
                }
            }
        }
    }

    /* Every 100 blocks: append UTXO commitment to MMR.
     * Uses the O(1) XOR accumulator instead of O(N) SHA3 full-table scan. */
    if (pindex_new->phashBlock) {
        rpc_blockchain_maybe_commit(pindex_new->nHeight,
                                     pindex_new->phashBlock->data,
                                     coins_tip->commitment.accumulator,
                                     coins_tip->commitment.count);
    }

    /* Periodically flush coins cache to SQLite.
     * If flush fails, we MUST stop connecting blocks. Continuing would
     * spend UTXOs that were never written to SQLite, causing permanent
     * UTXO loss (the "create → refuse flush → spend → later flush DELETEs
     * a UTXO that was never INSERTed" bug). */
    if (!flush_coins_if_needed(coins_tip, false)) {
        fprintf(stderr, // obs-ok:pre-existing-diagnostic
                "connect_block: coins flush failed at height %d "
                "— halting block connection to prevent UTXO loss\n",
                pindex_new->nHeight);
        if (pblock == &local_block)
            block_free(&local_block);
        trace_set_status(ct_span, TRACE_STATUS_ERROR);
        trace_end(ct_span);
        return false;
    }

    /* Flat-file sapling checkpoint. Runs after a successful
     * coins flush so any state we write here is consistent with what
     * just landed on disk. Every 10K blocks; no-op if the checkpoint
     * path isn't configured. */
    sapling_checkpoint_maybe_flush(pindex_new->nHeight);

    if (pblock == &local_block)
        block_free(&local_block);
    trace_end(ct_span);
    return true;
}

/* disconnect_tip() moved to lib/validation/src/disconnect_tip.c
 * during WS-6 phase 1 file-level split. */

/* ── Reorg Recovery ─────────────────────────────────────────────
 *
 * When disconnect_tip fails (missing undo data), the node is stuck:
 * the active chain tip cannot be rolled back, and the better chain
 * cannot be connected. This function implements a clean recovery:
 *
 *   1. SYNC_REORG → SYNC_REORG_RECOVERY (state machine transition)
 *   2. Clear the in-memory UTXO cache (discard stale entries)
 *   3. Force the active chain tip to the fork point
 *   4. Set coins_best_block in both memory and SQLite to fork hash
 *   5. Emit EV_REORG_DISCONNECT_FAILED + EV_REORG_RECOVERY_COMPLETE
 *
 * After recovery, activate_best_chain proceeds to connect blocks
 * from the fork point forward, rebuilding UTXOs for that range.
 *
 * Returns true if recovery succeeded, false if unrecoverable. */
static bool recover_from_disconnect_failure(
    struct main_state *ms,
    struct coins_view_cache *coins_tip,
    struct block_index *fork,
    int stuck_height)
{
    if (!fork || !fork->phashBlock)
        LOG_FAIL("validation", "recover_from_disconnect_failure called with null fork or null phashBlock");

    /* State machine: REORG → REORG_RECOVERY */
    sync_set_state(SYNC_REORG_RECOVERY,
                   "disconnect failed, clearing UTXO cache");

    event_emitf(EV_REORG_DISCONNECT_FAILED, 0,
        "stuck_h=%d fork_h=%d", stuck_height, fork->nHeight);

    /* Step 1: Clear the in-memory UTXO cache.
     * Do NOT flush — the cache contains stale entries from the
     * partially-disconnected chain that would corrupt SQLite. */
    coins_view_cache_clear(coins_tip);

    /* Steps 2 + 3: Force the active chain tip to the fork point AND
     * set coins_best_block to the fork hash in one atomic csr
     * commit. Previously these were two separate mutations that
     * could leave the six sources of truth briefly inconsistent —
     * exactly the shape of bug the chain_state_repository exists
     * to prevent.
     *
     * Note: the SQLite UTXO set may not exactly match the fork point
     * (blocks connected after the fork consumed UTXOs). This is
     * acceptable — connect_block will fail for those blocks, and
     * the operator can run `importchainstate` to get a clean set.
     * We do NOT reimport from LevelDB here because LevelDB's UTXO
     * set is at a different (later) height than the fork point. */
    process_block_commit_tip(ms, coins_tip, fork,
        "process_block.recover_from_disconnect_failure", false, true, NULL);

    /* Step 4: Flush any pending SQLite batch. */
    {
        struct node_db *ndb = process_block_node_db_internal();
        if (ndb && ndb->sync_in_batch)
            node_db_sync_flush(ndb);
    }

    /* Step 6: Clear BLOCK_FAILED flags on blocks above the fork point.
     * Previous connect attempts may have marked blocks invalid due to
     * stale UTXO data. After reimport, those blocks are valid. */
    {
        size_t iter = 0;
        struct block_index *bi = NULL;
        int cleared = 0;
        while (block_map_next(&ms->map_block_index, &iter, NULL, &bi)) {
            if (!bi) continue;
            if (bi->nHeight > fork->nHeight &&
                (bi->nStatus & BLOCK_FAILED_MASK)) {
                bi->nStatus &= ~BLOCK_FAILED_MASK;
                cleared++;
            }
        }
        if (cleared > 0)
            fprintf(stderr, // obs-ok:pre-existing-diagnostic
                    "reorg_recovery: cleared BLOCK_FAILED on %d blocks "
                    "above fork h=%d\n", cleared, fork->nHeight);
    }

    event_emitf(EV_REORG_RECOVERY_COMPLETE, 0,
        "fork_h=%d cache_cleared=true", fork->nHeight);

    fprintf(stderr,
        "activate_best_chain: recovered from disconnect failure, "
        "chain reset to h=%d, UTXO cache cleared\n", fork->nHeight);

    return true;
}

bool activate_best_chain(struct validation_state *state,
                         struct main_state *ms,
                         struct coins_view_cache *coins_tip,
                         const struct chain_params *params,
                         struct block *pblock,
                         const char *datadir)
{
    /* Note: anchor/UTXO guards are now handled by the chain activation
     * controller (activation_request_connect). This function should only
     * be called via the controller. */

    /* clear the "more pending" signal — we are about to
     * try to make progress. The loop below sets it again if it returns
     * early because of the per-pass child-connect limit. */
    process_block_set_active_tip_more_pending(false);

    struct block_index *pindex_most_work = NULL;

    if (pblock) {
        struct uint256 block_hash;
        block_get_hash(pblock, &block_hash);
        struct block_index *pindex_new =
            block_map_find(&ms->map_block_index, &block_hash);
        struct block_index *tip =
            active_chain_tip(&ms->chain_active);
        if (pindex_new && tip && tip->phashBlock &&
            pindex_new != tip &&
            uint256_eq(&pblock->header.hashPrevBlock, tip->phashBlock)) {
            if (pindex_new->pprev != tip ||
                pindex_new->nHeight != tip->nHeight + 1) {
                pindex_new->pprev = tip;
                pindex_new->nHeight = tip->nHeight + 1;
                block_index_build_skip(pindex_new);
                struct arith_uint256 proof = GetBlockProof(pindex_new);
                arith_uint256_add(&pindex_new->nChainWork,
                                  &tip->nChainWork, &proof);
                fprintf(stderr, // obs-ok:pre-existing-diagnostic
                        "activate_best_chain: repaired provided near-tip "
                        "index h=%d from header prev=tip\n",
                        pindex_new->nHeight);
            }
            if (!connect_tip(state, ms, coins_tip, pindex_new,
                             pblock, params, datadir)) {
                fprintf(stderr, // obs-ok:pre-existing-diagnostic
                    "activate_best_chain: provided near-tip connect FAILED "
                    "at height %d reason=%s invalid=%d\n",
                    pindex_new->nHeight,
                    state->reject_reason[0] ? state->reject_reason
                                            : "unknown",
                    validation_state_is_invalid(state));
                return false;
            }
            return true;
        }
        if (pindex_new && tip && tip->nHeight > 1000000 &&
            pindex_new->nHeight > tip->nHeight + 512) {
            /* Wake the gap-fill service to enqueue the intermediate
             * blocks from tip+1 upward. The far-ahead live block cannot
             * connect yet, so priority-queueing only that block creates a
             * dead end and can crowd out the connectable bottom range. */
            gap_fill_kick();
            printf("activate_best_chain: defer far-ahead live block h=%d "
                   "tip=%d (gap-fill kicked)\n",
                   pindex_new->nHeight, tip->nHeight);
            return true;
        }

        if (pindex_new && tip && pindex_new->nHeight > tip->nHeight &&
            pindex_new->nHeight <= tip->nHeight + 512) {
            struct block_index *path[512];
            int path_len = 0;
            struct block_index *walk = pindex_new;
            bool missing_data = false;
            bool near_tip_block = (pindex_new->nHeight == tip->nHeight + 1);

            if (near_tip_block && pindex_new->pprev != tip &&
                tip->phashBlock) {
                bool prev_is_tip = uint256_eq(&pblock->header.hashPrevBlock,
                                               tip->phashBlock);
                if (!prev_is_tip && pindex_new->pprev &&
                    pindex_new->pprev->phashBlock) {
                    prev_is_tip = uint256_eq(pindex_new->pprev->phashBlock,
                                             tip->phashBlock);
                }
                if (prev_is_tip) {
                    pindex_new->pprev = tip;
                    block_index_build_skip(pindex_new);
                }
            }

            if (near_tip_block && pindex_new->pprev == tip) {
                if (!connect_tip(state, ms, coins_tip, pindex_new,
                                 pblock, params, datadir)) {
                    fprintf(stderr, // obs-ok:pre-existing-diagnostic
                        "activate_best_chain: direct near-tip connect FAILED "
                        "at height %d reason=%s invalid=%d\n",
                        pindex_new->nHeight,
                        state->reject_reason[0] ? state->reject_reason
                                                : "unknown",
                        validation_state_is_invalid(state));
                    return false;
                }
                return true;
            }

            while (walk && walk != tip && path_len < 512) {
                if (!(walk->nStatus & BLOCK_HAVE_DATA)) {
                    missing_data = true;
                    break;
                }
                path[path_len++] = walk;
                walk = walk->pprev;
            }

            if (walk == tip && !missing_data) {
                for (int i = path_len - 1; i >= 0; i--) {
                    struct block *use_block = (path[i] == pindex_new)
                                            ? pblock : NULL;
                    if (!connect_tip(state, ms, coins_tip, path[i],
                                     use_block, params, datadir)) {
                        fprintf(stderr, // obs-ok:pre-existing-diagnostic
                            "activate_best_chain: fast connect_tip FAILED "
                            "at height %d reason=%s invalid=%d\n",
                            path[i]->nHeight,
                            state->reject_reason[0] ? state->reject_reason
                                                    : "unknown",
                            validation_state_is_invalid(state));
                        return false;
                    }
                }
                return true;
            }

            if (missing_data && walk && walk->phashBlock) {
                struct download_manager *dm_abc = msg_get_download_mgr();
                if (dm_abc)
                    dl_queue_priority(dm_abc, walk->phashBlock,
                                      walk->nHeight);
            }

            if (near_tip_block) {
                fprintf(stderr, // obs-ok:pre-existing-diagnostic
                    "activate_best_chain: fast path could not connect "
                    "near-tip block h=%d tip=%d pprev_h=%d "
                    "have_data=%d missing_data=%d\n",
                    pindex_new->nHeight, tip->nHeight,
                    pindex_new->pprev ? pindex_new->pprev->nHeight : -1,
                    (pindex_new->nStatus & BLOCK_HAVE_DATA) != 0,
                    missing_data);
            }

            /* fork-tip rollback.
             *
             * If the peer's incoming block claims a parent hash that
             * matches our tip's PARENT (not our tip), our local tip
             * is on a 1-block fork the network rejected. A 1-block
             * reorg is well within MAX_REORG_LENGTH, so we can
             * safely disconnect our tip and re-extend with the peer's
             * block. Without this, a single bad tip block strands
             * the node forever even though gap-fill downloads the
             * correct successors. */
            if (near_tip_block && pblock && tip && tip->pprev &&
                tip->pprev->phashBlock) {
                bool extends_tip_parent = uint256_eq(
                    &pblock->header.hashPrevBlock,
                    tip->pprev->phashBlock);
                if (extends_tip_parent) {
                    fprintf(stderr, // obs-ok:pre-existing-diagnostic
                        "activate_best_chain: fork-tip rollback "
                        "h=%d (local tip on wrong 1-block fork; peer "
                        "block extends from parent h=%d)\n",
                        tip->nHeight, tip->pprev->nHeight);
                    event_emitf(EV_REORG_START, 0,
                                "fork_tip_rollback h=%d new_h=%d",
                                tip->nHeight, pindex_new->nHeight);
                    if (!disconnect_tip(state, ms, coins_tip, datadir)) {
                        fprintf(stderr, // obs-ok:pre-existing-diagnostic
                            "activate_best_chain: fork-tip rollback "
                            "FAILED to disconnect tip h=%d; chain "
                            "remains stuck\n", tip->nHeight);
                        event_emitf(EV_REORG_DISCONNECT_FAILED, 0,
                                    "fork_tip h=%d", tip->nHeight);
                        return false;
                    }
                    struct block_index *new_tip =
                        active_chain_tip(&ms->chain_active);
                    if (new_tip && pindex_new->pprev != new_tip) {
                        pindex_new->pprev = new_tip;
                        block_index_build_skip(pindex_new);
                    }
                    if (!connect_tip(state, ms, coins_tip, pindex_new,
                                     pblock, params, datadir)) {
                        fprintf(stderr, // obs-ok:pre-existing-diagnostic
                            "activate_best_chain: fork-tip rollback "
                            "connect FAILED at h=%d (chain now at "
                            "h=%d)\n",
                            pindex_new->nHeight,
                            new_tip ? new_tip->nHeight : -1);
                        return false;
                    }
                    event_emitf(EV_REORG_RECOVERY_COMPLETE, 0,
                                "fork_tip_rollback new_h=%d",
                                pindex_new->nHeight);
                    return true;
                }
            }

            /* sibling-fork rollback.
             *
             * Scenario: tip and pindex_new->pprev are SIBLING blocks
             * at the same height (h=tip->nHeight), both extending
             * the same grandparent. Live evidence:
             *   pprev_h=3087032 tip=3087032 — pindex_new->pprev is
             *   a different block_index at h=3087032 from our tip.
             * Both forks share tip->pprev as common ancestor, so the
             * reorg depth is 1 — well within MAX_REORG_LENGTH.
             *
             * Recovery: disconnect our tip, connect the peer's
             * sibling as new h=tip->nHeight, then connect pblock as
             * h=tip->nHeight+1. */
            if (near_tip_block && pblock && tip && pindex_new->pprev &&
                pindex_new->pprev != tip &&
                pindex_new->pprev->nHeight == tip->nHeight &&
                pindex_new->pprev->pprev == tip->pprev &&
                (pindex_new->pprev->nStatus & BLOCK_HAVE_DATA)) {
                struct block_index *peer_parent = pindex_new->pprev;
                fprintf(stderr, // obs-ok:pre-existing-diagnostic
                    "activate_best_chain: sibling-fork rollback h=%d "
                    "(our tip and peer's parent are siblings at "
                    "h=%d, both extending h=%d; switching to peer's "
                    "fork)\n",
                    tip->nHeight, tip->nHeight,
                    tip->pprev ? tip->pprev->nHeight : -1);
                event_emitf(EV_REORG_START, 0,
                            "sibling_fork_rollback h=%d new_h=%d",
                            tip->nHeight, pindex_new->nHeight);
                if (!disconnect_tip(state, ms, coins_tip, datadir)) {
                    fprintf(stderr, // obs-ok:pre-existing-diagnostic
                        "activate_best_chain: sibling-fork rollback "
                        "FAILED to disconnect tip h=%d\n",
                        tip->nHeight);
                    event_emitf(EV_REORG_DISCONNECT_FAILED, 0,
                                "sibling_fork h=%d", tip->nHeight);
                    return false;
                }
                if (!connect_tip(state, ms, coins_tip, peer_parent,
                                 NULL, params, datadir)) {
                    fprintf(stderr, // obs-ok:pre-existing-diagnostic
                        "activate_best_chain: sibling-fork rollback "
                        "could not connect peer_parent h=%d\n",
                        peer_parent->nHeight);
                    return false;
                }
                if (!connect_tip(state, ms, coins_tip, pindex_new,
                                 pblock, params, datadir)) {
                    fprintf(stderr, // obs-ok:pre-existing-diagnostic
                        "activate_best_chain: sibling-fork rollback "
                        "could not connect new tip h=%d\n",
                        pindex_new->nHeight);
                    return false;
                }
                event_emitf(EV_REORG_RECOVERY_COMPLETE, 0,
                            "sibling_fork_rollback new_h=%d",
                            pindex_new->nHeight);
                return true;
            }

            fprintf(stderr, // obs-ok:pre-existing-diagnostic
                "activate_best_chain: near-tip block h=%d was not a direct "
                "extension of tip=%d; falling through to most-work reorg "
                "selection\n",
                pindex_new->nHeight, tip ? tip->nHeight : -1);
        }
    }

    int connected_tip_children = 0;
    int tip_child_connect_limit = active_tip_child_connect_limit();

    do {
        if (g_shutdown_requested) {
            fprintf(stderr, // obs-ok:pre-existing-diagnostic
                    "activate_best_chain: shutdown requested before "
                    "activation pass, flushing coins at h=%d\n",
                    active_chain_height(&ms->chain_active));
            flush_coins_if_needed(coins_tip, true);
            return true;
        }

        struct block_index *tip_child_base =
            active_chain_tip(&ms->chain_active);
        struct block_index *tip_child =
            find_best_active_tip_child(ms, tip_child_base, datadir);
        if (!tip_child)
            tip_child = find_verified_unlinked_active_tip_child(
                ms, tip_child_base, datadir);
        if (tip_child) {
            if (s_utxo_activation_paused_height == tip_child->nHeight) {
                fprintf(stderr, // obs-ok:pre-existing-diagnostic
                    "activate_best_chain: activation paused at h=%d "
                    "after unrecovered UTXO mismatch and recent reimport\n",
                    tip_child->nHeight);
                return true;
            }
            fprintf(stderr, // obs-ok:pre-existing-diagnostic
                    "activate_best_chain: connecting active-tip child "
                    "h=%d from tip=%d have_data=%d chain_tx=%lld\n",
                    tip_child->nHeight,
                    tip_child_base ? tip_child_base->nHeight : -1,
                    (tip_child->nStatus & BLOCK_HAVE_DATA) != 0,
                    (long long)tip_child->nChainTx);
            if (!connect_tip(state, ms, coins_tip, tip_child,
                             NULL, params, datadir)) {
                fprintf(stderr, // obs-ok:pre-existing-diagnostic
                        "activate_best_chain: active-tip child connect "
                        "FAILED h=%d reason=%s invalid=%d\n",
                        tip_child->nHeight,
                        state->reject_reason[0] ? state->reject_reason
                                                : "unknown",
                        validation_state_is_invalid(state));
                if (process_block_is_missing_utxo_failure(state)) {
                    process_block_note_utxo_failure(ms, coins_tip,
                                                   tip_child->nHeight,
                                                   datadir);
                    validation_state_init(state);
                    if (s_utxo_activation_paused_height ==
                        tip_child->nHeight)
                        return true;
                    continue;
                }
                return false;
            }
            if (g_shutdown_requested) {
                fprintf(stderr, // obs-ok:pre-existing-diagnostic
                    "activate_best_chain: shutdown requested after "
                    "connecting h=%d, flushing coins\n",
                    tip_child->nHeight);
                flush_coins_if_needed(coins_tip, true);
                return true;
            }
            connected_tip_children++;
            if (connected_tip_children >= tip_child_connect_limit) {
                fprintf(stderr, // obs-ok:pre-existing-diagnostic
                    "activate_best_chain: paused after connecting %d "
                    "active-tip children (limit=%d) so service startup and "
                    "RPC stay responsive\n",
                    connected_tip_children, tip_child_connect_limit);
                /* tell the activation controller drain loop
                 * that another pass will likely make more progress.
                 * Without this we'd wait for the next P2P block to
                 * trigger a fresh activation. */
                process_block_set_active_tip_more_pending(true);
                return true;
            }
            continue;
        }

        pindex_most_work = find_most_work_chain(ms);

        struct block_index *tip = active_chain_tip(&ms->chain_active);
        if (!pindex_most_work || pindex_most_work == tip)
            return true;
        /* Don't reorg to a chain with less or equal work than our tip.
         * This happens when nChainTx gaps make find_most_work_chain
         * return a shorter chain that is actually part of our chain. */
        if (tip && arith_uint256_compare(&pindex_most_work->nChainWork,
                                          &tip->nChainWork) <= 0)
            return true;
        printf("activate_best_chain: tip=%d most_work=%d\n",
               tip ? tip->nHeight : -1,
               pindex_most_work->nHeight);
        event_emitf(EV_BOOT_ACTIVATE, 0, "tip=%d most_work=%d",
                    tip ? tip->nHeight : -1,
                    pindex_most_work->nHeight);

        /* Check reorg length */
        if (tip) {
            /* hard checkpoint invariant.
             *
             * ZCL_FINALITY_DEPTH (=10) blocks deep is the protocol
             * promise — anything older is permanently immutable.
             * Refuse to even start the fork-point walk if the
             * candidate chain would reorg below that floor. Saves
             * the wasted walk, removes a silent-CPU stall source,
             * and gives the operator a clear log line.
             *
             * We don't know the fork-point yet (that's what the
             * walk computes), but `pindex_most_work->nHeight` is
             * a lower bound on the fork-point height: any walk
             * must end at or below it. If most_work itself is
             * below tip - ZCL_FINALITY_DEPTH, the reorg is forbidden
             * regardless of where the fork ends up. */
            {
                const char *reason = NULL;
                if (!reorg_is_allowed(tip->nHeight,
                                       pindex_most_work->nHeight,
                                       &reason)) {
                    fprintf(stderr, // obs-ok:pre-existing-diagnostic
                        "activate_best_chain: refusing reorg below "
                        "finality floor tip=%d most_work=%d depth=%d "
                        "reason=%s (ZCL_FINALITY_DEPTH=%d)\n",
                        tip->nHeight, pindex_most_work->nHeight,
                        tip->nHeight - pindex_most_work->nHeight,
                        reason ? reason : "(null)",
                        ZCL_FINALITY_DEPTH);
                    event_emitf(EV_CHAIN_TIP_REJECTED, 0,
                                "code=below_finality_depth tip=%d "
                                "most_work=%d depth=%d",
                                tip->nHeight,
                                pindex_most_work->nHeight,
                                tip->nHeight - pindex_most_work->nHeight);
                    return true;
                }
            }

            /* Find fork point.
             * SAFETY: check pprev at every step — blocks loaded from
             * flat file may have dangling pprev if the file was saved
             * before all P2P blocks were linked.
             * CYCLE SAFETY: cap step count and require strict
             * monotonicity on nHeight. A corrupted block index from a
             * half-completed chain restore can leave pprev ring-shaped;
             * without these guards the walk loops forever and the boot
             * stays silent at 100% CPU (observed: 14+ min stall before
             * this guard was added). Mirrors the protection on the
             * connect-path walk below. */
            #define ACTIVATE_PPREV_WALK_MAX 200000
            struct block_index *fork = tip;
            {
                int steps = 0;
                int last_h = INT_MAX;
                while (fork && fork->pprev &&
                       fork->nHeight > pindex_most_work->nHeight) {
                    if (steps++ > ACTIVATE_PPREV_WALK_MAX ||
                        fork->pprev->nHeight >= fork->nHeight) {
                        fprintf(stderr, // obs-ok:pre-existing-diagnostic
                            "activate_best_chain: aborting corrupt pprev "
                            "walk (fork-down) at h=%d steps=%d tip=%d "
                            "most_work=%d\n",
                            fork->nHeight, steps,
                            tip ? tip->nHeight : -1,
                            pindex_most_work->nHeight);
                        return true;
                    }
                    last_h = fork->nHeight;
                    fork = fork->pprev;
                }
                (void)last_h;
            }
            if (!fork) return true; /* chain broken, wait for P2P */
            struct block_index *walk = pindex_most_work;
            {
                int steps = 0;
                while (walk && walk->pprev &&
                       walk->nHeight > fork->nHeight) {
                    if (steps++ > ACTIVATE_PPREV_WALK_MAX ||
                        walk->pprev->nHeight >= walk->nHeight) {
                        fprintf(stderr, // obs-ok:pre-existing-diagnostic
                            "activate_best_chain: aborting corrupt pprev "
                            "walk (most-work-down) at h=%d steps=%d "
                            "tip=%d most_work=%d\n",
                            walk->nHeight, steps,
                            tip ? tip->nHeight : -1,
                            pindex_most_work->nHeight);
                        return true;
                    }
                    walk = walk->pprev;
                }
            }
            {
                int steps = 0;
                while (fork && walk && fork != walk &&
                       fork->pprev && walk->pprev) {
                    if (steps++ > ACTIVATE_PPREV_WALK_MAX ||
                        fork->pprev->nHeight >= fork->nHeight ||
                        walk->pprev->nHeight >= walk->nHeight) {
                        fprintf(stderr, // obs-ok:pre-existing-diagnostic
                            "activate_best_chain: aborting corrupt pprev "
                            "walk (common-ancestor) at fork_h=%d walk_h=%d "
                            "steps=%d tip=%d most_work=%d\n",
                            fork->nHeight, walk->nHeight, steps,
                            tip ? tip->nHeight : -1,
                            pindex_most_work->nHeight);
                        return true;
                    }
                    fork = fork->pprev;
                    walk = walk->pprev;
                }
            }
            #undef ACTIVATE_PPREV_WALK_MAX
            /* If pprev walk couldn't find a common ancestor (broken
             * links after LDB import), treat this as extending the
             * current chain — use tip as the fork point.  Do NOT
             * set fork=NULL which triggers a destructive genesis reset. */
            if (fork != walk)
                fork = tip;

            /* During IBD, allow deep reorgs — fork blocks received in
             * parallel can cause the wrong chain to be connected initially.
             * At tip (steady state), enforce the reorg limit. */
            /* If most_work chain is not actually better, skip reorg */
            if (pindex_most_work->nHeight <= tip->nHeight) {
                return true; /* current chain is already at or above most_work */
            }

            int reorg_depth = tip->nHeight - (fork ? fork->nHeight : -1);
            bool in_ibd = (sync_get_state() <= SYNC_BLOCKS_DOWNLOAD);
            /* Skip reorg limit when fork is NULL but we're extending the
             * chain (not actually reorging). This happens after LDB import
             * when pprev pointers aren't fully resolved. */
            bool extending = (!fork && pindex_most_work->nHeight > tip->nHeight &&
                              pindex_most_work->nHeight <= tip->nHeight + 200);
            if (!extending && !in_ibd && reorg_depth > ZCL_FINALITY_DEPTH) {
                printf("activate_best_chain: reorg depth %d exceeds finality depth %d\n",
                       reorg_depth, ZCL_FINALITY_DEPTH);
                return false;
            }
            if (!extending && in_ibd && reorg_depth > MAX_IBD_REORG_LENGTH) {
                printf("activate_best_chain: IBD reorg depth %d exceeds "
                       "max %d\n", reorg_depth, MAX_IBD_REORG_LENGTH);
                return false;
            }

            /* Disconnect blocks from current tip to fork point */
            if (!fork) {
                /* No common ancestor found — chains are completely
                 * divergent (broken pprev links). Reset to genesis.
                 * This is a clear rollback, so the csr commit uses
                 * typed rollback authorization (via the helper) and does not move
                 * pindex_best_header — the header tip stays put so
                 * accept_block_header's retry logic can rebuild the
                 * chain upward. */
                struct block_index *genesis = active_chain_at(
                    &ms->chain_active, 0);
                if (genesis) {
                    process_block_commit_tip(ms, coins_tip, genesis,
                        "process_block.activate_best_chain.no_fork_reset",
                        false, false, NULL);
                    printf("activate_best_chain: no fork point, "
                           "reset to genesis\n");
                }
            } else if (tip->nHeight > fork->nHeight) {
                event_emitf(EV_REORG_START, 0, "fork=%d tip=%d depth=%d",
                            fork->nHeight, tip->nHeight,
                            tip->nHeight - fork->nHeight);
                sync_set_state(SYNC_REORG, "chain reorganization");
                while (active_chain_tip(&ms->chain_active) != fork) {
                    if (!disconnect_tip(state, ms, coins_tip, datadir)) {
                        int stuck_h = active_chain_height(&ms->chain_active);
                        if (!recover_from_disconnect_failure(
                                ms, coins_tip, fork, stuck_h)) {
                            sync_set_state(SYNC_FAILED,
                                "unrecoverable disconnect failure");
                            LOG_FAIL("validation", "unrecoverable disconnect failure during reorg at height %d",
                                     active_chain_height(&ms->chain_active));
                        }
                        break; /* exit disconnect loop, proceed to connect */
                    }
                }
            }
        }

        /* Connect blocks from fork to most-work tip.
         * Count total depth, then allocate dynamically.
         * Cap at 500 blocks per batch to prevent OOM when connecting
         * 1M+ blocks (e.g. after LDB import with symlinked blk files).
         * The outer do-while loop re-finds most_work and continues. */
        #define CONNECT_BATCH_MAX 500

        struct block_index *current_tip = active_chain_tip(&ms->chain_active);
        int total_depth = 0;
        int last_walk_height = INT_MAX;
        for (struct block_index *w = pindex_most_work;
             w && w != current_tip; w = w->pprev) {
            if (total_depth > 200000 ||
                (last_walk_height != INT_MAX &&
                 w->nHeight >= last_walk_height)) {
                fprintf(stderr, // obs-ok:pre-existing-diagnostic
                        "activate_best_chain: aborting corrupt pprev walk "
                        "at h=%d last_h=%d depth=%d tip=%d most_work=%d\n",
                        w->nHeight, last_walk_height, total_depth,
                        current_tip ? current_tip->nHeight : -1,
                        pindex_most_work ? pindex_most_work->nHeight : -1);
                return true;
            }
            last_walk_height = w->nHeight;
            total_depth++;
        }

        int batch_depth = total_depth > CONNECT_BATCH_MAX
                        ? CONNECT_BATCH_MAX : total_depth;

        printf("activate_best_chain: connect path depth=%d "
               "(from h=%d to tip h=%d)%s\n",
               total_depth, pindex_most_work->nHeight,
               current_tip ? current_tip->nHeight : -1,
               total_depth > CONNECT_BATCH_MAX ? " [batched]" : "");
        fflush(stdout);

        /* Walk backward from most_work but only collect batch_depth
         * entries. For batched connects, we start from the OLDEST
         * needed block (closest to current tip), not from most_work. */
        struct block_index **connect_path = zcl_malloc(
            (size_t)batch_depth * sizeof(struct block_index *), "connect_path");
        if (!connect_path)
            LOG_FAIL("validation", "malloc failed for connect_path (%d entries)", batch_depth);

        /* Walk all the way back to build the path from tip to most_work,
         * but only keep the last batch_depth entries (closest to tip). */
        int path_len = 0;
        struct block_index *w = pindex_most_work;
        /* Skip entries beyond our batch window */
        int skip = total_depth - batch_depth;
        for (int s = 0; s < skip && w && w != current_tip; s++)
            w = w->pprev;
        for (; w && w != current_tip && path_len < batch_depth;
             w = w->pprev)
            connect_path[path_len++] = w;

        /* Connect in forward order (reverse of path) */
        if (path_len > 0) {
            printf("activate_best_chain: first connect h=%d last h=%d "
                   "path_len=%d\n",
                   connect_path[path_len - 1]->nHeight,
                   connect_path[0]->nHeight, path_len);
            fflush(stdout);
        }
        for (int i = path_len - 1; i >= 0; i--) {
            /* Check for shutdown request (Ctrl-C during replay) */
            if (g_shutdown_requested) {
                printf("activate_best_chain: shutdown requested at height %d, "
                       "flushing coins...\n",
                       active_chain_height(&ms->chain_active));
                flush_coins_if_needed(coins_tip, true); /* force flush */
                free(connect_path);
                return true; /* clean exit, coins flushed */
            }

            struct block *use_block = NULL;
            if (pblock && i == 0) {
                struct uint256 block_hash;
                block_header_get_hash(&pblock->header, &block_hash);
                if (connect_path[0]->phashBlock &&
                    uint256_cmp(&block_hash, connect_path[0]->phashBlock) == 0)
                    use_block = pblock;
            }

            /* Only connect blocks that have data. If a block on the
             * path doesn't have data yet (header-only), stop here.
             * The download manager will fetch it; on the next call
             * to activate_best_chain we'll continue from this point. */
            if (!(connect_path[i]->nStatus & BLOCK_HAVE_DATA)) {
                /* Priority-queue this block — it's the NEXT one needed
                 * to advance the chain. Gets assigned to the next peer
                 * before any other queued blocks. */
                if (connect_path[i]->phashBlock) {
                    struct download_manager *dm_abc = msg_get_download_mgr();
                    if (dm_abc)
                        dl_queue_priority(dm_abc, connect_path[i]->phashBlock,
                                           connect_path[i]->nHeight);
                }
                /* Force flush coins to SQLite before pausing. If we
                 * connected any blocks above, their UTXOs are in the
                 * in-memory cache only. A restart without flush would
                 * lose them, corrupting the UTXO set. */
                flush_coins_if_needed(coins_tip, true);
                free(connect_path);
                return true; /* partial success, will continue later */
            }

            if (!connect_tip(state, ms, coins_tip, connect_path[i],
                            use_block, params, datadir)) {
                fprintf(stderr, // obs-ok:pre-existing-diagnostic
                    "activate_best_chain: connect_tip FAILED at height %d "
                    "reason=%s invalid=%d\n",
                    connect_path[i]->nHeight,
                    state->reject_reason[0] ? state->reject_reason
                                            : "unknown",
                    validation_state_is_invalid(state));
                event_emitf(EV_BOOT_ACTIVATE, 0, "FAILED h=%d reason=%s",
                    connect_path[i]->nHeight,
                    state->reject_reason[0] ? state->reject_reason
                                            : "unknown");

                /* Auto-recovery: if UTXO mismatches keep failing at the
                 * same height, write a flag file so boot.c reimports
                 * UTXOs from LevelDB on next restart. */
                if (process_block_is_missing_utxo_failure(state)) {
                    process_block_note_utxo_failure(ms, coins_tip,
                                                   connect_path[i]->nHeight,
                                                   datadir);
                    if (s_utxo_activation_paused_height ==
                        connect_path[i]->nHeight) {
                        validation_state_init(state);
                        free(connect_path);
                        return true;
                    }
                    /* After 10 consecutive UTXO failures at the same
                     * height, the in-memory recovery attempts are
                     * clearly exhausted — the flag file above is
                     * already written, but boot.c only consumes it on
                     * startup.  Requesting a clean shutdown lets
                     * systemd restart the process; boot then reads
                     * the flag and runs the LDB reimport in
                     * utxo_recovery_import_ldb, which restores a
                     * consistent UTXO set.  Without this escape
                     * hatch, the node burns CPU in a hot retry loop
                     * forever (observed: 4700+ identical failures
                     * over 2h 44m).
                     *
                     * Bootloop debounce: if a reimport was already
                     * attempted recently (mtime of the marker file
                     * written by utxo_recovery_import_ldb) and we're
                     * STILL hot-looping at the same height, the LDB
                     * source didn't carry the missing UTXO either
                     * (observed 2026-04-22 04:45 — zclassicd's
                     * on-disk chainstate was memtable-stale at
                     * h=3,078,003).  Auto-restarting just burns
                     * another 5-8min sapling rebuild.  Emit a FATAL
                     * event, keep running (visibly stuck), and wait
                     * for operator intervention. */
                }
                if (validation_state_is_invalid(state)) {
                    /* Block failed validation — mark it and retry.
                     * The do-while loop will call find_most_work_chain
                     * again, which skips this failed block and finds
                     * an alternative chain. This matches ZClassic C++
                     * ActivateBestChainStep behavior. */
                    validation_state_init(state);
                    connect_path = NULL; /* prevent double-free at line 979 */
                    break; /* break inner loop, retry outer do-while */
                }
                /* System error (not invalid block) — abort */
                free(connect_path);
                return false;
            }
        }
        /* Flush coins after each batch to bound memory usage.
         * The outer do-while loop re-finds most_work and connects
         * the next batch until we reach the tip. */
        if (total_depth > CONNECT_BATCH_MAX) {
            flush_coins_if_needed(coins_tip, true);
            printf("activate_best_chain: batch done, flushed at h=%d "
                   "(%d remaining)\n",
                   active_chain_height(&ms->chain_active),
                   total_depth - batch_depth);
        }
        free(connect_path);

    } while (pindex_most_work != active_chain_tip(&ms->chain_active) &&
             !g_shutdown_requested);

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
    (void)coins_tip;  /* activation controller owns coins_tip reference */

    bool checked = check_block(pblock, state, params, true, true, true);
    if (!checked)
        LOG_FAIL("validation", "check_block failed: %s",
                 state->reject_reason[0] ? state->reject_reason : "unknown");

    struct block_index *pindex = NULL;
    bool requested = force_processing;

    if (!accept_block(pblock, state, ms, params, &pindex, requested, datadir))
        LOG_FAIL("validation", "accept_block failed: %s",
                 state->reject_reason[0] ? state->reject_reason : "unknown");

    /* Do NOT connect blocks if we're waiting for a UTXO snapshot.
     * Connecting blocks from genesis with an empty UTXO set permanently
     * marks valid blocks as BLOCK_FAILED (e.g. coinbase maturity checks
     * fail because the coinbase outputs were never added to the view).
     * accept_block above is safe — it only indexes the block on disk. */
    /* Connect blocks via controller (single authority). The controller
     * handles: anchor check, UTXO availability, mutex serialization. */
    {
        struct activation_exec_outcome ao;
        activation_request_connect(boot_activation_controller(),
                                   ACTIVATION_SRC_NEW_BLOCK, pblock, &ao);
        if (ao.result == ACTIVATION_EXEC_FAILED)
            LOG_FAIL("validation", "activation FAILED: %s", ao.reason);
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
        LOG_FAIL("validation", "test_block_validity: contextual_check_block_header failed");
    }
    if (!check_block(block, state, params, true, true, true)) {
        coins_view_cache_free(&view);
        LOG_FAIL("validation", "test_block_validity: check_block failed");
    }
    if (!contextual_check_block(block, state, params, pindex_prev)) {
        coins_view_cache_free(&view);
        LOG_FAIL("validation", "test_block_validity: contextual_check_block failed");
    }
    if (!connect_block(block, state, &index_dummy, &view, params, true)) {
        coins_view_cache_free(&view);
        LOG_FAIL("validation", "test_block_validity: connect_block failed");
    }

    coins_view_cache_free(&view);
    return true;
}
