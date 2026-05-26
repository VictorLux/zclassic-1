/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * tip_finalize_stage — implementation. See jobs/tip_finalize_stage.h.
 *
 * S-9 consumes utxo_apply_log and records the live tip-finalize event.
 * It writes only tip_finalize_log plus its stage cursor in progress.kv. */

#include "platform/time_compat.h"
#include "jobs/tip_finalize_stage.h"

#include "chain/chain.h"
#include "core/arith_uint256.h"
#include "event/event.h"
#include "json/json.h"
#include "storage/progress_store.h"
#include "util/log_macros.h"
#include "util/stage.h"
#include "validation/main_state.h"

#include <pthread.h>
#include <sqlite3.h>
#include <stdatomic.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define STAGE_NAME "tip_finalize"

struct utxo_apply_row {
    int ok;
    int64_t spent_count;
    int64_t added_count;
};

struct finalized_tip_row {
    bool found;
    bool ok;
    bool has_tip_hash;
    struct uint256 tip_hash;
};

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct main_state *g_ms = NULL;
static stage_t *g_stage = NULL;
static tip_finalize_utxo_count_fn g_utxo_counter = NULL;
static void *g_utxo_counter_user = NULL;

static _Atomic uint64_t g_finalized_total = 0;
static _Atomic uint64_t g_upstream_failed_total = 0;
static _Atomic uint64_t g_reorg_detected_total = 0;
static _Atomic uint64_t g_utxo_count_diverged_total = 0;
static _Atomic uint64_t g_precondition_failed_total = 0;
static _Atomic uint64_t g_total_work_added_high = 0;
static _Atomic uint64_t g_total_work_added_low = 0;
static _Atomic int64_t  g_last_step_unix = 0;
static _Atomic int64_t  g_last_blocked_unix = 0;
static _Atomic int64_t  g_last_advance_height = -1;

static int64_t wall_now_s(void)
{
    return (int64_t)platform_time_wall_time_t();
}

static bool ensure_log_schema(sqlite3 *db)
{
    static const char *const sql =
        "CREATE TABLE IF NOT EXISTS tip_finalize_log ("
        "  height           INTEGER PRIMARY KEY,"
        "  status           TEXT    NOT NULL,"
        "  ok               INTEGER NOT NULL,"
        "  work_delta_high  INTEGER NOT NULL,"
        "  work_delta_low   INTEGER NOT NULL,"
        "  utxo_size_after  INTEGER NOT NULL,"
        "  reorg_depth      INTEGER NOT NULL,"
        "  finalized_at     INTEGER NOT NULL"
        ")";
    char *err = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr,  // obs-ok:tip-finalize-schema-failure
                "[tip_finalize] schema ensure failed: %s\n",
                err ? err : "(no message)");
        if (err) sqlite3_free(err);
        return false;
    }
    if (sqlite3_exec(db,
        "ALTER TABLE tip_finalize_log ADD COLUMN tip_hash BLOB",
        NULL, NULL, &err) != SQLITE_OK) {
        if (!err || strstr(err, "duplicate column name") == NULL) {
            fprintf(stderr,  // obs-ok:tip-finalize-schema-failure
                    "[tip_finalize] schema alter failed: %s\n",
                    err ? err : "(no message)");
            if (err) sqlite3_free(err);
            return false;
        }
        sqlite3_free(err);
    }
    return true;
}

static uint64_t upstream_cursor_persisted(sqlite3 *db, const char *name)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
        "SELECT cursor FROM stage_cursor WHERE name = ?",
        -1, &st, NULL) != SQLITE_OK) {
        fprintf(stderr,  // obs-ok:tip-finalize-upstream-prepare-failure
                "[tip_finalize] upstream cursor prepare failed: %s\n",
                sqlite3_errmsg(db));
        return 0;
    }
    sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
    uint64_t out = 0;
    if (sqlite3_step(st) == SQLITE_ROW)  // raw-sql-ok:kernel-primitive
        out = (uint64_t)sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return out;
}

