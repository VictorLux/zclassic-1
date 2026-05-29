/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php.
 *
 * connect_tip — connect one block to the active chain.
 *
 * The single most-walked path in the consensus core (Equihash PoW
 * verify, every signature/proof, every UTXO mutation, Sapling
 * checkpoints, BIP30 invariants, self-heal hot-loop, wallet sync,
 * cross-validator mirror). Extracted from process_block_core.c
 * (WS-6 phase 1, file-level split). Pure code motion; function body
 * byte-identical to its prior site.
 *
 * Phase 2 will decompose the body into named phase helpers
 * (prepare_inputs / verify_signatures / apply_undo / commit). */

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
#include "validation/validationinterface.h"
#include "validation/checkpoint.h"
#include "validation/main_constants.h"
#include "validation/process_block_internals.h"
#include "controllers/blockchain_controller.h"
#include "controllers/sync_controller.h"
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
#include "storage/coins_view_stage_backing.h"
#include "storage/utxo_projection.h"
#include "wallet/wallet.h"
#include "validation/txmempool.h"
#include "event/event.h"
#include "models/database.h"
#include "models/tx_index.h"
#include "config/runtime.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"
#include "util/trace.h"
#include "services/snapshot_sync_service.h"
#include "services/block_source_policy.h"
#include "services/chain_activation_controller.h"
#include "services/chain_evidence_controller.h"
#include "services/chain_state_repository.h"
#include "services/gap_fill_service.h"
#include "services/chain_tip.h"

#include "process_block_internal.h"

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
        struct coins_view legacy_backing;
        struct coins_view_stage_backing stage_backing;
        int recovery_attempts = 0;
        bool missing_utxo_unrecovered = false;

	retry_connect:
        /* B4-wiring: authority-gated backing selection. Under the default
         * LEGACY author this is byte-identical to wrapping coins_tip; under
         * STAGE (B7 flip) connect_block's input lookups resolve through the
         * UTXO projection (the authoritative set) while writes/best-block
         * stay on coins_tip. The RAM read-cache (`view`) is unchanged and
         * sits in front of whichever backing is chosen. Dormant until B7. */
        coins_view_cache_as_view(&legacy_backing, coins_tip);
        coins_view_select_connect_backing(&backing, &stage_backing,
                                          &legacy_backing,
                                          utxo_projection_get_global());
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
     *   LevelDB block_index is fsynced BEFORE coins.db UTXOs are
     *   committed for at-tip operation.
     *
     * A kill -9 between these writes may leave block_index at N+1
     * while coins.db is still at N. That direction is recoverable by
     * reconnecting/replaying the block. The reverse direction
     * (coins.db at N+1 while block_index is at N) is not safe: BIP30
     * sees block N+1's own coinbase already present and rejects the
     * block forever. Live 2026-05-25 reproduced this shape at
     * h=3124225.
     *
     * During IBD, bulk sync keeps the lazy flush policy; at-tip ops
     * force a per-block coins flush after the block_index write so the
     * durable UTXO set never gets ahead of the durable chain tip. */

    /* Timing only (no behavior change): the post-update_tip durable
     * write path (LevelDB block_index write+fsync, forced at-tip coins.db
     * commit, tx_index, wallet sync) was previously outside any stage
     * marker. Per docs/work/sync-perf-profile-2026-05-29.md this splits
     * the inferred fsync-vs-rest cost into measured sub-stages, reusing
     * the existing connect_tip: h=… stage=… elapsed_ms=… per-block idiom
     * (live-height-gated, same density as the existing 4 stages). */
    stage_start_us = GetTimeMicros();

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
         * durable before the forced at-tip coins.db commit below.
         *
         * Exception: during fast-sync body-pull / direct-import the
         * caller has explicitly opted into batched durability. Those
         * paths run under IBD/lazy coins flushing rather than the live
         * at-tip per-block durability contract. */
        if (atomic_load_explicit(&g_body_pull_active,
                                  memory_order_relaxed)) {
            block_tree_db_write_block_index(g_active_block_tree, &dbi);
        } else {
            block_tree_db_write_block_index_sync(g_active_block_tree, &dbi);
        }

        /* Free nSolution after persisting to disk — saves 1344B per block
         * (4GB total for 3M entries). Serving code in msg_headers.c and
         * msg_blocks.c falls back to reading from disk when NULL. */
        free(pindex_new->nSolution);
        pindex_new->nSolution = NULL;
        pindex_new->nSolutionSize = 0;
    }
    process_block_check_crash_stage(PBCS_AFTER_BLOCK_INDEX_WRITE);
    process_block_log_live_stage(live_height, "index_write",
                                 GetTimeMicros() - stage_start_us);

    stage_start_us = GetTimeMicros();
    if (!is_initial_block_download(ms)) {
        if (!flush_coins_if_needed(coins_tip, true)) {
            fprintf(stderr, // obs-ok:pre-existing-diagnostic
                    "connect_tip: coins flush failed after durable "
                    "block_index at height %d — halting block "
                    "connection to preserve recoverable ordering\n",
                    pindex_new->nHeight);
            if (pblock == &local_block)
                block_free(&local_block);
            trace_set_status(ct_span, TRACE_STATUS_ERROR);
            trace_end(ct_span);
            return validation_state_error(state,
                                          "coins-flush-after-index-failed");
        }
    }
    process_block_check_crash_stage(PBCS_AFTER_COINS_DISK_FLUSH);
    process_block_log_live_stage(live_height, "coins_commit",
                                 GetTimeMicros() - stage_start_us);

    stage_start_us = GetTimeMicros();
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
    process_block_log_live_stage(live_height, "tx_index",
                                 GetTimeMicros() - stage_start_us);

    stage_start_us = GetTimeMicros();
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
    process_block_log_live_stage(live_height, "wallet_sync",
                                 GetTimeMicros() - stage_start_us);

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
            block_source_policy_note_projection_deferred(
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
