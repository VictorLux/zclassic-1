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
#include "platform/time_compat.h"
#include "services/header_admit_inbox.h"
#include "storage/progress_store.h"
#include "util/blocker.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"
#include "util/stage.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"

#include <pthread.h>
#include <stdatomic.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STAGE_NAME       "header_admit"

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct main_state *g_ms = NULL;
static stage_t *g_stage = NULL;
static _Atomic uint64_t g_admitted_total = 0;
static _Atomic uint64_t g_inbox_drained_total = 0;
static _Atomic uint64_t g_inbox_logged_total = 0;
static _Atomic int64_t  g_last_admit_height = -1;
static _Atomic int64_t  g_last_step_unix = 0;
static _Atomic int64_t  g_last_blocked_unix = 0;
static _Atomic header_admit_mode_t g_mode = HEADER_ADMIT_MODE_SHADOW;
#ifdef ZCL_TESTING
static header_admit_authoritative_hook g_authoritative_hook = NULL;
static void *g_authoritative_hook_user = NULL;
#endif

MAILBOX_DEFINE(header_admit, struct header_admit_msg,
               HEADER_ADMIT_INBOX_CAPACITY)

static int64_t wall_now_s(void)
{
    return platform_time_wall_unix();
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

static void handle_header_admit_msg(const struct header_admit_msg *m)
{
    if (!m) return;

    atomic_fetch_add(&g_inbox_drained_total, 1);

    struct main_state *ms = g_ms;
    sqlite3 *db = progress_store_db();
    if (!ms || !db || m->height < 0 || m->height > INT32_MAX)
        return;

    struct block_index *bi = active_chain_at(&ms->chain_active,
                                             (int)m->height);
    if (!bi || !bi->phashBlock)
        return;

    if (memcmp(bi->phashBlock->data, m->hash.data, 32) != 0) {
        fprintf(stderr,  // obs-ok:header-admit-inbox-hash-mismatch
                "[header_admit] inbox hash mismatch height=%lld peer=%u\n",
                (long long)m->height, m->peer_id);
        return;
    }

    const struct uint256 *parent_hash = NULL;
    if (m->height > 0) {
        if (!bi->pprev || !bi->pprev->phashBlock)
            return;
        parent_hash = bi->pprev->phashBlock;
    }

    if (log_insert(db, (int)m->height, bi->phashBlock, parent_hash))
        atomic_fetch_add(&g_inbox_logged_total, 1);
}

static bool authoritative_admit(struct main_state *ms, struct block_index *bi)
{
#ifdef ZCL_TESTING
    if (g_authoritative_hook)
        return g_authoritative_hook(ms, bi, g_authoritative_hook_user);
#endif

    if (!ms || !bi || !bi->phashBlock)
        return false;

    struct block_index *mapped =
        block_map_find(&ms->map_block_index, bi->phashBlock);
    if (mapped && mapped != bi)
        return false;

    if ((bi->nStatus & BLOCK_VALID_MASK) < BLOCK_VALID_TREE)
        bi->nStatus = (bi->nStatus & ~BLOCK_VALID_MASK) |
                      BLOCK_VALID_TREE;
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

    if (header_admit_get_mode() == HEADER_ADMIT_MODE_AUTHORITATIVE &&
        !authoritative_admit(ms, bi)) {
        fprintf(stderr,  // obs-ok:header-admit-authoritative-failure
                "[header_admit] authoritative admit failed height=%d\n",
                next_h);
        return STAGE_ERROR;
    }

    if (!log_insert(db, next_h, bi->phashBlock, parent_hash))
        return STAGE_ERROR;

    c->cursor_out = c->cursor_in + 1;
    atomic_fetch_add(&g_admitted_total, 1);
    atomic_store(&g_last_admit_height, (int64_t)next_h);
    return STAGE_ADVANCED;
}

/* ── Public API ────────────────────────────────────────────────────── */

void header_admit_set_mode(header_admit_mode_t mode)
{
    if (mode != HEADER_ADMIT_MODE_AUTHORITATIVE)
        mode = HEADER_ADMIT_MODE_SHADOW;
    atomic_store(&g_mode, mode);
}

header_admit_mode_t header_admit_get_mode(void)
{
    return atomic_load(&g_mode);
}

#ifdef ZCL_TESTING
void header_admit_stage_set_authoritative_hook(
    header_admit_authoritative_hook hook,
    void *user)
{
    g_authoritative_hook = hook;
    g_authoritative_hook_user = user;
}
#endif

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
    (void)mailbox_header_admit_drain(handle_header_admit_msg);
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
    atomic_store(&g_inbox_drained_total, (uint64_t)0);
    atomic_store(&g_inbox_logged_total, (uint64_t)0);
    atomic_store(&g_last_admit_height, (int64_t)-1);
    atomic_store(&g_last_step_unix, (int64_t)0);
    atomic_store(&g_last_blocked_unix, (int64_t)0);
#ifdef ZCL_TESTING
    g_authoritative_hook = NULL;
    g_authoritative_hook_user = NULL;
#endif
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

bool header_admit_stage_has_record(int32_t height,
                                   const struct uint256 *hash)
{
    if (height < 0 || !hash)
        return false;

    sqlite3 *db = progress_store_db();
    if (!db)
        return false;

    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(db,
        "SELECT hash FROM header_admit_log WHERE height=?",
        -1, &st, NULL);
    if (rc != SQLITE_OK)
        return false;

    sqlite3_bind_int(st, 1, height);
    bool found = false;
    if (sqlite3_step(st) == SQLITE_ROW) {  // raw-sql-ok:kernel-primitive
        const void *blob = sqlite3_column_blob(st, 0);
        int nb = sqlite3_column_bytes(st, 0);
        found = (blob && nb == 32 &&
                 memcmp(blob, hash->data, 32) == 0);
    }
    sqlite3_finalize(st);
    return found;
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
    json_push_kv_int (out, "inbox_drained_total",
                      (int64_t)atomic_load(&g_inbox_drained_total));
    json_push_kv_int (out, "inbox_logged_total",
                      (int64_t)atomic_load(&g_inbox_logged_total));
    json_push_kv_int (out, "last_admit_height",
                      atomic_load(&g_last_admit_height));
    json_push_kv_int (out, "last_step_unix",
                      atomic_load(&g_last_step_unix));
    json_push_kv_int (out, "last_blocked_unix",
                      atomic_load(&g_last_blocked_unix));
    json_push_kv_str (out, "mode",
                      header_admit_get_mode() ==
                          HEADER_ADMIT_MODE_AUTHORITATIVE
                              ? "authoritative"
                              : "shadow");
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

/* ── S-11 mini-diff harness ─────────────────────────────────────────── */

/* SELECT MAX(height) FROM header_admit_log. Returns -1 if empty. */
static int32_t log_max_height(sqlite3 *db)
{
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(db,
        "SELECT MAX(height) FROM header_admit_log", -1, &st, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[header_admit] diff: prepare MAX(height) failed: %s\n", sqlite3_errmsg(db));  // obs-ok:header-admit-diff-prepare-failure
        return -1;  /* raw-return-ok:diagnostic-treats-as-empty */
    }
    int32_t out = -1;
    if (sqlite3_step(st) == SQLITE_ROW &&  // raw-sql-ok:kernel-primitive
        sqlite3_column_type(st, 0) != SQLITE_NULL) {
        out = (int32_t)sqlite3_column_int(st, 0);
    }
    sqlite3_finalize(st);
    return out;
}

static void diff_sample_record(struct header_admit_diff_report *r,
                                int32_t h,
                                const uint8_t *log_hash, bool log_present,
                                const uint8_t *chain_hash, bool chain_present)
{
    if (r->sample_count >= HEADER_ADMIT_DIFF_MAX_SAMPLES) return;
    struct header_admit_diff_sample *s = &r->samples[r->sample_count++];
    s->height        = h;
    s->log_present   = log_present;
    s->chain_present = chain_present;
    if (log_present && log_hash)   memcpy(s->log_hash,   log_hash,   32);
    else                           memset(s->log_hash,   0, 32);
    if (chain_present && chain_hash) memcpy(s->chain_hash, chain_hash, 32);
    else                             memset(s->chain_hash, 0, 32);
}

bool header_admit_stage_diff(int32_t start_h, int32_t end_h,
                              struct header_admit_diff_report *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    out->first_divergent_height = -1;
    out->log_max_height         = -1;
    out->chain_tip_height       = -1;

    /* Not-ready guard: stage uninit or progress.kv closed. The report
     * is still populated with sensible defaults so callers can render. */
    struct main_state *ms = g_ms;
    sqlite3 *db = progress_store_db();
    if (!g_stage || !ms || !db) {
        out->status = HEADER_ADMIT_DIFF_NOT_READY;
        out->start_height = (start_h < 0) ? 0 : start_h;
        out->end_height   = (end_h   < 0) ? 0 : end_h;
        return true;
    }

    out->cursor             = (int32_t)stage_cursor(g_stage);
    out->log_max_height     = log_max_height(db);
    out->chain_tip_height   = active_chain_height(&ms->chain_active);

    /* Resolve auto-bounds. */
    int32_t s = (start_h < 0) ? 0 : start_h;
    int32_t e = end_h;
    if (e < 0) {
        int32_t a = out->log_max_height;
        int32_t b = out->chain_tip_height;
        if (a < 0 && b < 0)      e = -1;
        else if (a < 0)          e = b;
        else if (b < 0)          e = a;
        else                     e = (a < b) ? a : b;
    }

    if (e < s) {
        out->status       = HEADER_ADMIT_DIFF_EMPTY;
        out->start_height = s;
        out->end_height   = e;
        return true;
    }

    /* Hard cap the range. */
    int64_t span = (int64_t)e - (int64_t)s + 1;
    if (span > HEADER_ADMIT_DIFF_MAX_RANGE) {
        e = s + HEADER_ADMIT_DIFF_MAX_RANGE - 1;
        span = HEADER_ADMIT_DIFF_MAX_RANGE;
    }
    out->start_height = s;
    out->end_height   = e;

    /* Load all log rows in the range into a packed array indexed by
     * (height - s). Bounded: span <= HEADER_ADMIT_DIFF_MAX_RANGE → ≤ 330 KB. */
    struct row {
        uint8_t hash[32];
        bool    present;
    };
    struct row *rows = zcl_calloc((size_t)span, sizeof(struct row),
                                   "header_admit_diff_rows");
    if (!rows) {
        out->status = HEADER_ADMIT_DIFF_NOT_READY;
        return true;
    }

    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(db,
        "SELECT height, hash FROM header_admit_log "
        "WHERE height BETWEEN ? AND ? ORDER BY height",
        -1, &st, NULL);
    if (rc != SQLITE_OK) {
        free(rows);
        out->status = HEADER_ADMIT_DIFF_NOT_READY;
        return true;
    }
    sqlite3_bind_int(st, 1, s);
    sqlite3_bind_int(st, 2, e);
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {  // raw-sql-ok:kernel-primitive
        int32_t h = sqlite3_column_int(st, 0);
        const void *blob = sqlite3_column_blob(st, 1);
        int nb = sqlite3_column_bytes(st, 1);
        if (h < s || h > e || !blob || nb != 32) continue;
        struct row *r = &rows[h - s];
        memcpy(r->hash, blob, 32);
        r->present = true;
    }
    sqlite3_finalize(st);

    /* Walk the range. For each height, compare log row vs in-memory chain. */
    for (int32_t h = s; h <= e; h++) {
        const struct row *r = &rows[h - s];
        struct block_index *bi = active_chain_at(&ms->chain_active, h);
        bool chain_present = (bi != NULL && bi->phashBlock != NULL);
        const uint8_t *chain_hash =
            chain_present ? bi->phashBlock->data : NULL;

        if (!r->present && !chain_present) continue;  /* both missing → skip */

        out->checked_count++;
        if (r->present && chain_present) {
            if (memcmp(r->hash, chain_hash, 32) == 0) {
                out->match_count++;
            } else {
                out->mismatch_count++;
                if (out->first_divergent_height < 0)
                    out->first_divergent_height = h;
                diff_sample_record(out, h, r->hash, true, chain_hash, true);
            }
        } else if (r->present && !chain_present) {
            /* log has it, chain doesn't — chain shrank or reorged through. */
            out->missing_in_chain_count++;
            if (out->first_divergent_height < 0)
                out->first_divergent_height = h;
            diff_sample_record(out, h, r->hash, true, NULL, false);
        } else {
            /* chain has it, log doesn't — S-2 cursor lag. */
            out->missing_in_log_count++;
            if (out->first_divergent_height < 0)
                out->first_divergent_height = h;
            diff_sample_record(out, h, NULL, false, chain_hash, true);
        }
    }

    free(rows);

    /* Status: hash mismatches dominate; reorg-style log-ahead next;
     * normal cursor-lag third; otherwise converged or empty. */
    if (out->mismatch_count > 0)             out->status = HEADER_ADMIT_DIFF_DIVERGENT;
    else if (out->missing_in_chain_count > 0) out->status = HEADER_ADMIT_DIFF_LOG_AHEAD;
    else if (out->missing_in_log_count > 0)   out->status = HEADER_ADMIT_DIFF_CHAIN_AHEAD;
    else if (out->checked_count > 0)          out->status = HEADER_ADMIT_DIFF_CONVERGED;
    else                                       out->status = HEADER_ADMIT_DIFF_EMPTY;

    return true;
}
