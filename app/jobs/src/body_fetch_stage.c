/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * body_fetch_stage — implementation. See jobs/body_fetch_stage.h.
 *
 * Single-process singleton, single-step (no worker pool). The work per
 * step is one in-memory flag check + one SQL insert, so batching adds
 * complexity without throughput. The F-2 stage primitive does the
 * cursor + replay heavy lifting; this module is the step body and the
 * schema-bootstrap glue for the `body_fetch_log` table that lives in
 * progress.kv alongside the cursor table. */

#include "platform/time_compat.h"
#include "jobs/body_fetch_stage.h"
#include "jobs/stage_helpers.h"

#include "chain/chain.h"
#include "core/uint256.h"
#include "json/json.h"
#include "storage/progress_store.h"
#include "util/blocker.h"
#include "util/log_macros.h"
#include "util/stage.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define STAGE_NAME "body_fetch"

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct main_state *g_ms = NULL;
static stage_t *g_stage = NULL;
static _Atomic uint64_t g_observed_total = 0;
static _Atomic uint64_t g_skipped_total  = 0;
static _Atomic int64_t  g_last_advance_height = -1;
static _Atomic int64_t  g_last_step_unix = 0;
static _Atomic int64_t  g_last_blocked_unix = 0;

/* ── Schema bootstrap (idempotent) ─────────────────────────────────── */

static bool ensure_log_schema(sqlite3 *db)
{
    static const char *const sql =
        "CREATE TABLE IF NOT EXISTS body_fetch_log ("
        "  height      INTEGER PRIMARY KEY,"
        "  hash        BLOB    NOT NULL,"
        "  source      TEXT    NOT NULL,"
        "  bytes       INTEGER NOT NULL DEFAULT 0,"
        "  fetched_at  INTEGER NOT NULL,"
        "  ok          INTEGER NOT NULL,"
        "  fail_reason TEXT"
        ")";
    char *err = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
        LOG_WARN("body_fetch", "[body_fetch] schema ensure failed: %s", err ? err : "(no message)");
        if (err) sqlite3_free(err);
        return false;
    }
    return true;
}

/* ── Reads from progress.kv ───────────────────────────────────────── */

/* Read the persisted cursor of the upstream stage. We query the
 * stage_cursor table directly rather than calling
 * `validate_headers_stage_cursor()` so that body_fetch's floor check is
 * decoupled from the upstream stage's runtime init order — and so the
 * floor reflects what is DURABLY committed, not the in-memory value
 * which is 0 on a fresh init until the first stage_run_once.
 * See stage_cursor_persisted() in jobs/stage_helpers.h. */

/* Returns 1 if found and ok-flag retrieved, 0 if no row, -1 on error. */
static int vh_log_ok_at(sqlite3 *db, int height, int *out_ok,
                        char *out_reason, size_t reason_size)
{
    *out_ok = -1;
    if (out_reason && reason_size)
        out_reason[0] = 0;
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(db,
        "SELECT ok, COALESCE(fail_reason,'') "
        "FROM validate_headers_log WHERE height = ?",
        -1, &st, NULL);
    if (rc != SQLITE_OK)
        LOG_ERR("body_fetch", "[body_fetch] vh log prepare failed: %s",
                sqlite3_errmsg(db));
    sqlite3_bind_int(st, 1, height);
    int found = 0;
    rc = sqlite3_step(st);  // raw-sql-ok:kernel-primitive
    if (rc == SQLITE_ROW) {
        *out_ok = sqlite3_column_int(st, 0);
        const unsigned char *txt = sqlite3_column_text(st, 1);
        if (txt && out_reason && reason_size)
            snprintf(out_reason, reason_size, "%s", (const char *)txt);
        found = 1;
    } else if (rc != SQLITE_DONE) {
        sqlite3_finalize(st);
        LOG_ERR("body_fetch",
                "[body_fetch] vh log step failed height=%d rc=%d: %s",
                height, rc, sqlite3_errmsg(db));
    }
    sqlite3_finalize(st);
    return found;
}

/* ── Writes to body_fetch_log ─────────────────────────────────────── */