static int utxo_apply_log_at(sqlite3 *db, int height,
                             struct utxo_apply_row *out)
{
    memset(out, 0, sizeof(*out));
    out->ok = -1;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
        "SELECT ok, spent_count, added_count "
        "FROM utxo_apply_log WHERE height = ?",
        -1, &st, NULL) != SQLITE_OK) {
        fprintf(stderr,  // obs-ok:tip-finalize-utxo-log-prepare-failure
                "[tip_finalize] utxo_apply_log prepare failed: %s\n",
                sqlite3_errmsg(db));
        return -1;  // raw-return-ok:logged-above
    }
    sqlite3_bind_int(st, 1, height);
    int found = 0;
    int rc = sqlite3_step(st);  // raw-sql-ok:kernel-primitive
    if (rc == SQLITE_ROW) {
        out->ok = sqlite3_column_int(st, 0);
        out->spent_count = sqlite3_column_int64(st, 1);
        out->added_count = sqlite3_column_int64(st, 2);
        found = 1;
    }
    sqlite3_finalize(st);
    return found;
}

static bool utxo_apply_sums_through(sqlite3 *db, int height,
                                    int64_t *spent_out,
                                    int64_t *added_out)
{
    *spent_out = 0;
    *added_out = 0;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
        "SELECT COALESCE(SUM(spent_count),0), "
        "       COALESCE(SUM(added_count),0) "
        "FROM utxo_apply_log WHERE height <= ? AND ok = 1",
        -1, &st, NULL) != SQLITE_OK) {
        fprintf(stderr,  // obs-ok:tip-finalize-utxo-sum-prepare-failure
                "[tip_finalize] utxo_apply_log sum prepare failed: %s\n",
                sqlite3_errmsg(db));
        return false;
    }
    sqlite3_bind_int(st, 1, height);
    bool ok = false;
    if (sqlite3_step(st) == SQLITE_ROW) {  // raw-sql-ok:kernel-primitive
        *spent_out = sqlite3_column_int64(st, 0);
        *added_out = sqlite3_column_int64(st, 1);
        ok = true;
    }
    sqlite3_finalize(st);
    return ok;
}

static bool log_insert(sqlite3 *db, int height, const char *status, bool ok,
                       const struct arith_uint256 *work_delta,
                       int64_t utxo_size_after, int reorg_depth,
                       const struct uint256 *tip_hash)
{
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
        "INSERT OR REPLACE INTO tip_finalize_log "
        "(height, status, ok, work_delta_high, work_delta_low, "
        " utxo_size_after, reorg_depth, finalized_at, tip_hash) "
        "VALUES (?,?,?,?,?,?,?,?,?)",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr,  // obs-ok:tip-finalize-log-prepare-failure
                "[tip_finalize] prepare insert failed: %s\n",
                sqlite3_errmsg(db));
        return false;
    }

    uint64_t hi = 0, lo = 0;
    if (work_delta) {
        lo = arith_uint256_get_low64(work_delta);
        hi = ((uint64_t)work_delta->pn[3] << 32) | work_delta->pn[2];
    }

    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)height);
    sqlite3_bind_text (stmt, 2, status, -1, SQLITE_STATIC);
    sqlite3_bind_int  (stmt, 3, ok ? 1 : 0);
    sqlite3_bind_int64(stmt, 4, (sqlite3_int64)hi);
    sqlite3_bind_int64(stmt, 5, (sqlite3_int64)lo);
    sqlite3_bind_int64(stmt, 6, (sqlite3_int64)utxo_size_after);
    sqlite3_bind_int  (stmt, 7, reorg_depth);
    sqlite3_bind_int64(stmt, 8, (sqlite3_int64)wall_now_s());
    if (tip_hash)
        sqlite3_bind_blob(stmt, 9, tip_hash->data, 32, SQLITE_STATIC);
    else
        sqlite3_bind_null(stmt, 9);
    rc = sqlite3_step(stmt);  // raw-sql-ok:kernel-primitive
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        fprintf(stderr,  // obs-ok:tip-finalize-log-insert-failure
                "[tip_finalize] insert height=%d rc=%d\n", height, rc);
        return false;
    }
    return true;
}

