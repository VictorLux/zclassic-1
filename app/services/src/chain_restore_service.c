/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Chain Restore Service — deterministic chain tip restoration.
 * See chain_restore_service.h for architecture overview. */

#include "services/chain_restore_service.h"
#include "services/chain_state_repository.h"
#include "models/db_txn.h"
#include "validation/main_state.h"
#include "validation/chainstate.h"
#include "chain/chain.h"
#include "services/snapshot_sync_service.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/log_macros.h"
#include "util/safe_alloc.h"

/* ── Planning (pure function) ──────────────────────────────────── */

void chain_restore_plan(struct chain_restore_plan *out,
                        const struct chain_restore_input *in)
{
    memset(out, 0, sizeof(*out));

    /* Null hash → nothing to restore */
    if (uint256_is_null(&in->coins_best_hash)) {
        out->next_state = CHAIN_RESTORE_FAILED;
        out->should_skip_activate = true;
        snprintf(out->reason, sizeof(out->reason),
                 "coins_best_block is null — no UTXO state");
        return;
    }

    /* Path A: hash found in block_map with valid height */
    if (in->hash_found_in_map && in->found_height > 0) {
        out->next_state = CHAIN_RESTORE_FOUND_IN_INDEX;
        out->should_set_chain_tip = true;
        out->should_set_best_header = true;
        out->should_skip_activate = true;
        out->anchor_height = in->found_height;
        out->anchor_hash = in->coins_best_hash;
        snprintf(out->reason, sizeof(out->reason),
                 "found in block index at h=%d", in->found_height);
        return;
    }

    /* Path B: hash NOT in block_map but we know UTXO height */
    if (in->utxo_max_height > 0) {
        out->next_state = CHAIN_RESTORE_ANCHOR_CREATED;
        out->should_create_anchor = true;
        out->should_set_chain_tip = true;
        out->should_set_best_header = true;
        out->should_set_snapshot_anchor = true;
        out->should_skip_activate = true;
        out->anchor_height = in->utxo_max_height;
        out->anchor_hash = in->coins_best_hash;
        snprintf(out->reason, sizeof(out->reason),
                 "anchor at h=%d (hash not in index, %s)",
                 in->utxo_max_height,
                 in->source == CHAIN_RESTORE_SRC_LDB_IMPORT ? "LDB import"
                 : in->source == CHAIN_RESTORE_SRC_SNAPSHOT ? "snapshot"
                 : "boot");
        return;
    }

    /* Path C: no height info at all */
    out->next_state = CHAIN_RESTORE_FAILED;
    out->should_skip_activate = true;
    snprintf(out->reason, sizeof(out->reason),
             "coins_best_block set but height unknown — awaiting P2P");
}

/* ── Anchor creation (shared implementation) ───────────────────── */

struct block_index *chain_restore_create_anchor(
    struct main_state *ms,
    const struct uint256 *hash,
    int height)
{
    if (!ms || !hash || height <= 0)
        LOG_NULL("chain_restore", "create_anchor called with null ms/hash or height=%d", height);

    struct block_index *anchor = zcl_calloc(1, sizeof(struct block_index), "chain_restore anchor");
    if (!anchor)
        LOG_NULL("chain_restore", "calloc failed for anchor block_index at h=%d", height);

    block_index_init(anchor);
    anchor->nHeight = height;
    anchor->nStatus = BLOCK_VALID_TREE | BLOCK_HAVE_DATA;
    anchor->nChainTx = 1;
    anchor->nTx = 1;

    /* nChainWork = max(all existing) + large margin.
     * Ensures find_most_work_chain always prefers chains built on
     * this anchor over old chains that don't reflect the UTXO state. */
    {
        struct arith_uint256 max_work;
        arith_uint256_set_u64(&max_work, 0);
        size_t iter = 0;
        struct block_index *entry;
        while (block_map_next(&ms->map_block_index, &iter, NULL, &entry)) {
            if (!entry) continue;
            if (arith_uint256_compare(&entry->nChainWork, &max_work) > 0)
                max_work = entry->nChainWork;
        }
        struct arith_uint256 margin;
        arith_uint256_set_u64(&margin, 4096ULL * (uint64_t)height);
        arith_uint256_add(&anchor->nChainWork, &max_work, &margin);
    }

    if (!block_map_insert(&ms->map_block_index, hash, anchor)) {
        free(anchor);
        return NULL;
    }

    anchor->phashBlock = block_map_find_hash(&ms->map_block_index, hash);
    if (!anchor->phashBlock) {
        /* Insert succeeded but hash lookup failed — shouldn't happen */
        fprintf(stderr, "chain_restore: anchor inserted but hash not found\n");
    }

    return anchor;
}