static bool log_insert(sqlite3 *db, int height,
                        const struct uint256 *hash,
                        const char *source, int64_t bytes,
                        bool ok, const char *reason)
{
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
        "INSERT OR REPLACE INTO body_fetch_log "
        "(height, hash, source, bytes, fetched_at, ok, fail_reason) "
        "VALUES (?,?,?,?,?,?,?)",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        LOG_WARN("body_fetch", "[body_fetch] prepare insert failed: %s", sqlite3_errmsg(db));
        return false;
    }
    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)height);
    sqlite3_bind_blob (stmt, 2, hash->data, 32, SQLITE_STATIC);
    sqlite3_bind_text (stmt, 3, source, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 4, (sqlite3_int64)bytes);
    sqlite3_bind_int64(stmt, 5, (sqlite3_int64)platform_time_wall_unix());
    sqlite3_bind_int  (stmt, 6, ok ? 1 : 0);
    if (reason)
        sqlite3_bind_text(stmt, 7, reason, -1, SQLITE_TRANSIENT);
    else
        sqlite3_bind_null(stmt, 7);

    rc = sqlite3_step(stmt);  // raw-sql-ok:kernel-primitive
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        LOG_WARN("body_fetch", "[body_fetch] insert height=%d rc=%d", height, rc);
        return false;
    }
    return true;
}

/* ── Step body ─────────────────────────────────────────────────────── */

static job_result_t step_body_fetch(struct stage_step_ctx *c)
{
    atomic_store(&g_last_step_unix, platform_time_wall_unix());

    struct main_state *ms = g_ms;
    if (!ms) return JOB_IDLE;
    sqlite3 *db = progress_store_db();
    if (!db) return JOB_IDLE;

    int next_h = (int)c->cursor_in;
    if (next_h < 0) return JOB_FATAL;

    /* Floor: never overrun validate_headers' DURABLY persisted cursor.
     * validate cursor = "next height to validate" → heights
     * [0, vh_cursor-1] are validated. We can fetch up to vh_cursor-1.
     * Reading from disk (vs the in-memory accessor) means body_fetch
     * never advances past what is actually committed upstream, and
     * keeps body_fetch testable in isolation. */
    uint64_t vh_cursor = stage_cursor_persisted(db, "validate_headers",
                                                STAGE_NAME);
    if ((uint64_t)next_h >= vh_cursor) {
        atomic_store(&g_last_blocked_unix, platform_time_wall_unix());
        return JOB_IDLE;  /* not BLOCKED — validate will catch up */
    }

    /* Read the validate_headers_log row to learn pass/fail. Floor
     * guarantees the row exists; defend against torn writes anyway. */
    int vh_ok = -1;
    char vh_reason[96];
    int found = vh_log_ok_at(db, next_h, &vh_ok,
                             vh_reason, sizeof(vh_reason));
    if (found < 0) return JOB_FATAL;
    if (found == 0) {
        /* Row missing despite floor — surface as IDLE (will retry). */
        atomic_store(&g_last_blocked_unix, platform_time_wall_unix());
        return JOB_IDLE;
    }

    /* Look up the in-memory block_index entry — we need the hash and
     * the BLOCK_HAVE_DATA flag. */
    struct block_index *bi = active_chain_at(&ms->chain_active, next_h);
    if (!bi || !bi->phashBlock) {
        /* Concurrent reorg through this height between validate and
         * fetch. Surface as IDLE so the supervisor retries; the chain
         * will stabilise. */
        atomic_store(&g_last_blocked_unix, platform_time_wall_unix());
        return JOB_IDLE;
    }

    if (vh_ok == 0) {
        if (strcmp(vh_reason,
                   "no-header-solution-backfill-required") == 0) {
            blocker_init(&c->blocker,
                         "body_fetch.header_solution_missing",
                         STAGE_NAME,
                         BLOCKER_TRANSIENT,
                         "validate_headers is waiting for a real "
                         "Equihash solution, not rejecting consensus");
            atomic_store(&g_last_blocked_unix, platform_time_wall_unix());
            return JOB_BLOCKED;
        }
        /* Header failed PoW/Equihash earlier — record skip + advance. */
        if (!log_insert(db, next_h, bi->phashBlock,
                         "skipped_invalid", 0, false,
                         "header_validation_failed"))
            return JOB_FATAL;
        atomic_fetch_add(&g_skipped_total, 1);
        atomic_store(&g_last_advance_height, (int64_t)next_h);
        c->cursor_out = c->cursor_in + 1;
        return JOB_ADVANCED;
    }

    /* Header passed validation; check body availability. */
    if (!(bi->nStatus & BLOCK_HAVE_DATA)) {
        /* Body not yet on disk — JOB_IDLE, don't advance. The natural
         * backpressure: cursor stays put until msg_blocks brings it in. */
        atomic_store(&g_last_blocked_unix, platform_time_wall_unix());
        return JOB_IDLE;
    }

    /* Body observed on disk. Record presence; bytes=0 because size probing
     * would add per-height pread cost. */
    if (!log_insert(db, next_h, bi->phashBlock, "disk", 0, true, NULL))
        return JOB_FATAL;

    atomic_fetch_add(&g_observed_total, 1);
    atomic_store(&g_last_advance_height, (int64_t)next_h);
    c->cursor_out = c->cursor_in + 1;
    return JOB_ADVANCED;
}

