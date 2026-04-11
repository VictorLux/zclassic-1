/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Chain Restore Service — deterministic chain tip restoration.
 * See chain_restore_service.h for architecture overview. */

#include "services/chain_restore_service.h"
#include "validation/main_state.h"
#include "validation/chainstate.h"
#include "chain/chain.h"
#include "services/snapshot_sync_service.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
        return NULL;

    struct block_index *anchor = calloc(1, sizeof(struct block_index));
    if (!anchor)
        return NULL;

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
        return NULL;

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

    if (plan->should_set_chain_tip)
        active_chain_set_tip(&ms->chain_active, target);

    if (plan->should_set_best_header)
        ms->pindex_best_header = target;

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
