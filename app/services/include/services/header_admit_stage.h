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
 *   cursor_out      == cursor_in + 1 on STAGE_ADVANCED
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
struct json_value;

/* Max steps drained per supervisor tick. Bounded to keep contention
 * on progress.kv low and to avoid starving other supervisor children. */
#define HEADER_ADMIT_BATCH_PER_TICK  500

/* Bind the stage to `ms` (the live chainstate) and ensure the
 * header_admit_log schema in progress.kv. Idempotent — a second call
 * returns true if already initialised against the same `ms`. Requires
 * `progress_store_open` to have succeeded first. */
bool header_admit_stage_init(struct main_state *ms);

/* Run one saga step. Returns the F-2 result code. Safe to call before
 * init (returns STAGE_IDLE). */
stage_result_t header_admit_stage_step_once(void);

/* Drain up to `max_steps` consecutive ADVANCE steps. Stops early on
 * IDLE, BLOCKED, or ERROR. Returns the count of ADVANCED steps. */
int header_admit_stage_drain(int max_steps);

/* Disarm + free. Idempotent. */
void header_admit_stage_shutdown(void);

/* Observability. */
uint64_t header_admit_stage_cursor(void);
uint64_t header_admit_stage_admitted_total(void);

/* zcl_state subsystem=header_admit (CLAUDE.md convention). */
bool header_admit_stage_dump_state_json(struct json_value *out,
                                         const char *key);

#endif /* ZCL_SERVICES_HEADER_ADMIT_STAGE_H */