static bool finalized_tip_row_at(sqlite3 *db, int height,
                                 struct finalized_tip_row *out)
{
    memset(out, 0, sizeof(*out));
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
        "SELECT ok, tip_hash FROM tip_finalize_log WHERE height = ?",
        -1, &st, NULL) != SQLITE_OK) {
        fprintf(stderr,  // obs-ok:tip-finalize-hash-prepare-failure
                "[tip_finalize] finalized row prepare failed: %s\n",
                sqlite3_errmsg(db));
        return false;
    }
    sqlite3_bind_int(st, 1, height);
    int rc = sqlite3_step(st);  // raw-sql-ok:kernel-primitive
    if (rc == SQLITE_ROW) {
        out->found = true;
        out->ok = sqlite3_column_int(st, 0) != 0;
        const void *blob = sqlite3_column_blob(st, 1);
        int n = sqlite3_column_bytes(st, 1);
        if (blob && n == 32) {
            memcpy(out->tip_hash.data, blob, 32);
            out->has_tip_hash = true;
        }
    } else if (rc != SQLITE_DONE) {
        fprintf(stderr,  // obs-ok:tip-finalize-hash-step-failure
                "[tip_finalize] finalized row step failed rc=%d: %s\n",
                rc, sqlite3_errmsg(db));
        sqlite3_finalize(st);
        return false;
    }
    sqlite3_finalize(st);
    return true;
}

static int64_t log_row_count(sqlite3 *db)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
        "SELECT COUNT(*) FROM tip_finalize_log",
        -1, &st, NULL) != SQLITE_OK) {
        fprintf(stderr,  // obs-ok:tip-finalize-count-prepare-failure
                "[tip_finalize] log count prepare failed: %s\n",
                sqlite3_errmsg(db));
        return -1;  // raw-return-ok:logged-above
    }
    int64_t n = -1;
    if (sqlite3_step(st) == SQLITE_ROW)  // raw-sql-ok:kernel-primitive
        n = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return n;
}

static int reorg_depth_from(struct block_index *old_tip,
                            struct block_index *new_tip)
{
    int depth = 0;
    struct block_index *p = new_tip;
    while (p && p->nHeight > old_tip->nHeight) {
        p = p->pprev;
        depth++;
    }
    while (p && old_tip && p != old_tip) {
        p = p->pprev;
        old_tip = old_tip->pprev;
        depth++;
    }
    return depth > 0 ? depth : 1;
}

static bool preconditions_ok(const struct block_index *bi)
{
    if (!bi) return false;
    if (!(bi->nStatus & BLOCK_HAVE_DATA)) return false;
    if ((bi->nStatus & BLOCK_VALID_MASK) < BLOCK_VALID_SCRIPTS)
        return false;
    if ((bi->nStatus & BLOCK_VALID_MASK) < BLOCK_VALID_HEADER)
        return false;
    return true;
}

static bool finalized_row_active_match(sqlite3 *db, int row_height,
                                       bool *out_known,
                                       bool *out_matches)
{
    *out_known = false;
    *out_matches = false;
    struct finalized_tip_row row;
    if (!finalized_tip_row_at(db, row_height, &row))
        return false;
    if (!row.found || !row.ok || !row.has_tip_hash)
        return true;

    struct main_state *ms = g_ms;
    struct block_index *active =
        ms ? active_chain_at(&ms->chain_active, row_height + 1) : NULL;
    if (!active || !active->phashBlock)
        return true;
    *out_known = true;
    *out_matches = uint256_eq(&row.tip_hash, active->phashBlock);
    return true;
}

