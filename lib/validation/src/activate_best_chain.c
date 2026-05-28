/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php.
 *
 * activate_best_chain — top-level chain selection / advance loop.
 *
 * Calls connect_tip/disconnect_tip in the necessary order to bring
 * the active chain to the best valid candidate. Owns the reorg recovery
 * path (recover_from_disconnect_failure) since that helper is only used
 * by activate_best_chain and is conceptually part of its loop.
 *
 * Extracted from process_block_core.c (WS-6 phase 1, file-level split).
 * Pure code motion; function bodies are byte-identical to their prior
 * sites. */

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

#include "validation/process_block.h"
#include "validation/main_logic.h"
#include "validation/check_block.h"
#include "validation/connect_block.h"
#include "validation/mirror_consensus.h"
#include "validation/validationinterface.h"
#include "validation/checkpoint.h"
#include "validation/main_constants.h"
#include "validation/process_block_internals.h"
#include "coins/utxo_commitment.h"
#include "coins/undo.h"
#include "net/download.h"
#include "chain/checkpoints.h"
#include "chain/pow.h"
#include "chain/mmr.h"
#include "chain/mmb.h"
#include "consensus/upgrades.h"
#include "core/core_io.h"
#include "core/serialize.h"
#include "core/utiltime.h"
#include "rpc/legacy_rpc_client.h"
#include "storage/disk_block_io.h"
#include "storage/txdb.h"
#include "storage/block_index_db.h"
#include "storage/coins_view_sqlite.h"
#include "wallet/wallet.h"
#include "validation/txmempool.h"
#include "event/event.h"
#include "models/database.h"
#include "config/runtime.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"
#include "util/trace.h"
#include "services/snapshot_sync_service.h"
#include "services/chain_activation_controller.h"
#include "services/chain_evidence_controller.h"
#include "services/chain_state_repository.h"
#include "services/gap_fill_service.h"
#include "services/chain_tip.h"

#include "process_block_internal.h"

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
