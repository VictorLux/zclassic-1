/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Stage primitive — a saga-style step that owns a persistent cursor.
 *
 * Why this exists
 * ----------------
 * The wedge class the master plan promises to extinct (`Wave S`) is
 * "chain advance silently stops making progress and nobody notices."
 * Today that progress lives in transient memory inside the chain-
 * advance coordinator. A stage primitive turns chain advance — and
 * every other long-running, batched workflow — into the same shape:
 *
 *   - A 64-bit cursor on disk identifies the last consumed unit.
 *   - A step function consumes the next unit (or batch), produces
 *     output, and writes the new cursor in the SAME transaction.
 *   - On crash-mid-step, the cursor is unchanged on next boot, so the
 *     work is replayed idempotently.
 *
 * Stage states (every step returns one):
 *
 *   STAGE_ADVANCED  — cursor moved; output committed
 *   STAGE_BLOCKED   — typed blocker preventing progress; cursor unchanged
 *   STAGE_IDLE      — no work available right now; cursor unchanged
 *   STAGE_ERROR     — unexpected failure; cursor unchanged
 *
 * Persistence (v1)
 * -----------------
 * One SQLite table, `stage_cursor`, keyed by stage name. Reads and
 * writes use direct prepared statements rather than the AR lifecycle
 * because:
 *
 *   - A cursor is a single column, not a row aggregate.
 *   - Stages are kernel primitives; the AR lifecycle is designed for
 *     app-layer models. Pulling AR in here adds dependencies and
 *     erases the "one writer per cursor" simplicity.
 *
 * Both sqlite3_step call sites carry a `// raw-sql-ok:kernel-primitive`
 * marker so the lint gate doesn't fire.
 *
 * Threading
 * ----------
 * The stage struct is single-writer. Caller orchestrates one step at a
 * time per stage; concurrent stage_run_once calls on the same stage are
 * undefined behaviour. Multiple stages can run in parallel because each
 * one owns a distinct (name, sqlite_db_handle) pair. */

#ifndef ZCL_UTIL_STAGE_H
#define ZCL_UTIL_STAGE_H

#include "util/blocker.h"

#include <sqlite3.h>
#include <stdbool.h>
#include <stdint.h>

#define STAGE_NAME_MAX 64

typedef enum {
    STAGE_ADVANCED = 0,
    STAGE_BLOCKED  = 1,
    STAGE_IDLE     = 2,
    STAGE_ERROR    = 3,
} stage_result_t;

const char *stage_result_name(stage_result_t r);

/* Context passed to a step function. The step:
 *   - Reads `cursor_in` (the current persisted cursor).
 *   - Does bounded work.
 *   - On advance: writes `cursor_out` (must be > cursor_in) and returns
 *     STAGE_ADVANCED. The framework commits the new cursor.
 *   - On blocked: fills `blocker` (caller-owned record) and returns
 *     STAGE_BLOCKED. The framework records the blocker via blocker_set
 *     and leaves cursor untouched.
 *   - On idle: returns STAGE_IDLE.
 *   - On error: returns STAGE_ERROR. */
struct stage_step_ctx {
    uint64_t              cursor_in;
    uint64_t              cursor_out;
    struct blocker_record blocker;     /* populated iff STAGE_BLOCKED */
    void                 *user;
};

typedef stage_result_t (*stage_step_fn)(struct stage_step_ctx *ctx);

typedef struct stage stage_t;

/* Construct a stage. `name` must be non-empty and ≤ STAGE_NAME_MAX - 1.
 * `step` must be non-NULL. `user` is opaque, passed to the step on
 * every invocation. Returns NULL on bad input. */
stage_t *stage_create(const char *name, stage_step_fn step, void *user);
void     stage_destroy(stage_t *s);

const char *stage_name(const stage_t *s);
uint64_t    stage_cursor(const stage_t *s);

/* Counters (for observability / Prometheus). */
uint64_t stage_advanced_count(const stage_t *s);
uint64_t stage_blocked_count(const stage_t *s);
uint64_t stage_idle_count(const stage_t *s);
uint64_t stage_error_count(const stage_t *s);

/* Initialize the `stage_cursor` table on the given DB handle. Safe to
 * call repeatedly. */
bool stage_table_ensure(sqlite3 *db);

/* Run one step:
 *   1. Read the current cursor from `stage_cursor` (defaults to 0 on
 *      first run).
 *   2. Invoke the step function with cursor_in populated.
 *   3. If the step returns STAGE_ADVANCED, persist cursor_out atomically
 *      in the same transaction (the step body should itself enroll any
 *      output writes into the outer txn via the user pointer).
 *   4. If STAGE_BLOCKED, call blocker_set with the filled record.
 *
 * Returns the step's result code. STAGE_ERROR if persistence fails. */
stage_result_t stage_run_once(stage_t *s, sqlite3 *db);

/* Boot-time restore: explicitly set the cursor. Persists immediately.
 * Intended for replaying a known-good cursor on import. */
bool stage_set_cursor(stage_t *s, sqlite3 *db, uint64_t value);

#endif /* ZCL_UTIL_STAGE_H */