static bool rewind_cursor_if_active_chain_reorged(sqlite3 *db)
{
    if (!g_stage || !g_ms)
        return true;

    uint64_t cursor = upstream_cursor_persisted(db, STAGE_NAME);
    if (cursor == 0)
        return true;
    if (cursor > (uint64_t)INT32_MAX) {
        fprintf(stderr,  // obs-ok:tip-finalize-rewind-failure
                "[tip_finalize] reorg rewind cursor too large: %llu\n",
                (unsigned long long)cursor);
        return false;
    }

    bool known = false;
    bool matches = false;
    if (!finalized_row_active_match(db, (int)cursor - 1, &known, &matches))
        return false;
    if (!known || matches)
        return true;

    uint64_t rewind_to = 0;
    for (int h = (int)cursor - 2; h >= 0; h--) {
        known = false;
        matches = false;
        if (!finalized_row_active_match(db, h, &known, &matches))
            return false;
        if (known && matches) {
            rewind_to = (uint64_t)h + 1u;
            break;
        }
    }
    if (rewind_to == cursor)
        return true;

    if (!stage_set_cursor(g_stage, db, rewind_to)) {
        fprintf(stderr,  // obs-ok:tip-finalize-rewind-failure
                "[tip_finalize] reorg rewind failed from=%llu to=%llu\n",
                (unsigned long long)cursor,
                (unsigned long long)rewind_to);
        return false;
    }

    atomic_fetch_add(&g_reorg_detected_total, 1);
    atomic_store(&g_last_blocked_unix, wall_now_s());
    event_emitf(EV_BLOCK_REJECTED, 0,
                "tip_finalize reorg_cursor_rewind from=%llu to=%llu",
                (unsigned long long)cursor,
                (unsigned long long)rewind_to);
    return true;
}

static bool live_utxo_count_after(int height_after, int64_t *out_count)
{
    *out_count = -1;
    if (!g_utxo_counter)
        return true;
    return g_utxo_counter(height_after, out_count, g_utxo_counter_user);
}

static job_result_t step_finalize(struct stage_step_ctx *c)
{
    atomic_store(&g_last_step_unix, wall_now_s());

    struct main_state *ms = g_ms;
    if (!ms) return JOB_IDLE;
    sqlite3 *db = progress_store_db();
    if (!db) return JOB_IDLE;

    int next_h = (int)c->cursor_in;
    if (next_h < 0) return JOB_FATAL;

    uint64_t uv_cursor = upstream_cursor_persisted(db, "utxo_apply");
    if ((uint64_t)next_h >= uv_cursor) {
        atomic_store(&g_last_blocked_unix, wall_now_s());
        return JOB_IDLE;
    }

    struct utxo_apply_row upstream;
    int found = utxo_apply_log_at(db, next_h, &upstream);
    if (found < 0) return JOB_FATAL;
    if (found == 0) {
        atomic_store(&g_last_blocked_unix, wall_now_s());
        return JOB_IDLE;
    }

    if (upstream.ok == 0) {
        struct arith_uint256 zero;
        arith_uint256_set_zero(&zero);
        if (!log_insert(db, next_h, "upstream_failed", false, &zero,
                        -1, 0, NULL))
            return JOB_FATAL;
        atomic_fetch_add(&g_upstream_failed_total, 1);
        atomic_store(&g_last_advance_height, (int64_t)next_h);
        c->cursor_out = c->cursor_in + 1;
        return JOB_ADVANCED;
    }

    struct block_index *old_tip = active_chain_at(&ms->chain_active, next_h);
    struct block_index *new_tip = active_chain_at(&ms->chain_active,
                                                  next_h + 1);
    if (!new_tip) {
        atomic_store(&g_last_blocked_unix, wall_now_s());
        return JOB_IDLE;
    }
    if (!old_tip) {
        atomic_store(&g_last_blocked_unix, wall_now_s());
        return JOB_IDLE;
    }

    struct arith_uint256 work_delta;
    arith_uint256_set_zero(&work_delta);

    if (new_tip->pprev != old_tip) {
        int depth = reorg_depth_from(old_tip, new_tip);
        if (!log_insert(db, next_h, "reorg_detected", false, &work_delta,
                        -1, depth, NULL))
            return JOB_FATAL;
        atomic_fetch_add(&g_reorg_detected_total, 1);
        event_emitf(EV_BLOCK_REJECTED, 0,
                    "tip_finalize reorg_detected height=%d depth=%d",
                    next_h, depth);
        c->cursor_out = c->cursor_in + 1;
        return JOB_ADVANCED;
    }

    if (!preconditions_ok(new_tip) ||
        arith_uint256_compare(&new_tip->nChainWork,
                              &old_tip->nChainWork) <= 0) {
        if (!log_insert(db, next_h, "precondition_failed", false,
                        &work_delta, -1, 0, NULL))
            return JOB_FATAL;
        atomic_fetch_add(&g_precondition_failed_total, 1);
        event_emitf(EV_BLOCK_REJECTED, 0,
                    "tip_finalize precondition_failed height=%d", next_h);
        c->cursor_out = c->cursor_in + 1;
        return JOB_ADVANCED;
    }

    arith_uint256_sub(&work_delta, &new_tip->nChainWork,
                      &old_tip->nChainWork);

    int64_t utxo_size_after = -1;
    if (!live_utxo_count_after(next_h + 1, &utxo_size_after))
        return JOB_FATAL;
    if (utxo_size_after >= 0) {
        int64_t spent = 0, added = 0;
        if (!utxo_apply_sums_through(db, next_h, &spent, &added))
            return JOB_FATAL;
        int64_t expected = added - spent;
        if (utxo_size_after != expected) {
            if (!log_insert(db, next_h, "utxo_count_diverged", false,
                            &work_delta, utxo_size_after, 0, NULL))
                return JOB_FATAL;
            atomic_fetch_add(&g_utxo_count_diverged_total, 1);
            event_emitf(EV_BLOCK_REJECTED, 0,
                        "tip_finalize utxo_count_diverged height=%d "
                        "live=%lld expected=%lld",
                        next_h, (long long)utxo_size_after,
                        (long long)expected);
            c->cursor_out = c->cursor_in + 1;
            return JOB_ADVANCED;
        }
    }

    if (!log_insert(db, next_h, "finalized", true, &work_delta,
                    utxo_size_after, 0, new_tip->phashBlock))
        return JOB_FATAL;

    atomic_fetch_add(&g_finalized_total, 1);
    atomic_fetch_add(&g_total_work_added_low,
                     arith_uint256_get_low64(&work_delta));
    atomic_fetch_add(&g_total_work_added_high,
                     ((uint64_t)work_delta.pn[3] << 32) | work_delta.pn[2]);
    atomic_store(&g_last_advance_height, (int64_t)next_h);
    c->cursor_out = c->cursor_in + 1;
    return JOB_ADVANCED;
}

