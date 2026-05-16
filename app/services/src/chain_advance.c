/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * chain_advance — atomic per-block chain advance.
 *
 * See services/chain_advance.h for the full contract.
 *
 * ── 9-step protocol ────────────────────────────────────────
 *
 * 1. Validate inputs            — early return on NULL/invalid
 * 2. coins_view_sqlite_begin    — BEGIN IMMEDIATE on coins.db
 * 3. node_db_begin              — BEGIN on node.db
 *    connect_block(...)         — apply UTXO deltas into local view
 *      check_crash_stage(AFTER_CONNECT_BLOCK)
 * 4. coins_view_cache_flush     — view → coins_tip cache (RAM)
 *      check_crash_stage(AFTER_COINS_VIEW_FLUSH)
 * 5. sapling_tree_persist_once  — sapling state row inside ndb's txn
 * 6. process_block_commit_tip   — in-memory active_chain mutation
 *      check_crash_stage(AFTER_UPDATE_TIP)
 * 7. flush_coins_if_needed(force=true)  — durable UTXOs to coins.db
 *    coins_view_sqlite_commit   — coins.db COMMIT  ←  ORDERING INVARIANT
 *      check_crash_stage(AFTER_COINS_DISK_FLUSH)
 * 8. block_tree_db_write_block_index_sync — durable LevelDB block_index
 *      check_crash_stage(AFTER_BLOCK_INDEX_WRITE)
 *    node_db_commit             — node.db COMMIT
 * 9. return CA_OK
 *
 * Crash window between (7) and (8): coins.db at N+1, node.db at N —
 * forward-recoverable by boot.c's utxo_recovery_service. The reverse
 * direction (node.db at N+1 but coins.db at N) is unreachable by
 * construction because we order the two COMMITs.
 *
 * On any failure inside steps 3-6, both transactions are rolled back
 * and the on-disk state matches pre-call exactly. */

#include "services/chain_advance.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "validation/process_block.h"
#include "validation/process_block_internals.h"
#include "validation/connect_block.h"
#include "validation/main_state.h"
#include "coins/coins_view.h"
#include "consensus/validation.h"
#include "primitives/block.h"
#include "chain/chainparams.h"
#include "services/oracle_policy.h"
#include "storage/coins_view_sqlite.h"
#include "storage/block_index_db.h"
#include "models/database.h"
#include "core/utiltime.h"
#include "util/trace.h"

const char *chain_advance_result_name(enum chain_advance_result r)
{
    static const char *names[] = {
        [CA_OK]                  = "ok",
        [CA_REJECTED_VALIDATION] = "rejected_validation",
        [CA_REJECTED_CSR]        = "rejected_csr",
        [CA_FAILED_DISK_WRITE]   = "failed_disk_write",
        [CA_FAILED_ROLLBACK]     = "failed_rollback",
        [CA_HALTED_BY_POLICY]    = "halted_by_policy",
    };
    if (r >= 0 && r < CA_NUM_RESULTS)
        return names[r];
    return "unknown";
}

/* Best-effort rollback of both handles. Returns CA_FAILED_ROLLBACK
 * when ROLLBACK itself fails (caller treats as fatal); otherwise
 * the supplied fallback result. */
static enum chain_advance_result
ca_rollback(struct coins_view_sqlite *cvs, bool coins_in_tx,
            struct node_db *ndb, bool ndb_in_tx,
            enum chain_advance_result fallback)
{
    bool ok = true;
    if (ndb_in_tx && ndb) {
        if (!node_db_rollback(ndb)) {
            fprintf(stderr,
                    "chain_advance: ROLLBACK on node.db failed — "
                    "transaction state indeterminate, treating as fatal\n");
            ok = false;
        }
    }
    if (coins_in_tx && cvs) {
        if (!coins_view_sqlite_rollback(cvs)) {
            fprintf(stderr,
                    "chain_advance: ROLLBACK on coins.db failed — "
                    "transaction state indeterminate, treating as fatal\n");
            ok = false;
        }
    }
    return ok ? fallback : CA_FAILED_ROLLBACK;
}