/* ── Execution ─────────────────────────────────────────────────── */

struct block_index *chain_restore_execute(
    const struct chain_restore_plan *plan,
    struct main_state *ms)
{
    if (!plan || !ms)
        LOG_NULL("chain_restore", "execute called with null plan or main_state");

    if (plan->next_state == CHAIN_RESTORE_FAILED)
        return NULL;

    struct block_index *target = NULL;

    if (plan->should_create_anchor) {
        target = chain_restore_create_anchor(
            ms, &plan->anchor_hash, plan->anchor_height);
        if (!target) {
            fprintf(stderr, "chain_restore: anchor creation failed\n");
            return NULL;
        }
        printf("Chain restore: anchor at h=%d\n", plan->anchor_height);
    } else if (plan->next_state == CHAIN_RESTORE_FOUND_IN_INDEX) {
        target = block_map_find(&ms->map_block_index, &plan->anchor_hash);
        if (!target) {
            fprintf(stderr, "chain_restore: hash in plan but not in map\n");
            return NULL;
        }
    }

    if (!target)
        return NULL;

    /* Route the tip/header mutations through the chain_state_repository
     * so block_map, active_chain, coins_tip and pindex_best_header
     * move atomically. The chain restore path is exactly the scenario
     * that caused 2026-04-10: boot detected a "best hash" and shoved
     * it into active_chain without cross-checking SQLite state. */
    if (plan->should_set_chain_tip && target->phashBlock) {
        struct chain_state_commit commit = {
            .new_tip             = target,
            .new_coins_best      = *target->phashBlock,
            .expected_utxo_count = 0,
            .update_header_tip   = plan->should_set_best_header,
            /* Chain restore is explicitly a "snap to where UTXOs
             * actually live" operation, which can legitimately look
             * like a backward move from csr's perspective. Bypass the
             * orphan-rows guard; Phase 2 recovery_policy will gate
             * this class of move via the operator-visible policy. */
            .allow_rollback      = true,
            .wallet_scan_height  = -1,
            .reason              = "chain_restore.execute",
        };

        /* Wrap the CSR commit in a scoped db transaction when the
         * singleton is wired to a real node_db. csr_commit_tip itself
         * only mutates in-memory state today, but csr_validate_locked
         * issues SQLite reads and any future write path here would be
         * silently leak-prone without the scope. The scope also gives
         * operators a BEGIN/COMMIT event pair bracketing the tip move,
         * which is the single highest-signal event from the 2026-04-10
         * incident. Unit-test paths that stub csr with a NULL ndb fall
         * through to the legacy raw-setter branch below. */
        struct chain_state_repository *csr = csr_instance();
        struct node_db *cr_ndb = (csr && csr->initialized) ? csr->ndb : NULL;
        enum csr_result rc;

        if (cr_ndb && cr_ndb->open) {
            DB_TXN_SCOPE(txn, cr_ndb, "chain_restore.execute");
            if (!txn) {
                fprintf(stderr,
                    "chain_restore: failed to open db_txn scope\n");
                return NULL;
            }
            rc = csr_commit_tip(csr, &commit);
            if (rc != CSR_OK) {
                /* Scope auto-rollback fires on return. */
                fprintf(stderr,
                    "chain_restore: csr rejected tip commit (%s) h=%d\n",
                    csr_result_name(rc), target->nHeight);
                return NULL;
            }
            if (!db_txn_commit(txn))
                return NULL;
        } else {
            rc = csr_commit_tip(csr, &commit);
            if (rc != CSR_OK) {
                if (rc == CSR_REJECTED_NOT_INITIALIZED) {
                    /* Unit-test path: singleton was never wired. Keep
                     * the legacy raw-setter behaviour so the existing
                     * test_chain_restore_service suite continues to
                     * exercise the end-to-end flow. */
                    active_chain_set_tip(&ms->chain_active, target);
                    if (plan->should_set_best_header)
                        ms->pindex_best_header = target;
                } else {
                    fprintf(stderr,
                        "chain_restore: csr rejected tip commit (%s) h=%d\n",
                        csr_result_name(rc), target->nHeight);
                    return NULL;
                }
            }
        }
    } else if (plan->should_set_best_header) {
        /* Extremely rare: plan asked for header-only update with no
         * chain tip change. Preserve legacy behaviour. */
        ms->pindex_best_header = target;
    }

