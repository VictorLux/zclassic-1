/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php.
 *
 * Core consensus paths split out of process_block.c: chain selection,
 * accept_block_header / accept_block, connect_tip / disconnect_tip,
 * activate_best_chain, process_new_block.
 *
 * Pure code motion. Function bodies are byte-identical to the
 * original lib/validation/src/process_block.c. */

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

/* add_to_block_index() relocated to lib/validation/src/accept_block_header.c
 * (its sole caller) in the single-engine swap so the runtime in-memory
 * block_index producer survives the eventual process_block_core.c deletion.
 * Behavior-preserving code motion; the declaration remains in
 * process_block_internal.h. */

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
                gap_fill_kick();
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

/* Evidence-side mirror of find_most_work_chain's candidate eligibility.
 *
 * The two functions scan the SAME map_block_index and MUST agree about
 * which entries represent a real competing chain. They diverged: a prior
 * recompute_index_from_genesis stamped nChainWork (and left BLOCK_HAVE_DATA)
 * on thousands of STALE off-chain fork/orphan entries above the header tip,
 * whose pprev pointers are not linked down to the active tip. The original
 * predicate counted ANY such higher-work HAVE_DATA entry whose
 * block_index_get_ancestor(candidate, tip->nHeight) != tip as a competing
 * chain — but for a torn fork that walk returns NULL (chain.c: unlinked
 * pprev), and NULL != tip flipped the result to "not best work" forever,
 * permanently wedging tip promotion.
 *
 * find_most_work_chain (process_block_core.c:344-413), the authority on
 * selection, requires BLOCK_VALID_TREE (:368), tolerates unlinked pprev
 * (:398-406), and never reorgs below the tip (:430) — so it had already
 * selected our tip's path. The disagreement was purely this predicate's
 * looser filter. Align the filter:
 *   - require BLOCK_VALID_TREE (mirror :368) — stale entries that lost
 *     tree validity are not selectable and cannot be "better work";
 *   - a candidate only beats the tip when its ancestry RESOLVES to a real
 *     block at tip->nHeight that is NOT the tip (a genuine connectable
 *     sibling fork → we SHOULD reorg → return false). A NULL resolution
 *     means the candidate is not contiguous-data-linked down to tip height
 *     (torn/orphan fork): find_most_work_chain could never select it, so
 *     it must NOT veto promotion.
 *
 * Reorg safety: a genuinely better, fully-downloaded, fully-linked
 * competing chain still resolves block_index_get_ancestor(candidate,
 * tip->nHeight) to a real block != tip, so this still returns false and
 * the reorg path runs. Only the unresolvable (NULL) torn-fork case is
 * reclassified, and that case was never a valid reorg target. */
static bool process_block_tip_is_best_work(const struct main_state *ms,
                                           const struct block_index *tip)
{
    size_t iter = 0;
    struct block_index *candidate = NULL;

    if (!ms || !tip)
        return false;
    while (block_map_next(&ms->map_block_index, &iter, NULL, &candidate)) {
        if (!candidate || block_has_any_failure(candidate))
            continue;
        /* Match find_most_work_chain :368 — only header-tree-valid blocks
         * are selectable; stale invalidated forks cannot be "best work". */
        if (!block_index_is_valid(candidate, BLOCK_VALID_TREE))
            continue;
        if (!(candidate->nStatus & BLOCK_HAVE_DATA))
            continue;
        if (arith_uint256_compare(&candidate->nChainWork,
                                  &tip->nChainWork) <= 0)
            continue;
        struct block_index *anc =
            block_index_get_ancestor(candidate, tip->nHeight);
        /* Only a RESOLVED, non-tip ancestor proves a real connectable
         * competing chain (→ reorg). A NULL resolution is a torn/orphan
         * fork that find_most_work_chain cannot select — it must not veto
         * the tip and wedge promotion. */
        if (anc && anc != tip)
            return false;
    }
    return true;
}

#ifdef ZCL_TESTING
bool process_block_test_tip_is_best_work(const struct main_state *ms,
                                         const struct block_index *tip)
{
    return process_block_tip_is_best_work(ms, tip);
}
#endif

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
        (void)chain_set_active_tip(ms, new_tip, TIP_FROM_CONNECT,
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
    if (rc == CSR_REJECTED_DB_BUSY)
        mirror_consensus_record_blocker("db-writer-busy");
    else if (rc == CSR_REJECTED_PERSIST)
        mirror_consensus_record_blocker("csr-persist-failed");
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
            (void)chain_set_active_tip(ms, NULL, TIP_FROM_DISCONNECT,
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

/* accept_block_header()   moved to lib/validation/src/accept_block_header.c
 * accept_block()          DELETED with the legacy validation engine.
 * connect_tip()           DELETED with the legacy validation engine.
 * disconnect_tip()        DELETED with the legacy validation engine.
 * activate_best_chain()   DELETED with the legacy validation engine.
 * process_new_block()     DELETED with the legacy validation engine.
 *
 * The reducer (reducer_ingest_block / reducer_kick, app/services + app/jobs)
 * is the sole block-connect engine; every ingest call site routes through it
 * directly. The shared infra below (find_most_work_chain,
 * process_block_commit_tip, update_tip, process_block_propagate_failed_child,
 * process_block_test_hydrate_index_from_disk) survives — the reducer stages
 * and the keeper tests call it. */
