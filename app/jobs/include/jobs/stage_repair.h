/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * stage_repair — reducer-stage repair helpers used by Conditions.
 *
 * These helpers operate on progress.kv, the reducer's durable cursor/log
 * store. They intentionally live with Jobs rather than Conditions so each
 * Condition stays a small detect/remedy/witness file. */

#ifndef ZCL_JOBS_STAGE_REPAIR_H
#define ZCL_JOBS_STAGE_REPAIR_H

#include "core/uint256.h"
#include "primitives/block.h"

#include <stdbool.h>

struct sqlite3;

enum stage_repair_header_solution_poison {
    STAGE_REPAIR_POISON_NONE = 0,
    STAGE_REPAIR_POISON_VALIDATE_SOLUTIONLESS,
    STAGE_REPAIR_POISON_DOWNSTREAM_STALE,
};

struct stage_repair_header_solution_result {
    bool repaired;
    int target_height;
    int deleted_rows;
    int rewound_cursors;
    enum stage_repair_header_solution_poison mode;
};

struct stage_repair_body_fetch_gap {
    bool ready;
    bool body_observed;
    int target_height;
    int validate_cursor;
    int body_fetch_cursor;
};

enum stage_repair_header_solution_poison
stage_repair_header_solution_poison_mode(struct sqlite3 *db, int height);

bool stage_repair_header_solution_poison_present(struct sqlite3 *db,
                                                 int height);

bool stage_repair_header_solution_save(struct sqlite3 *db,
                                       int height,
                                       const struct uint256 *hash,
                                       const struct block_header *header);

bool stage_repair_header_solution_load(struct sqlite3 *db,
                                       int height,
                                       const struct uint256 *expected_hash,
                                       struct block_header *out);

/* Returns true iff a header-solution row is present at `height` AND — when
 * `expected_hash != NULL` — its stored hash equals expected_hash, i.e. the
 * CORRECT solution for the canonical block at that height is present. Pass NULL
 * for a presence-only check (any row that round-trips). Hash-aware callers pass
 * active_chain_at(height)->phashBlock so a STALE wrong-block row (e.g. an
 * earlier off-by-N save) does NOT count as available — otherwise the backfill /
 * self-heal paths would skip a height whose stored solution validate_headers
 * (whose load IS hash-checked) keeps rejecting, wedging the tip. */
bool stage_repair_header_solution_available(struct sqlite3 *db, int height,
                                            const struct uint256 *expected_hash);

bool stage_repair_header_solution_poison_rewind(
    struct sqlite3 *db,
    int height,
    int active_tip_height,
    struct stage_repair_header_solution_result *out);

bool stage_repair_body_fetch_missing_have_data_candidate(
    struct sqlite3 *db,
    int height,
    struct stage_repair_body_fetch_gap *out);

bool stage_repair_body_fetch_missing_have_data_frontier_candidate(
    struct sqlite3 *db,
    struct stage_repair_body_fetch_gap *out);

bool stage_repair_body_fetch_observed(struct sqlite3 *db, int height);

struct stage_reconcile_result {
    bool clamped;   /* the tip_finalize cursor was strictly above floor and moved */
    int  floor;     /* coins_best + 1 */
};

/* Reconcile a reducer cursor/coins desync that wedges the chain.
 *
 * After an unclean restart (kill-9 + WAL) the durable tip_finalize cursor can
 * sit AHEAD of the durably-applied coins tip (`coins_best`). tip_finalize then
 * idles ("cursor says done") and never re-finalizes, so the connect gate
 * rejects every block at coins_best+1 with "block-not-finalized-by-reducer".
 *
 * This clamps ONLY the tip_finalize cursor DOWN to `coins_best + 1`, so on the
 * next stage init tip_finalize re-finalizes coins_best+1.. forward by replaying
 * the INTACT upstream logs (utxo_apply_log etc. are left untouched; their
 * cursors are left at the header high, so step_finalize's `next_h <
 * utxo_apply_cursor` precondition and `utxo_apply_log_at(next_h)` lookup both
 * succeed).
 *
 * SAFETY (proven in test_stage_reducer_unwedge):
 *   - It NEVER deletes any *_log row, so the Tier-2 public-tip authority
 *     (`SELECT MAX(height) FROM tip_finalize_log WHERE ok=1`) can never drop
 *     below coins_best — the surviving anchor row at coins_best holds the floor.
 *     The public tip can only move UPWARD from coins_best.
 *   - It touches ONLY the tip_finalize cursor — no upstream cursor or log — so
 *     the re-finalize cannot self-stall.
 *   - No-op (clamped=false) unless the tip_finalize cursor is strictly above
 *     `coins_best + 1`; refuses (returns true, clamped=false) when
 *     `coins_best < 0` (no durable anchor to floor on).
 *
 * Must run at boot AFTER coins_best is durable and BEFORE the stages init (so
 * they load the clamped cursor). Single transaction. */
bool stage_reconcile_clamp_tip_finalize_to_floor(
    struct sqlite3 *db,
    int coins_best,
    struct stage_reconcile_result *out);

#endif /* ZCL_JOBS_STAGE_REPAIR_H */
