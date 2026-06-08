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
struct main_state;

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

struct stage_reducer_frontier_reconcile_result {
    bool repaired;
    bool clamped_tip_finalize;
    bool refused_coin_tear;
    bool refused_coin_unknown;
    bool coins_applied_found;
    int hstar;
    int served_floor;
    int coins_applied_height;
    bool clamped_body_fetch;
    int body_fetch_cursor_before;
    int body_fetch_cursor_after;
    int tip_finalize_cursor_before;
    int tip_finalize_cursor_after;
    int sweep_top;
    int lowest_have_data_cleared;
    int scripts_set;
    int have_data_set;
    int have_data_cleared;
    int failed_mask_cleared;
    int header_events_emitted;
};

/* Reconcile a reducer cursor/coins desync that wedges the chain.
 *
 * After an unclean restart (kill-9 + WAL) the durable tip_finalize cursor can
 * sit AHEAD of the durably-applied coins tip (`coins_best`). tip_finalize then
 * idles ("cursor says done") and never re-finalizes, so the connect gate
 * rejects every block at coins_best+1 with "block-not-finalized-by-reducer".
 *
 * This reconciles ONLY the tip_finalize cursor to the stronger of
 * `coins_best + 1` and durable served finality
 * (`MAX(tip_finalize_log.ok=1) + 1`). The second guard is mandatory because a
 * stale coins_best scan can lag rows already served to RPC; boot must never
 * lower the public tip below those finalized rows. Upstream logs/cursors are
 * left untouched, so any re-finalize pass still has its evidence.
 *
 * SAFETY (proven in test_stage_reducer_unwedge):
 *   - It NEVER deletes any *_log row, and it never writes the tip_finalize
 *     cursor below the Tier-2 public-tip authority
 *     (`SELECT MAX(height) FROM tip_finalize_log WHERE ok=1`) plus one. The
 *     public tip must not regress below served finality.
 *   - It touches ONLY the tip_finalize cursor — no upstream cursor or log — so
 *     the re-finalize cannot self-stall.
 *   - No-op (clamped=false) when the tip_finalize cursor already equals the
 *     target; refuses (returns true, clamped=false) when `coins_best < 0`
 *     (no durable anchor to floor on).
 *
 * Must run at boot AFTER coins_best is durable and BEFORE the stages init (so
 * they load the clamped cursor). Single transaction. */
bool stage_reconcile_clamp_tip_finalize_to_floor(
    struct sqlite3 *db,
    int coins_best,
    struct stage_reconcile_result *out);

/* L1 reducer-frontier reconcile: repair block_index mirror flags from
 * hash-bound durable reducer logs, rewind body_fetch to the lowest cleared
 * HAVE_DATA hole so bodies can be re-requested, then clamp tip_finalize to the
 * coin-capped H*+1 floor so tip_finalize can replay forward. This is a
 * flag/cursor repair only: it never deletes log rows and never mutates coins.
 * If coins_applied_height is absent or present above H*, the helper refuses
 * (unknown/L2 coin-tear domain). */
bool stage_reducer_frontier_reconcile_light_needed(
    struct sqlite3 *db,
    struct main_state *ms,
    struct stage_reducer_frontier_reconcile_result *out);

bool stage_reducer_frontier_reconcile_light(
    struct sqlite3 *db,
    struct main_state *ms,
    struct stage_reducer_frontier_reconcile_result *out);

#endif /* ZCL_JOBS_STAGE_REPAIR_H */