enum chain_advance_result
chain_advance(struct validation_state *state,
              struct main_state *ms,
              struct coins_view_cache *coins_tip,
              struct block_index *new_tip,
              struct block *pblock,
              const struct chain_params *params,
              const char *datadir,
              const char *reason)
{
    /* ── Step 1: validate inputs ───────────────────────────── */
    if (!state || !ms || !coins_tip || !new_tip || !pblock ||
        !params || !datadir || !reason)
        return CA_REJECTED_VALIDATION;

    /* T2.1: refuse to extend the tip when oracle_policy has flagged
     * a fork or evidence-prefix violation. No state mutation here — we
     * return before opening any transaction. */
    if (!oracle_policy_chain_extension_allowed()) {
        fprintf(stderr,
                "chain_advance: REFUSED at h=%d (oracle_policy state "
                "= %s; clear via MCP after investigation)\n",
                new_tip->nHeight,
                oracle_policy_get_state() == OP_PANIC
                    ? "PANIC" : "HALTED");
        return CA_HALTED_BY_POLICY;
    }

    struct trace_span *ca_span = trace_start("chain.advance");
    trace_attr_int(ca_span, "height", new_tip->nHeight);
    trace_attr_str(ca_span, "reason", reason);

    const int live_height = new_tip->nHeight;
    const int64_t t0_us = GetTimeMicros();
    int64_t stage_us = t0_us;

    struct coins_view_sqlite *cvs = process_block_get_coins_sqlite();
    struct block_tree_db    *btdb = process_block_get_block_tree();
    struct node_db          *ndb  = process_block_get_node_db();

    bool coins_in_tx = false;
    bool ndb_in_tx   = false;

    /* ── Step 2: BEGIN IMMEDIATE on coins.db ──────────────── */
    if (cvs) {
        if (!coins_view_sqlite_begin(cvs)) {
            fprintf(stderr,
                "chain_advance: coins_view_sqlite_begin failed h=%d\n",
                live_height);
            trace_set_status(ca_span, TRACE_STATUS_ERROR);
            trace_end(ca_span);
            return CA_FAILED_DISK_WRITE;
        }
        coins_in_tx = true;
    }

    /* ── Step 3: BEGIN on node.db + connect_block ─────────── */
    if (ndb && ndb->open) {
        if (!node_db_begin(ndb)) {
            fprintf(stderr,
                "chain_advance: node_db_begin failed h=%d\n",
                live_height);
            trace_set_status(ca_span, TRACE_STATUS_ERROR);
            trace_end(ca_span);
            return ca_rollback(cvs, coins_in_tx, NULL, false,
                               CA_FAILED_DISK_WRITE);
        }
        ndb_in_tx = true;
    }

    struct coins_view_cache view;
    struct coins_view backing;
    coins_view_cache_as_view(&backing, coins_tip);
    coins_view_cache_init(&view, &backing);

    /* sapling tree side effect: connect_block updates it via this
     * static pointer, and clears after. Match connect_tip's exact
     * pattern. */
    connect_block_set_sapling_tree(&ms->sapling_tree);
    stage_us = GetTimeMicros();
    bool rv = connect_block(pblock, state, new_tip, &view, params, false);
    connect_block_set_sapling_tree(NULL);
    process_block_log_live_stage_ext(live_height, "connect_block",
                                     (long long)(GetTimeMicros() - stage_us));

    if (!rv) {
        coins_view_cache_free(&view);
        memset(&view, 0, sizeof(view));
        trace_set_status(ca_span, TRACE_STATUS_ERROR);
        trace_attr_str(ca_span, "error", "connect_block");
        trace_end(ca_span);
        return ca_rollback(cvs, coins_in_tx, ndb, ndb_in_tx,
                           CA_REJECTED_VALIDATION);
    }
    process_block_check_crash_stage_ext(PBCS_AFTER_CONNECT_BLOCK);

    /* ── Step 4: coins_view_cache_flush (view → coins_tip RAM) ─ */
    stage_us = GetTimeMicros();
    if (!coins_view_cache_flush(&view)) {
        fprintf(stderr, "chain_advance: coins_view_cache_flush failed h=%d\n",
                live_height);
        coins_view_cache_free(&view);
        memset(&view, 0, sizeof(view));
        trace_set_status(ca_span, TRACE_STATUS_ERROR);
        trace_attr_str(ca_span, "error", "coins_view_cache_flush");
        trace_end(ca_span);
        (void)validation_state_error(state, "coins-flush-failed");
        return ca_rollback(cvs, coins_in_tx, ndb, ndb_in_tx,
                           CA_FAILED_DISK_WRITE);
    }
    process_block_log_live_stage_ext(live_height, "coins_view_flush",
                                     (long long)(GetTimeMicros() - stage_us));
    coins_view_cache_free(&view);
    memset(&view, 0, sizeof(view));
    process_block_check_crash_stage_ext(PBCS_AFTER_COINS_VIEW_FLUSH);

    /* ── Step 5: sapling tree row inside node.db txn ──────── */
    /* Best-effort: failure is logged via persist_once but never
     * aborts the advance (matches connect_tip's prior behavior). */
    (void)process_block_persist_sapling_tree();

    /* ── Step 6: csr_commit_tip — in-memory active_chain ──── */
    stage_us = GetTimeMicros();
    if (!process_block_commit_tip_ext(ms, coins_tip, new_tip, reason, true)) {
        fprintf(stderr,
            "chain_advance: csr rejected commit h=%d (reason=%s)\n",
            live_height, reason);
        (void)validation_state_error(state, "csr-tip-commit-rejected");
        trace_set_status(ca_span, TRACE_STATUS_ERROR);
        trace_attr_str(ca_span, "error", "csr_rejected");
        trace_end(ca_span);
        return ca_rollback(cvs, coins_in_tx, ndb, ndb_in_tx,
                           CA_REJECTED_CSR);
    }
    process_block_log_live_stage_ext(live_height, "csr_commit_tip",
                                     (long long)(GetTimeMicros() - stage_us));
    new_tip->nStatus = (new_tip->nStatus & ~BLOCK_VALID_MASK) |
                       BLOCK_VALID_SCRIPTS;
    process_block_check_crash_stage_ext(PBCS_AFTER_UPDATE_TIP);

    /* ── Step 7: durable coins.db flush + COMMIT ─────────────
     *
     * THE ordering invariant: coins.db must be durable on disk
     * BEFORE the LevelDB block_index entry. A kill -9 between the
     * coins COMMIT and the block_index write leaves coins.db at
     * N+1, node.db at N — boot's utxo_recovery_service forward-
     * rolls deterministically. The reverse direction (block_index
     * ahead of coins) requires re-running connect_block to rebuild
     * UTXO state, which is the 250k-block self_heal scan we are
     * trying to retire.
     *
     * Force=true ensures the flush actually lands; otherwise the
     * lazy policy could leave dirty entries in RAM and the coins
     * COMMIT below would close an empty transaction. */
    stage_us = GetTimeMicros();
    if (!process_block_flush_coins(coins_tip, true)) {
        fprintf(stderr,
            "chain_advance: forced coins flush failed h=%d — "
            "aborting tip advance\n", live_height);
        (void)validation_state_error(state, "coins-disk-flush-failed");
        trace_set_status(ca_span, TRACE_STATUS_ERROR);
        trace_attr_str(ca_span, "error", "coins_flush");
        trace_end(ca_span);
        return ca_rollback(cvs, coins_in_tx, ndb, ndb_in_tx,
                           CA_FAILED_DISK_WRITE);
    }
    if (coins_in_tx && cvs) {
        if (!coins_view_sqlite_commit(cvs)) {
            fprintf(stderr,
                "chain_advance: coins_view_sqlite_commit failed h=%d\n",
                live_height);
            trace_set_status(ca_span, TRACE_STATUS_ERROR);
            trace_attr_str(ca_span, "error", "coins_commit");
            trace_end(ca_span);
            /* coins.db state indeterminate; rollback ndb. */
            return ca_rollback(NULL, false, ndb, ndb_in_tx,
                               CA_FAILED_DISK_WRITE);
        }
        coins_in_tx = false;
    }
    process_block_log_live_stage_ext(live_height, "coins_disk_flush",
                                     (long long)(GetTimeMicros() - stage_us));
    process_block_check_crash_stage_ext(PBCS_AFTER_COINS_DISK_FLUSH);

    /* ── Step 8: LevelDB block_index sync + node.db COMMIT ── */
    if (btdb) {
        struct disk_block_index dbi;
        disk_block_index_init(&dbi);
        if (new_tip->pprev && new_tip->pprev->phashBlock)
            dbi.hashPrev = *new_tip->pprev->phashBlock;
        dbi.nHeight            = new_tip->nHeight;
        dbi.nStatus            = new_tip->nStatus;
        dbi.nTx                = new_tip->nTx;
        dbi.nFile              = new_tip->nFile;
        dbi.nDataPos           = new_tip->nDataPos;
        dbi.nUndoPos           = new_tip->nUndoPos;
        dbi.nCachedBranchId    = new_tip->nCachedBranchId;
        dbi.nVersion           = new_tip->nVersion;
        dbi.hashMerkleRoot     = new_tip->hashMerkleRoot;
        dbi.hashFinalSaplingRoot = new_tip->hashFinalSaplingRoot;
        dbi.nTime              = new_tip->nTime;
        dbi.nBits              = new_tip->nBits;
        dbi.nNonce             = new_tip->nNonce;
        if (new_tip->nSolution && new_tip->nSolutionSize > 0)
            memcpy(dbi.nSolution, new_tip->nSolution, new_tip->nSolutionSize);
        dbi.nSolutionSize = new_tip->nSolutionSize;

        stage_us = GetTimeMicros();
        if (!block_tree_db_write_block_index_sync(btdb, &dbi)) {
            fprintf(stderr,
                "chain_advance: block_tree_db_write_block_index_sync "
                "failed h=%d\n", live_height);
            (void)validation_state_error(state,
                "block-index-sync-failed");
            trace_set_status(ca_span, TRACE_STATUS_ERROR);
            trace_attr_str(ca_span, "error", "block_index_sync");
            trace_end(ca_span);
            /* coins.db already committed at N+1; node.db rolled back
             * to N — boot recovery forward-rolls. */
            return ca_rollback(NULL, false, ndb, ndb_in_tx,
                               CA_FAILED_DISK_WRITE);
        }
        process_block_log_live_stage_ext(live_height, "block_index_sync",
                                         (long long)(GetTimeMicros() - stage_us));
        process_block_check_crash_stage_ext(PBCS_AFTER_BLOCK_INDEX_WRITE);

        /* Free nSolution after persisting — saves ~1.3 KB per
         * block_index entry; serving paths fall back to disk reads. */
        free(new_tip->nSolution);
        new_tip->nSolution = NULL;
        new_tip->nSolutionSize = 0;
    }

    if (ndb_in_tx && ndb) {
        if (!node_db_commit(ndb)) {
            fprintf(stderr,
                "chain_advance: node_db_commit failed h=%d — "
                "coins.db already at N+1; boot forward-roll required\n",
                live_height);
            trace_set_status(ca_span, TRACE_STATUS_ERROR);
            trace_attr_str(ca_span, "error", "node_db_commit");
            trace_end(ca_span);
            return CA_FAILED_DISK_WRITE;
        }
        ndb_in_tx = false;
    }

    /* ── Step 9: success ──────────────────────────────────── */
    trace_attr_int(ca_span, "elapsed_us",
                   (int)(GetTimeMicros() - t0_us));
    trace_end(ca_span);
    return CA_OK;
}