bool tip_finalize_stage_init(struct main_state *ms)
{
    if (!ms) LOG_FAIL("tip_finalize", "init: NULL main_state");

    sqlite3 *db = progress_store_db();
    if (!db) LOG_FAIL("tip_finalize", "init: progress_store not open");

    pthread_mutex_lock(&g_lock);
    if (g_stage != NULL) {
        bool same = (g_ms == ms);
        pthread_mutex_unlock(&g_lock);
        if (!same)
            LOG_FAIL("tip_finalize",
                "init: already bound to a different main_state");
        return true;
    }

    if (!ensure_log_schema(db)) {
        pthread_mutex_unlock(&g_lock);
        return false;
    }

    stage_t *s = stage_create(STAGE_NAME, step_finalize, NULL);
    if (!s) {
        pthread_mutex_unlock(&g_lock);
        LOG_FAIL("tip_finalize", "init: stage_create failed");
    }

    g_ms = ms;
    g_stage = s;
    pthread_mutex_unlock(&g_lock);

    fprintf(stderr,  // obs-ok:tip-finalize-lifecycle
            "[tip_finalize] stage initialised (shadow mode)\n");
    return true;
}

job_result_t tip_finalize_stage_step_once(void)
{
    if (!g_stage) return JOB_IDLE;
    sqlite3 *db = progress_store_db();
    if (!db) return JOB_IDLE;
    if (!rewind_cursor_if_active_chain_reorged(db))
        return JOB_FATAL;
    return stage_run_once(g_stage, db);
}

int tip_finalize_stage_drain(int max_steps)
{
    if (max_steps <= 0) return 0;
    int advanced = 0;
    for (int i = 0; i < max_steps; i++) {
        job_result_t r = tip_finalize_stage_step_once();
        if (r != JOB_ADVANCED) break;
        advanced++;
    }
    return advanced;
}

