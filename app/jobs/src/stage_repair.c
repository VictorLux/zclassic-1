/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * stage_repair — public entry/coordinator for the reducer-stage repair
 * helpers used by Conditions. The focused concerns live in sibling TUs:
 *   - stage_repair_header_solution.c — header-solution backfill (save/load),
 *   - stage_repair_body_fetch.c      — body-fetch candidacy detection,
 *   - stage_repair_rewind.c          — the destructive poison rewind.
 * This TU owns the boot-time tip-finalize clamp (the SAFE, non-destructive
 * reconcile that floors the tip_finalize cursor to coins_best+1 without
 * deleting any log row). It reuses the shared progress.kv accessors from
 * jobs/stage_repair_internal.h. */

#include "jobs/stage_repair.h"
#include "jobs/stage_repair_internal.h"

#include "storage/progress_store.h"
#include "util/log_macros.h"
#include "util/stage.h"

#include <sqlite3.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

bool stage_reconcile_clamp_tip_finalize_to_floor(
    sqlite3 *db, int coins_best, struct stage_reconcile_result *out)
{
    if (out)
        memset(out, 0, sizeof(*out));
    if (!db)
        return false;

    /* No durable applied anchor → nothing to floor on; leave the chain alone. */
    if (coins_best < 0)
        return true;

    int floor = coins_best + 1;
    if (out)
        out->floor = floor;

    if (!stage_table_ensure(db))
        return false;

    progress_store_tx_lock();

    int cur = -1;
    if (!stage_repair_cursor_at_unlocked(db, "tip_finalize", &cur)) {
        progress_store_tx_unlock();
        return false;
    }

    /* Only act on the wedge: tip_finalize cursor strictly AHEAD of the applied
     * tip. A cursor at or below the floor is healthy (or behind, which is a
     * different concern) — leave it untouched so we never disturb a node that
     * is finalizing normally. */
    if (cur <= floor) {
        progress_store_tx_unlock();
        return true;   /* no-op, clamped=false */
    }

    char *err = NULL;
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, &err) != SQLITE_OK) {
        LOG_WARN("stage_repair",
                 "[stage_repair] tip_finalize clamp BEGIN failed: %s",
                 err ? err : "(no message)");
        if (err) sqlite3_free(err);
        progress_store_tx_unlock();
        return false;
    }

    /* Clamp ONLY the tip_finalize cursor. No log deletions, no upstream cursor
     * changes — the upstream evidence stays intact so the re-finalize replays
     * it forward, and the surviving tip_finalize_log rows keep the Tier-2
     * public-tip floor at coins_best. */
    if (!stage_repair_force_stage_cursor(db, "tip_finalize", floor)) {
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        progress_store_tx_unlock();
        return false;
    }

    if (sqlite3_exec(db, "COMMIT", NULL, NULL, &err) != SQLITE_OK) {
        LOG_WARN("stage_repair",
                 "[stage_repair] tip_finalize clamp COMMIT failed: %s",
                 err ? err : "(no message)");
        if (err) sqlite3_free(err);
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        progress_store_tx_unlock();
        return false;
    }
    progress_store_tx_unlock();

    if (out)
        out->clamped = true;
    LOG_WARN("stage_repair",
             "[stage_repair] reducer cursor/coins desync: clamped tip_finalize "
             "cursor %d -> %d (coins_best=%d) so the reducer re-finalizes "
             "forward; no logs deleted, public tip floored at coins_best",
             cur, floor, coins_best);
    return true;
}