    if (plan->should_set_snapshot_anchor)
        snapsync_set_anchor(target);

    return target;
}

/* ── Validation ────────────────────────────────────────────────── */

void chain_restore_validate(struct chain_restore_validation *out,
                            const struct main_state *ms,
                            const struct uint256 *expected_hash,
                            int expected_height)
{
    memset(out, 0, sizeof(*out));

    out->coins_hash_valid = expected_hash && !uint256_is_null(expected_hash);

    if (expected_hash) {
        struct block_index *found = block_map_find(
            &ms->map_block_index, expected_hash);
        out->anchor_in_map = (found != NULL);
    }

    struct block_index *tip = active_chain_tip(&ms->chain_active);
    out->chain_tip_set = (tip != NULL);

    if (tip && expected_height > 0)
        out->tip_matches_expected = (tip->nHeight == expected_height);

    out->all_ok = out->coins_hash_valid
               && out->anchor_in_map
               && out->chain_tip_set
               && out->tip_matches_expected;
}

/* ── Post-restore integrity check (P14.11 + P14.12) ────────────── */

void chain_integrity_check_post_restore(struct chain_integrity_result *out,
                                        const struct main_state *ms)
{
    memset(out, 0, sizeof(*out));
    out->first_nbits_zero_height = -1;
    out->first_hole_height = -1;

    if (!ms) {
        out->ok = false;
        return;
    }

    /* P14.11: every pindex above genesis must have nBits != 0. */
    size_t iter = 0;
    struct block_index *pi;
    while (block_map_next(&ms->map_block_index, &iter, NULL, &pi)) {
        if (!pi || pi->nHeight <= 0)
            continue;
        if (pi->nBits == 0) {
            out->zero_nbits_count++;
            if (out->first_nbits_zero_height < 0 ||
                pi->nHeight < out->first_nbits_zero_height)
                out->first_nbits_zero_height = pi->nHeight;
        }
    }

    /* P14.12: chain_active.chain[h] non-NULL for h in [0, tip]. */
    out->tip_height = active_chain_height(&ms->chain_active);
    for (int h = 0; h <= out->tip_height; h++) {
        if (active_chain_at(&ms->chain_active, h) == NULL) {
            out->active_chain_holes++;
            if (out->first_hole_height < 0 || h < out->first_hole_height)
                out->first_hole_height = h;
        }
    }

    out->ok = (out->zero_nbits_count == 0 && out->active_chain_holes == 0);
}

/* ── Boot activation decision ──────────────────────────────────── */

void boot_should_activate_chain(struct activation_decision *out,
                                int chain_tip_height,
                                int64_t utxo_count,
                                size_t block_index_size,
                                bool legacy_import,
                                bool anchor_was_created)
{
    memset(out, 0, sizeof(*out));
    out->chain_height = chain_tip_height;
    out->utxo_count = utxo_count;
    out->block_index_size = block_index_size;

    if (legacy_import) {
        out->should_activate = false;
        out->reason = ACTIVATE_SKIP_LEGACY_IMPORT;
        return;
    }

    if (anchor_was_created) {
        out->should_activate = false;
        out->reason = ACTIVATE_SKIP_ANCHOR_CREATED;
        return;
    }

    /* No UTXOs + many headers = awaiting P2P snapshot.
     * Connecting blocks from genesis would mark valid blocks FAILED. */
    if (utxo_count < 100000 && chain_tip_height == 0
        && block_index_size > 1000) {
        out->should_activate = false;
        out->reason = ACTIVATE_SKIP_NO_UTXOS_AWAITING;
        return;
    }

    out->should_activate = true;
    out->reason = ACTIVATE_OK;
}
