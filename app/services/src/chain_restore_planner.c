/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Chain restore planner — deterministic, side-effect-light planning for
 * chain tip restoration. */

#include "services/chain_restore_service.h"
#include "platform/time_compat.h"
#include "core/uint256.h"

#include <stdio.h>
#include <string.h>

struct chain_restore_boot_snapshot g_chain_restore_boot_snapshot;

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
        chain_restore_record_plan_result(out);
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
        chain_restore_record_plan_result(out);
        return;
    }

    /* Path B: hash NOT in block_map but we know UTXO height */
    if (in->utxo_max_height > 0) {
        out->next_state = CHAIN_RESTORE_ANCHOR_CREATED;
        out->should_create_anchor = true;
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
        chain_restore_record_plan_result(out);
        return;
    }

    /* Path C: no height info at all */
    out->next_state = CHAIN_RESTORE_FAILED;
    out->should_skip_activate = true;
    snprintf(out->reason, sizeof(out->reason),
             "coins_best_block set but height unknown — awaiting P2P");
    chain_restore_record_plan_result(out);
}

void chain_restore_record_plan_result(const struct chain_restore_plan *p)
{
    if (!p) return;
    g_chain_restore_boot_snapshot.has_data = true;
    g_chain_restore_boot_snapshot.boot_time =
        (int64_t)platform_time_wall_time_t();
    g_chain_restore_boot_snapshot.plan_recorded = true;
    g_chain_restore_boot_snapshot.plan_next_state = (int)p->next_state;
    g_chain_restore_boot_snapshot.plan_anchor_height = p->anchor_height;
    g_chain_restore_boot_snapshot.plan_should_skip_activate =
        p->should_skip_activate;
    size_t n = strnlen(p->reason, sizeof(p->reason));
    if (n >= sizeof(g_chain_restore_boot_snapshot.plan_reason))
        n = sizeof(g_chain_restore_boot_snapshot.plan_reason) - 1;
    memcpy(g_chain_restore_boot_snapshot.plan_reason, p->reason, n);
    g_chain_restore_boot_snapshot.plan_reason[n] = '\0';
}
