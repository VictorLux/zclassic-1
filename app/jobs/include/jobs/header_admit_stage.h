/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * header_admit_stage — Wave S, S-2.
 *
 * The first concrete saga consumer of progress.kv. Operates in
 * **shadow mode**: reads from the live in-memory active chain, writes
 * its own log to progress.kv's `header_admit_log` table, never mutates
 * the legacy block_index or any consensus structure. When the diff
 * harness (S-11) confirms parity for 30 days, S-12 will flip this
 * stage to authoritative.
 *
 * Cursor semantics
 * -----------------
 *   cursor = next height to admit
 *   cursor_in == 0  → about to admit genesis
 *   cursor_out      == cursor_in + 1 on JOB_ADVANCED
 *
 * Idempotency
 * ------------
 * Each step admits one header at height `cursor_in`. Writes to
 * `header_admit_log` use `INSERT OR REPLACE` keyed on height, so a
 * replay after crash-mid-step re-emits the same row (or a different
 * one if the chain has reorged through that height in between).
 *
 * The F-2 `stage` primitive wraps each step in a `BEGIN IMMEDIATE`
 * transaction on the progress.kv handle, so the log insert + cursor
 * UPSERT commit atomically.
 *
 * Schema
 * -------
 *   CREATE TABLE IF NOT EXISTS header_admit_log (
 *     height      INTEGER PRIMARY KEY,
 *     hash        BLOB    NOT NULL,
 *     parent_hash BLOB,              -- NULL for genesis
 *     admitted_at INTEGER NOT NULL
 *   );
 *
 * Lifecycle
 * ----------
 * `init` binds the stage to a `main_state` and ensures the schema.
 * `step_once` runs one step (used both by the supervisor's periodic
 * tick and by the unit tests). `shutdown` disposes the stage. The
 * supervisor wiring lives in `config/src/boot_services.c` —
 * `staged.header_admit` is registered with `period_secs=2`, and its
 * on_tick drains up to `HEADER_ADMIT_BATCH_PER_TICK` steps before
 * yielding. */

#ifndef ZCL_SERVICES_HEADER_ADMIT_STAGE_H
#define ZCL_SERVICES_HEADER_ADMIT_STAGE_H

#include "util/stage.h"

#include <stdbool.h>
#include <stdint.h>

struct main_state;
struct block_index;
struct uint256;
struct json_value;

/* Max steps drained per supervisor tick. Bounded to keep contention
 * on progress.kv low and to avoid starving other supervisor children. */
#define HEADER_ADMIT_BATCH_PER_TICK  500

typedef enum {
    HEADER_ADMIT_MODE_SHADOW = 0,
    HEADER_ADMIT_MODE_AUTHORITATIVE
} header_admit_mode_t;

/* Cutover mode. SHADOW observes/logs only; AUTHORITATIVE lets the
 * stage drive header admission while legacy ingress becomes a guard. */
void header_admit_set_mode(header_admit_mode_t mode);
header_admit_mode_t header_admit_get_mode(void);

/* Bind the stage to `ms` (the live chainstate) and ensure the
 * header_admit_log schema in progress.kv. Idempotent — a second call
 * returns true if already initialised against the same `ms`. Requires
 * `progress_store_open` to have succeeded first. */
bool header_admit_stage_init(struct main_state *ms);

/* Run one saga step. Returns the F-2 result code. Safe to call before
 * init (returns JOB_IDLE). */
job_result_t header_admit_stage_step_once(void);

/* Drain up to `max_steps` consecutive ADVANCE steps. Stops early on
 * IDLE, BLOCKED, or ERROR. Returns the count of ADVANCED steps. */
int header_admit_stage_drain(int max_steps);

/* Disarm + free. Idempotent. */
void header_admit_stage_shutdown(void);

/* Observability. */
uint64_t header_admit_stage_cursor(void);
uint64_t header_admit_stage_admitted_total(void);
bool header_admit_stage_has_record(int32_t height,
                                   const struct uint256 *hash);

