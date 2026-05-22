/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * header_admit_stage — implementation. See services/header_admit_stage.h.
 *
 * Single-process singleton. The F-2 stage primitive does all the
 * cursor / replay heavy lifting; this module is just the step body and
 * the schema-bootstrap glue for the `header_admit_log` table that lives
 * in progress.kv alongside `stage_cursor`. */

#include "services/header_admit_stage.h"

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
#include <string.h>
#include <time.h>

#define STAGE_NAME       "header_admit"

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct main_state *g_ms = NULL;
static stage_t *g_stage = NULL;
static _Atomic uint64_t g_admitted_total = 0;
static _Atomic int64_t  g_last_admit_height = -1;
static _Atomic int64_t  g_last_step_unix = 0;
static _Atomic int64_t  g_last_blocked_unix = 0;

static int64_t wall_now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec;
}

/* ── Schema bootstrap (idempotent) ─────────────────────────────────── */

static bool ensure_log_schema(sqlite3 *db)
{
    static const char *const sql =
        "CREATE TABLE IF NOT EXISTS header_admit_log ("
        "  height      INTEGER PRIMARY KEY,"
        "  hash        BLOB    NOT NULL,"
        "  parent_hash BLOB,"
        "  admitted_at INTEGER NOT NULL"
        ")";
    char *err = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr,  // obs-ok:header-admit-schema-failure
                "[header_admit] schema ensure failed: %s\n",
                err ? err : "(no message)");
        if (err) sqlite3_free(err);
        return false;
    }
    return true;
}

/* ── Step body ─────────────────────────────────────────────────────── */

static bool log_insert(sqlite3 *db, int height,
                        const struct uint256 *hash,
                        const struct uint256 *parent_hash)
{
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
        "INSERT OR REPLACE INTO header_admit_log "
        "(height, hash, parent_hash, admitted_at) VALUES (?,?,?,?)",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr,  // obs-ok:header-admit-log-prepare-failure
                "[header_admit] prepare insert failed: %s\n",
                sqlite3_errmsg(db));
        return false;
    }
    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)height);
    sqlite3_bind_blob (stmt, 2, hash->data, 32, SQLITE_STATIC);
    if (parent_hash)
        sqlite3_bind_blob(stmt, 3, parent_hash->data, 32, SQLITE_STATIC);
    else
        sqlite3_bind_null(stmt, 3);
    sqlite3_bind_int64(stmt, 4, (sqlite3_int64)wall_now_s());

    rc = sqlite3_step(stmt);  // raw-sql-ok:kernel-primitive
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        fprintf(stderr,  // obs-ok:header-admit-log-insert-failure
                "[header_admit] insert height=%d rc=%d\n", height, rc);
        return false;
    }
    return true;
}

static stage_result_t step_admit(struct stage_step_ctx *c)
{
    atomic_store(&g_last_step_unix, wall_now_s());

    struct main_state *ms = g_ms;
    if (!ms) return STAGE_IDLE;

    sqlite3 *db = progress_store_db();
    if (!db) return STAGE_IDLE;

    int next_h = (int)c->cursor_in;
    if (next_h < 0) return STAGE_ERROR;

    struct block_index *bi = active_chain_at(&ms->chain_active, next_h);
    if (!bi || !bi->phashBlock) return STAGE_IDLE;

    const struct uint256 *parent_hash = NULL;
    if (next_h > 0) {
        if (!bi->pprev || !bi->pprev->phashBlock) {
            blocker_init(&c->blocker, "header_admit",
                          "missing_parent",
                          BLOCKER_PERMANENT,
                          "block_index entry has no pprev linkage");
            atomic_store(&g_last_blocked_unix, wall_now_s());
            return STAGE_BLOCKED;
        }
        parent_hash = bi->pprev->phashBlock;
    }

    if (!log_insert(db, next_h, bi->phashBlock, parent_hash))
        return STAGE_ERROR;

    c->cursor_out = c->cursor_in + 1;
    atomic_fetch_add(&g_admitted_total, 1);
    atomic_store(&g_last_admit_height, (int64_t)next_h);
    return STAGE_ADVANCED;
}

/* ── Public API ────────────────────────────────────────────────────── */

bool header_admit_stage_init(struct main_state *ms)
{
    if (!ms) LOG_FAIL("header_admit", "init: NULL main_state");

    sqlite3 *db = progress_store_db();
    if (!db) LOG_FAIL("header_admit",
        "init: progress_store not open");

    pthread_mutex_lock(&g_lock);

    /* Idempotent: same ms, already initialised → success. */
    if (g_stage != NULL) {
        bool same = (g_ms == ms);
        pthread_mutex_unlock(&g_lock);
        if (!same)
            LOG_FAIL("header_admit",
                "init: already bound to a different main_state");
        return true;
    }

    if (!ensure_log_schema(db)) {
        pthread_mutex_unlock(&g_lock);
        return false;
    }

    stage_t *s = stage_create(STAGE_NAME, step_admit, NULL);
    if (!s) {
        pthread_mutex_unlock(&g_lock);
        LOG_FAIL("header_admit", "init: stage_create failed");
    }

    g_ms = ms;
    g_stage = s;
    pthread_mutex_unlock(&g_lock);

    fprintf(stderr,  // obs-ok:header-admit-lifecycle
            "[header_admit] stage initialised (shadow mode)\n");
    return true;
}

stage_result_t header_admit_stage_step_once(void)
{
    if (!g_stage) return STAGE_IDLE;
    sqlite3 *db = progress_store_db();
    if (!db) return STAGE_IDLE;
    return stage_run_once(g_stage, db);
}

int header_admit_stage_drain(int max_steps)
{
    if (max_steps <= 0) return 0;
    int advanced = 0;
    for (int i = 0; i < max_steps; i++) {
        stage_result_t r = header_admit_stage_step_once();
        if (r != STAGE_ADVANCED) break;
        advanced++;
    }
    return advanced;
}

void header_admit_stage_shutdown(void)
{
    pthread_mutex_lock(&g_lock);
    if (g_stage) {
        stage_destroy(g_stage);
        g_stage = NULL;
    }
    g_ms = NULL;
    /* Reset per-init observability state. The persisted cursor in
     * progress.kv is preserved (that's the whole point of the saga);
     * what we reset is what would be misleading across re-inits —
     * lifetime counters that should restart with each bind. */
    atomic_store(&g_admitted_total, (uint64_t)0);
    atomic_store(&g_last_admit_height, (int64_t)-1);
    atomic_store(&g_last_step_unix, (int64_t)0);
    atomic_store(&g_last_blocked_unix, (int64_t)0);
    pthread_mutex_unlock(&g_lock);
}

uint64_t header_admit_stage_cursor(void)
{
    return g_stage ? stage_cursor(g_stage) : 0;
}

uint64_t header_admit_stage_admitted_total(void)
{
    return atomic_load(&g_admitted_total);
}

bool header_admit_stage_dump_state_json(struct json_value *out,
                                         const char *key)
{
    (void)key;
    if (!out) return false;
    json_set_object(out);

    json_push_kv_bool(out, "initialised", g_stage != NULL);
    json_push_kv_str (out, "stage_name", STAGE_NAME);
    json_push_kv_int (out, "cursor",
                      (int64_t)(g_stage ? stage_cursor(g_stage) : 0));
    json_push_kv_int (out, "admitted_total",
                      (int64_t)atomic_load(&g_admitted_total));
    json_push_kv_int (out, "last_admit_height",
                      atomic_load(&g_last_admit_height));
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
