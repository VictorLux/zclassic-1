/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * chain_advance — atomic per-block chain advance.
 *
 * See services/chain_advance.h for the contract this aspires to.
 *
 * ── Current state: delegating wrapper ──────────────────────
 *
 * This first commit of the chain_advance implementation provides
 * the public entry point but DOES NOT YET deliver the full atomic-
 * ity contract from the header. The body currently delegates to
 * connect_tip in lib/validation/src/process_block.c, which has the
 * known ordering gap described in the header:
 *
 *   1. coins_view_cache_flush(&view)   — view → coins_tip cache
 *   2. csr_commit_tip(...)              — in-memory chain tip
 *   3. block_tree_db_write_block_index — block_index → LevelDB
 *   4. flush_coins_if_needed(coins_tip) — coins_tip → coins.db
 *      (only every g_flush_policy.block_interval or .max_entries
 *      or .interval_secs, default 0 / 500000 / 3600s — so usually
 *      lazy, not per block)
 *
 * If kill -9 fires between step 3 and the eventual disk flush of
 * step 4, block_index says "block N+1 is connected" while coins.db
 * still shows hash_block at N. Boot recovery (utxo_recovery_service
 * forward-roll) currently rewinds the tip 1–6 blocks per kill cycle
 * to converge — see feedback_kill_restart_recovery_cost.md.
 *
 * The follow-up commits will:
 *   - Reorder steps 3 and 4 (force coins.db disk flush BEFORE the
 *     LevelDB block_index write completes its WriteBatch on a
 *     `sync=true` WriteOptions)
 *   - Add a process_block_test_crash_at(stage) debug hook so the
 *     atomicity test can inject crashes at every intermediate point
 *   - Land lib/test/src/test_chain_advance_atomicity.c that
 *     forks/SIGKILLs at each crash point and asserts the rebooted
 *     node's getblockcount equals the pre-kill value
 *   - Once atomicity is enforced by construction, delete the
 *     reactive guards (placeholder-block refusal at
 *     process_block.c:2073, chain_restore off-chain demotion, the
 *     150,000-block self-heal scan default)
 *
 * Until then, this wrapper preserves today's semantics with the
 * richer return enum — callers that adopt chain_advance now will
 * not see new behaviour, but they will get a stable surface that
 * future commits can swap the body of without touching them.
 */

#include "services/chain_advance.h"
#include "validation/process_block.h"

const char *chain_advance_result_name(enum chain_advance_result r)
{
    static const char *names[] = {
        [CA_OK]                  = "ok",
        [CA_REJECTED_VALIDATION] = "rejected_validation",
        [CA_REJECTED_CSR]        = "rejected_csr",
        [CA_FAILED_DISK_WRITE]   = "failed_disk_write",
        [CA_FAILED_ROLLBACK]     = "failed_rollback",
    };
    if (r >= 0 && r < CA_NUM_RESULTS)
        return names[r];
    return "unknown";
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
    /* `reason` is captured in CSR + EV_SYNC_STATE_CHANGE events by
     * the existing process_block_commit_tip caller; it isn't
     * separately threaded through this wrapper yet. */
    (void)reason;

    if (!ms || !new_tip || !params || !datadir)
        return CA_REJECTED_VALIDATION;

    bool ok = connect_tip(state, ms, coins_tip, new_tip, pblock,
                          params, datadir);
    if (ok) return CA_OK;

    /* connect_tip's bool return collapses several failure modes into
     * one. The state->reject_reason (and the structured logs the
     * caller already emitted via fprintf(stderr) / EV_BLOCK_REJECTED)
     * carry the detail. Future work disambiguates these via the new
     * crash-injection hook + a richer connect_tip return. For now
     * we report CA_REJECTED_VALIDATION as the catch-all so callers
     * can treat any non-OK result as "do not advance the chain
     * tip". */
    return CA_REJECTED_VALIDATION;
}