/* zcl_state subsystem=header_admit (CLAUDE.md convention). */
bool header_admit_stage_dump_state_json(struct json_value *out,
                                         const char *key);

#ifdef ZCL_TESTING
typedef bool (*header_admit_authoritative_hook)(
    struct main_state *ms,
    struct block_index *bi,
    void *user);

void header_admit_stage_set_authoritative_hook(
    header_admit_authoritative_hook hook,
    void *user);
#endif

/* ── S-11 mini-diff harness ─────────────────────────────────────────── */
/*
 * Compares the contents of `header_admit_log` (what S-2 has recorded)
 * against the live in-memory `active_chain` (the source S-2 read from).
 * Provides empirical confidence that the shadow stage is keeping parity
 * with the legacy chain advance before more stages stack on top.
 *
 * Possible anomalies:
 *   - DIVERGENT:    same height, different hash bytes (real bug)
 *   - LOG_AHEAD:    log has entries the chain no longer does (post-reorg)
 *   - CHAIN_AHEAD:  chain has heights the log hasn't admitted yet (cursor lag)
 *   - CONVERGED:    every compared height matches
 *
 * The function is read-only: no writes, no allocations beyond a single
 * bounded scratch buffer for the log rows, no cs_main lock acquired
 * (snapshot races are accepted for a diagnostic tool). */

enum header_admit_diff_status {
    HEADER_ADMIT_DIFF_CONVERGED    = 0,
    HEADER_ADMIT_DIFF_DIVERGENT    = 1,
    HEADER_ADMIT_DIFF_LOG_AHEAD    = 2,
    HEADER_ADMIT_DIFF_CHAIN_AHEAD  = 3,
    HEADER_ADMIT_DIFF_EMPTY        = 4,
    HEADER_ADMIT_DIFF_NOT_READY    = 5,  /* stage not init OR progress.kv closed */
};

#define HEADER_ADMIT_DIFF_MAX_SAMPLES 32
#define HEADER_ADMIT_DIFF_MAX_RANGE   10000  /* hard cap on (end-start+1) */

struct header_admit_diff_sample {
    int32_t height;
    uint8_t log_hash[32];
    uint8_t chain_hash[32];
    bool    log_present;
    bool    chain_present;
};

struct header_admit_diff_report {
    enum header_admit_diff_status status;
    int32_t start_height;
    int32_t end_height;
    int32_t checked_count;            /* heights with at least one side present */
    int32_t match_count;
    int32_t mismatch_count;
    int32_t missing_in_log_count;     /* chain has, log doesn't */
    int32_t missing_in_chain_count;   /* log has, chain doesn't */
    int32_t first_divergent_height;   /* -1 if no divergence */
    int32_t log_max_height;           /* MAX(height) FROM log, -1 if empty */
    int32_t chain_tip_height;         /* active_chain_height, -1 if NULL */
    int32_t cursor;                   /* stage cursor (next height to admit) */
    int     sample_count;
    struct header_admit_diff_sample samples[HEADER_ADMIT_DIFF_MAX_SAMPLES];
};

/* Compute the diff over [start_h, end_h]. Pass -1 for either bound to
 * mean "auto":
 *   start: recent tail when end is auto, otherwise 0
 *   end:   min(log_max_height, chain_tip_height)
 *
 * The range is hard-capped at HEADER_ADMIT_DIFF_MAX_RANGE heights; if a
 * caller asks for more with an explicit start, the effective end is
 * clamped and reported in `out->end_height`. With both bounds auto, the
 * effective start is shifted forward so the capped range covers the
 * most recent heights. The caller-owned `out` is fully populated on every
 * return, including failure cases (status = NOT_READY etc.).
 *
 * Returns true on success (report is valid). Returns false only on
 * misuse (out=NULL). */
bool header_admit_stage_diff(int32_t start_h, int32_t end_h,
                              struct header_admit_diff_report *out);

#endif /* ZCL_SERVICES_HEADER_ADMIT_STAGE_H */