/* ── Public API ────────────────────────────────────────────────────── */

bool body_fetch_stage_init(struct main_state *ms)
{
    if (!ms) LOG_FAIL("body_fetch", "init: NULL main_state");

    sqlite3 *db = progress_store_db();
    if (!db) LOG_FAIL("body_fetch",
        "init: progress_store not open");

    pthread_mutex_lock(&g_lock);

    /* Idempotent: same ms, already initialised → success. */
    if (g_stage != NULL) {
        bool same = (g_ms == ms);
        pthread_mutex_unlock(&g_lock);
        if (!same)
            LOG_FAIL("body_fetch",
                "init: already bound to a different main_state");
        return true;
    }

    if (!ensure_log_schema(db)) {
        pthread_mutex_unlock(&g_lock);
        return false;
    }

    stage_t *s = stage_create(STAGE_NAME, step_body_fetch, NULL);
    if (!s) {
        pthread_mutex_unlock(&g_lock);
        LOG_FAIL("body_fetch", "init: stage_create failed");
    }

    g_ms = ms;
    g_stage = s;
    pthread_mutex_unlock(&g_lock);

    LOG_INFO("body_fetch", "[body_fetch] stage initialised");
    return true;
}

job_result_t body_fetch_stage_step_once(void)
{
    if (!g_stage) return JOB_IDLE;
    sqlite3 *db = progress_store_db();
    if (!db) return JOB_IDLE;
    reducer_extend_window_to_candidate(g_ms, true);
    progress_store_tx_lock();
    job_result_t r = stage_run_once(g_stage, db);
    progress_store_tx_unlock();
    return r;
}

STAGE_DRAIN_IMPL(body_fetch)

void body_fetch_stage_shutdown(void)
{
    pthread_mutex_lock(&g_lock);
    if (g_stage) {
        stage_destroy(g_stage);
        g_stage = NULL;
    }
    g_ms = NULL;
    /* Reset per-init observability state. Persisted cursor + log rows
     * are preserved — that is the saga contract. */
    atomic_store(&g_observed_total, (uint64_t)0);
    atomic_store(&g_skipped_total, (uint64_t)0);
    atomic_store(&g_last_advance_height, (int64_t)-1);
    atomic_store(&g_last_step_unix, (int64_t)0);
    atomic_store(&g_last_blocked_unix, (int64_t)0);
    pthread_mutex_unlock(&g_lock);
}

uint64_t body_fetch_stage_cursor(void)
{
    return g_stage ? stage_cursor(g_stage) : 0;
}

uint64_t body_fetch_stage_observed_total(void)
{
    return atomic_load(&g_observed_total);
}

uint64_t body_fetch_stage_skipped_total(void)
{
    return atomic_load(&g_skipped_total);
}

bool body_fetch_stage_dump_state_json(struct json_value *out,
                                       const char *key)
{
    (void)key;
    if (!out) return false;
    json_set_object(out);

    json_push_kv_bool(out, "initialised", g_stage != NULL);
    json_push_kv_str (out, "stage_name", STAGE_NAME);
    json_push_kv_int (out, "cursor",
                      (int64_t)(g_stage ? stage_cursor(g_stage) : 0));
    json_push_kv_int (out, "observed_total",
                      (int64_t)atomic_load(&g_observed_total));
    json_push_kv_int (out, "skipped_total",
                      (int64_t)atomic_load(&g_skipped_total));
    json_push_kv_int (out, "last_advance_height",
                      atomic_load(&g_last_advance_height));
    json_push_kv_int (out, "last_step_unix",
                      atomic_load(&g_last_step_unix));
    json_push_kv_int (out, "last_blocked_unix",
                      atomic_load(&g_last_blocked_unix));
    if (g_stage) {
        json_push_kv_int(out, "advanced_count",
                         (int64_t)stage_advanced_count(g_stage));
        json_push_kv_int(out, "blocked_count",
                         (int64_t)stage_blocked_count(g_stage));
        json_push_kv_int(out, "idle_count",
                         (int64_t)stage_idle_count(g_stage));
        json_push_kv_int(out, "error_count",
                         (int64_t)stage_error_count(g_stage));
    }
    return true;
}
