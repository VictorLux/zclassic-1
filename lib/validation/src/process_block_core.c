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
unsigned int g_last_block_file_size = 0; /* extern in process_block_internal.h */

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
bool find_block_pos(struct disk_block_pos *pos, unsigned int block_size,
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

void block_index_refresh_header(struct block_index *pindex,
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

bool block_index_hydrate_from_disk(struct block_index *pindex,
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

bool process_block_commit_tip(struct main_state *ms,
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
 * accept_block()        moved to lib/validation/src/accept_block.c
 * connect_tip()         moved to lib/validation/src/connect_tip.c
 * disconnect_tip()      moved to lib/validation/src/disconnect_tip.c
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