void tip_finalize_stage_shutdown(void)
{
    pthread_mutex_lock(&g_lock);
    if (g_stage) {
        stage_destroy(g_stage);
        g_stage = NULL;
    }
    g_ms = NULL;
    g_utxo_counter = NULL;
    g_utxo_counter_user = NULL;
    atomic_store(&g_finalized_total, (uint64_t)0);
    atomic_store(&g_upstream_failed_total, (uint64_t)0);
    atomic_store(&g_reorg_detected_total, (uint64_t)0);
    atomic_store(&g_utxo_count_diverged_total, (uint64_t)0);
    atomic_store(&g_precondition_failed_total, (uint64_t)0);
    atomic_store(&g_total_work_added_high, (uint64_t)0);
    atomic_store(&g_total_work_added_low, (uint64_t)0);
    atomic_store(&g_last_step_unix, (int64_t)0);
    atomic_store(&g_last_blocked_unix, (int64_t)0);
    atomic_store(&g_last_advance_height, (int64_t)-1);
    pthread_mutex_unlock(&g_lock);
}

void tip_finalize_stage_set_utxo_counter(tip_finalize_utxo_count_fn fn,
                                         void *user)
{
    pthread_mutex_lock(&g_lock);
    g_utxo_counter = fn;
    g_utxo_counter_user = user;
    pthread_mutex_unlock(&g_lock);
}

uint64_t tip_finalize_stage_cursor(void)
{
    return g_stage ? stage_cursor(g_stage) : 0;
}

uint64_t tip_finalize_stage_finalized_total(void)
{
    return atomic_load(&g_finalized_total);
}

uint64_t tip_finalize_stage_upstream_failed_total(void)
{
    return atomic_load(&g_upstream_failed_total);
}

uint64_t tip_finalize_stage_reorg_detected_total(void)
{
    return atomic_load(&g_reorg_detected_total);
}

uint64_t tip_finalize_stage_utxo_count_diverged_total(void)
{
    return atomic_load(&g_utxo_count_diverged_total);
}

uint64_t tip_finalize_stage_precondition_failed_total(void)
{
    return atomic_load(&g_precondition_failed_total);
}

uint64_t tip_finalize_stage_total_work_added_high(void)
{
    return atomic_load(&g_total_work_added_high);
}

uint64_t tip_finalize_stage_total_work_added_low(void)
{
    return atomic_load(&g_total_work_added_low);
}

bool tip_finalize_dump_state_json(struct json_value *out, const char *key)
{
    (void)key;
    if (!out) return false;
    json_set_object(out);

    sqlite3 *db = progress_store_db();
    int64_t now = wall_now_s();
    int64_t last = atomic_load(&g_last_step_unix);

    json_push_kv_bool(out, "initialised", g_stage != NULL);
    json_push_kv_str (out, "stage_name", STAGE_NAME);
    json_push_kv_int (out, "cursor",
                      (int64_t)(g_stage ? stage_cursor(g_stage) : 0));
    json_push_kv_int (out, "finalized_total",
                      (int64_t)atomic_load(&g_finalized_total));
    json_push_kv_int (out, "upstream_failed_total",
                      (int64_t)atomic_load(&g_upstream_failed_total));
    json_push_kv_int (out, "reorg_detected_total",
                      (int64_t)atomic_load(&g_reorg_detected_total));
    json_push_kv_int (out, "utxo_count_diverged_total",
                      (int64_t)atomic_load(&g_utxo_count_diverged_total));
    json_push_kv_int (out, "precondition_failed_total",
                      (int64_t)atomic_load(&g_precondition_failed_total));
    json_push_kv_int (out, "total_work_added_high",
                      (int64_t)atomic_load(&g_total_work_added_high));
    json_push_kv_int (out, "total_work_added_low",
                      (int64_t)atomic_load(&g_total_work_added_low));
    json_push_kv_int (out, "last_advance_height",
                      atomic_load(&g_last_advance_height));
    json_push_kv_int (out, "last_step_unix", last);
    json_push_kv_int (out, "last_step_age_seconds",
                      last > 0 ? now - last : -1);
    json_push_kv_int (out, "last_blocked_unix",
                      atomic_load(&g_last_blocked_unix));
    json_push_kv_int (out, "log_rows", db ? log_row_count(db) : 0);
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
