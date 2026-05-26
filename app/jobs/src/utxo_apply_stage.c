/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * utxo_apply_stage — implementation. See jobs/utxo_apply_stage.h.
 *
 * S-8 consumes proof_validate_log and computes a transparent UTXO delta.
 * It writes only utxo_apply_log plus its stage cursor in progress.kv. */

#include "platform/time_compat.h"
#include "jobs/utxo_apply_stage.h"

#include "chain/chain.h"
#include "core/uint256.h"
#include "event/event.h"
#include "json/json.h"
#include "primitives/block.h"
#include "primitives/transaction.h"
#include "storage/disk_block_io.h"
#include "storage/progress_store.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"
#include "util/stage.h"
#include "util/util.h"
#include "validation/main_state.h"

#include <pthread.h>
#include <sqlite3.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STAGE_NAME "utxo_apply"
#define MAX_MONEY_ZAT 2100000000000000LL

struct proof_validate_row {
    int ok;
};

struct delta_entry {
    struct uint256 txid;
    uint32_t vout;
    int64_t value;
};

struct delta_summary {
    bool ok;
    const char *status;
    const char *failure_kind;
    uint8_t failure_detail[36];
    size_t spent_count;
    size_t added_count;
    int64_t total_value_delta;
};

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct main_state *g_ms = NULL;
static stage_t *g_stage = NULL;
static char g_datadir[2048] = {0};
static utxo_apply_reader_fn g_reader = NULL;
static void *g_reader_user = NULL;
static utxo_apply_lookup_fn g_lookup = NULL;
static void *g_lookup_user = NULL;
static utxo_apply_live_check_fn g_live_check = NULL;
static void *g_live_check_user = NULL;

static _Atomic uint64_t g_verified_total = 0;
static _Atomic uint64_t g_spend_unknown_total = 0;
static _Atomic uint64_t g_utxo_collision_total = 0;
static _Atomic uint64_t g_value_overflow_total = 0;
static _Atomic uint64_t g_delta_diverged_total = 0;
static _Atomic uint64_t g_upstream_failed_total = 0;
static _Atomic uint64_t g_internal_error_total = 0;
static _Atomic uint64_t g_total_outputs_added = 0;
static _Atomic uint64_t g_total_outputs_spent = 0;
static _Atomic int64_t  g_last_step_unix = 0;
static _Atomic int64_t  g_last_blocked_unix = 0;
static _Atomic int64_t  g_last_advance_height = -1;

static int64_t wall_now_s(void)
{
    return (int64_t)platform_time_wall_time_t();
}

static bool default_reader(struct block *out, const struct block_index *bi,
                           const char *datadir, void *user)
{
    (void)user;
    if (!out || !bi || !(bi->nStatus & BLOCK_HAVE_DATA))
        return false;

    struct disk_block_pos pos;
    disk_block_pos_init(&pos);
    pos.nFile = bi->nFile;
    pos.nPos = bi->nDataPos;
    return read_block_from_disk_pread(out, &pos, datadir ? datadir : "");
}

static void failure_detail_set(uint8_t out[36], const struct uint256 *txid,
                               uint32_t vout)
{
    memset(out, 0, 36);
    if (txid) memcpy(out, txid->data, 32);
    out[32] = (uint8_t)(vout & 0xff);
    out[33] = (uint8_t)((vout >> 8) & 0xff);
    out[34] = (uint8_t)((vout >> 16) & 0xff);
    out[35] = (uint8_t)((vout >> 24) & 0xff);
}

static bool ensure_log_schema(sqlite3 *db)
{
    static const char *const sql =
        "CREATE TABLE IF NOT EXISTS utxo_apply_log ("
        "  height               INTEGER PRIMARY KEY,"
        "  status               TEXT    NOT NULL,"
        "  ok                   INTEGER NOT NULL,"
        "  spent_count          INTEGER NOT NULL,"
        "  added_count          INTEGER NOT NULL,"
        "  total_value_delta    INTEGER NOT NULL,"
        "  first_failure_kind   TEXT,"
        "  first_failure_detail BLOB,"
        "  applied_at           INTEGER NOT NULL"
        ")";
    char *err = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr,  // obs-ok:utxo-apply-schema-failure
                "[utxo_apply] schema ensure failed: %s\n",
                err ? err : "(no message)");
        if (err) sqlite3_free(err);
        return false;
    }
    return true;
}

static uint64_t upstream_cursor_persisted(sqlite3 *db, const char *name)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
        "SELECT cursor FROM stage_cursor WHERE name = ?",
        -1, &st, NULL) != SQLITE_OK) {
        fprintf(stderr,  // obs-ok:utxo-apply-upstream-prepare-failure
                "[utxo_apply] upstream cursor prepare failed: %s\n",
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

static int proof_validate_log_at(sqlite3 *db, int height,
                                 struct proof_validate_row *out)
{
    memset(out, 0, sizeof(*out));
    out->ok = -1;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
        "SELECT ok FROM proof_validate_log WHERE height = ?",
        -1, &st, NULL) != SQLITE_OK) {
        fprintf(stderr,  // obs-ok:utxo-apply-proof-log-prepare-failure
                "[utxo_apply] proof_validate_log prepare failed: %s\n",
                sqlite3_errmsg(db));
        return -1;  // raw-return-ok:logged-above
    }
    sqlite3_bind_int(st, 1, height);
    int found = 0;
    int rc = sqlite3_step(st);  // raw-sql-ok:kernel-primitive
    if (rc == SQLITE_ROW) {
        out->ok = sqlite3_column_int(st, 0);
        found = 1;
    }
    sqlite3_finalize(st);
    return found;
}

static bool log_insert(sqlite3 *db, int height, const char *status, bool ok,
                       size_t spent_count, size_t added_count,
                       int64_t total_value_delta,
                       const char *failure_kind,
                       const uint8_t failure_detail[36])
{
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
        "INSERT OR REPLACE INTO utxo_apply_log "
        "(height, status, ok, spent_count, added_count, total_value_delta, "
        " first_failure_kind, first_failure_detail, applied_at) "
        "VALUES (?,?,?,?,?,?,?,?,?)",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr,  // obs-ok:utxo-apply-log-prepare-failure
                "[utxo_apply] prepare insert failed: %s\n",
                sqlite3_errmsg(db));
        return false;
    }
    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)height);
    sqlite3_bind_text (stmt, 2, status, -1, SQLITE_STATIC);
    sqlite3_bind_int  (stmt, 3, ok ? 1 : 0);
    sqlite3_bind_int64(stmt, 4, (sqlite3_int64)spent_count);
    sqlite3_bind_int64(stmt, 5, (sqlite3_int64)added_count);
    sqlite3_bind_int64(stmt, 6, (sqlite3_int64)total_value_delta);
    if (failure_kind)
        sqlite3_bind_text(stmt, 7, failure_kind, -1, SQLITE_STATIC);
    else
        sqlite3_bind_null(stmt, 7);
    if (failure_detail)
        sqlite3_bind_blob(stmt, 8, failure_detail, 36, SQLITE_STATIC);
    else
        sqlite3_bind_null(stmt, 8);
    sqlite3_bind_int64(stmt, 9, (sqlite3_int64)wall_now_s());
    rc = sqlite3_step(stmt);  // raw-sql-ok:kernel-primitive
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        fprintf(stderr,  // obs-ok:utxo-apply-log-insert-failure
                "[utxo_apply] insert height=%d rc=%d\n", height, rc);
        return false;
    }
    return true;
}

static int64_t log_row_count(sqlite3 *db)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
        "SELECT COUNT(*) FROM utxo_apply_log",
        -1, &st, NULL) != SQLITE_OK) {
        fprintf(stderr,  // obs-ok:utxo-apply-count-prepare-failure
                "[utxo_apply] log count prepare failed: %s\n",
                sqlite3_errmsg(db));
        return -1;  // raw-return-ok:logged-above
    }
    int64_t n = -1;
    if (sqlite3_step(st) == SQLITE_ROW)  // raw-sql-ok:kernel-primitive
        n = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return n;
}

static bool lookup_added(const struct delta_entry *added, size_t n,
                         const struct uint256 *txid, uint32_t vout,
                         int64_t *out_value)
{
    for (size_t i = 0; i < n; i++) {
        if (added[i].vout == vout && uint256_eq(&added[i].txid, txid)) {
            if (out_value) *out_value = added[i].value;
            return true;
        }
    }
    return false;
}

static bool external_lookup(const struct uint256 *txid, uint32_t vout,
                            struct utxo_apply_lookup *out)
{
    memset(out, 0, sizeof(*out));
    if (!g_lookup)
        return true;
    return g_lookup(txid, vout, out, g_lookup_user);
}

static void delta_summary_init(struct delta_summary *s)
{
    memset(s, 0, sizeof(*s));
    s->ok = true;
    s->status = "verified";
}

static void delta_fail(struct delta_summary *s, const char *status,
                       const char *kind, const struct uint256 *txid,
                       uint32_t vout)
{
    s->ok = false;
    s->status = status;
    s->failure_kind = kind;
    failure_detail_set(s->failure_detail, txid, vout);
}

static void free_delta(struct delta_entry *spent, struct delta_entry *added)
{
    free(spent);
    free(added);
}

static void compute_block_delta(const struct block *blk,
                                struct delta_summary *out)
{
    delta_summary_init(out);
    if (!blk) {
        out->ok = false;
        out->status = "internal_error";
        out->failure_kind = "missing_block";
        return;
    }

    size_t spend_cap = 0, add_cap = 0;
    for (size_t ti = 0; ti < blk->num_vtx; ti++) {
        const struct transaction *tx = &blk->vtx[ti];
        if (!transaction_is_coinbase(tx))
            spend_cap += tx->num_vin;
        add_cap += tx->num_vout;
    }

    struct delta_entry *spent = spend_cap
        ? zcl_calloc(spend_cap, sizeof(*spent), "utxo_apply_spent")
        : NULL;
    struct delta_entry *added = add_cap
        ? zcl_calloc(add_cap, sizeof(*added), "utxo_apply_added")
        : NULL;
    if ((spend_cap && !spent) || (add_cap && !added)) {
        out->ok = false;
        out->status = "internal_error";
        out->failure_kind = "alloc";
        free_delta(spent, added);
        return;
    }

    for (size_t ti = 0; ti < blk->num_vtx; ti++) {
        const struct transaction *tx = &blk->vtx[ti];
        int64_t tx_input_value = 0;
        int64_t tx_output_value = 0;

        if (!transaction_is_coinbase(tx)) {
            for (size_t vi = 0; vi < tx->num_vin; vi++) {
                const struct outpoint *op = &tx->vin[vi].prevout;
                int64_t value = 0;
                bool found = lookup_added(added, out->added_count,
                                          &op->hash, op->n, &value);
                if (!found) {
                    struct utxo_apply_lookup lk;
                    if (!external_lookup(&op->hash, op->n, &lk)) {
                        out->ok = false;
                        out->status = "internal_error";
                        out->failure_kind = "lookup";
                        free_delta(spent, added);
                        return;
                    }
                    found = lk.found;
                    value = lk.value;
                }
                if (!found) {
                    delta_fail(out, "spend_unknown_utxo",
                               "spend_unknown_utxo", &op->hash, op->n);
                    free_delta(spent, added);
                    return;
                }
                if (value < 0 || value > MAX_MONEY_ZAT) {
                    delta_fail(out, "value_overflow",
                               "input_value", &op->hash, op->n);
                    free_delta(spent, added);
                    return;
                }
                spent[out->spent_count].txid = op->hash;
                spent[out->spent_count].vout = op->n;
                spent[out->spent_count].value = value;
                out->spent_count++;
                out->total_value_delta -= value;
                tx_input_value += value;
            }
        }

        for (size_t vo = 0; vo < tx->num_vout; vo++) {
            const struct tx_out *txo = &tx->vout[vo];
            if (tx_out_is_null(txo))
                continue;
            if (txo->value < 0 || txo->value > MAX_MONEY_ZAT) {
                delta_fail(out, "value_overflow", "output_value",
                           &tx->hash, (uint32_t)vo);
                free_delta(spent, added);
                return;
            }
            if (lookup_added(added, out->added_count, &tx->hash,
                             (uint32_t)vo, NULL)) {
                delta_fail(out, "utxo_collision", "duplicate_output",
                           &tx->hash, (uint32_t)vo);
                free_delta(spent, added);
                return;
            }
            struct utxo_apply_lookup lk;
            if (!external_lookup(&tx->hash, (uint32_t)vo, &lk)) {
                out->ok = false;
                out->status = "internal_error";
                out->failure_kind = "lookup";
                free_delta(spent, added);
                return;
            }
            if (lk.found) {
                delta_fail(out, "utxo_collision", "utxo_collision",
                           &tx->hash, (uint32_t)vo);
                free_delta(spent, added);
                return;
            }
            added[out->added_count].txid = tx->hash;
            added[out->added_count].vout = (uint32_t)vo;
            added[out->added_count].value = txo->value;
            out->added_count++;
            out->total_value_delta += txo->value;
            tx_output_value += txo->value;
        }

        if (!transaction_is_coinbase(tx) && tx_output_value > tx_input_value) {
            delta_fail(out, "value_overflow", "outputs_exceed_inputs",
                       &tx->hash, 0);
            free_delta(spent, added);
            return;
        }
    }

    if (out->total_value_delta > MAX_MONEY_ZAT ||
        out->total_value_delta < -MAX_MONEY_ZAT) {
        out->ok = false;
        out->status = "value_overflow";
        out->failure_kind = "total_value_delta";
    }

    free_delta(spent, added);
}

static job_result_t step_apply(struct stage_step_ctx *c)
{
    atomic_store(&g_last_step_unix, wall_now_s());

    struct main_state *ms = g_ms;
    if (!ms) return JOB_IDLE;
    sqlite3 *db = progress_store_db();
    if (!db) return JOB_IDLE;

    int next_h = (int)c->cursor_in;
    if (next_h < 0) return JOB_FATAL;

    uint64_t pv_cursor = upstream_cursor_persisted(db, "proof_validate");
    if ((uint64_t)next_h >= pv_cursor) {
        atomic_store(&g_last_blocked_unix, wall_now_s());
        return JOB_IDLE;
    }

    struct proof_validate_row upstream;
    int found = proof_validate_log_at(db, next_h, &upstream);
    if (found < 0) return JOB_FATAL;
    if (found == 0) {
        atomic_store(&g_last_blocked_unix, wall_now_s());
        return JOB_IDLE;
    }

    if (upstream.ok == 0) {
        if (!log_insert(db, next_h, "upstream_failed", false, 0, 0, 0,
                        NULL, NULL))
            return JOB_FATAL;
        atomic_fetch_add(&g_upstream_failed_total, 1);
        atomic_store(&g_last_advance_height, (int64_t)next_h);
        c->cursor_out = c->cursor_in + 1;
        return JOB_ADVANCED;
    }

    struct block_index *bi = active_chain_at(&ms->chain_active, next_h);
    if (!bi || !(bi->nStatus & BLOCK_HAVE_DATA)) {
        atomic_store(&g_last_blocked_unix, wall_now_s());
        return JOB_IDLE;
    }

    struct block blk;
    block_init(&blk);
    utxo_apply_reader_fn reader = g_reader ? g_reader : default_reader;
    if (!reader(&blk, bi, g_datadir, g_reader_user)) {
        block_free(&blk);
        atomic_store(&g_last_blocked_unix, wall_now_s());
        return JOB_IDLE;
    }

    struct delta_summary summary;
    compute_block_delta(&blk, &summary);
    block_free(&blk);

    if (summary.ok && g_live_check) {
        const char *detail = NULL;
        if (!g_live_check(next_h, summary.spent_count, summary.added_count,
                          summary.total_value_delta, &detail,
                          g_live_check_user)) {
            summary.ok = false;
            summary.status = "delta_diverged";
            summary.failure_kind = detail ? detail : "live_delta";
            memset(summary.failure_detail, 0, sizeof(summary.failure_detail));
        }
    }

    if (summary.ok) {
        atomic_fetch_add(&g_verified_total, 1);
        atomic_fetch_add(&g_total_outputs_added,
                         (uint64_t)summary.added_count);
        atomic_fetch_add(&g_total_outputs_spent,
                         (uint64_t)summary.spent_count);
    } else if (strcmp(summary.status, "spend_unknown_utxo") == 0) {
        atomic_fetch_add(&g_spend_unknown_total, 1);
        event_emitf(EV_BLOCK_REJECTED, 0,
                    "utxo_apply spend_unknown_utxo height=%d", next_h);
    } else if (strcmp(summary.status, "utxo_collision") == 0) {
        atomic_fetch_add(&g_utxo_collision_total, 1);
        event_emitf(EV_BLOCK_REJECTED, 0,
                    "utxo_apply utxo_collision height=%d", next_h);
    } else if (strcmp(summary.status, "value_overflow") == 0) {
        atomic_fetch_add(&g_value_overflow_total, 1);
        event_emitf(EV_BLOCK_REJECTED, 0,
                    "utxo_apply value_overflow height=%d", next_h);
    } else if (strcmp(summary.status, "delta_diverged") == 0) {
        atomic_fetch_add(&g_delta_diverged_total, 1);
        event_emitf(EV_BLOCK_REJECTED, 0,
                    "utxo_apply delta_diverged height=%d", next_h);
    } else {
        atomic_fetch_add(&g_internal_error_total, 1);
        event_emitf(EV_BLOCK_REJECTED, 0,
                    "utxo_apply internal_error height=%d", next_h);
    }

    if (!log_insert(db, next_h, summary.status, summary.ok,
                    summary.spent_count, summary.added_count,
                    summary.total_value_delta, summary.failure_kind,
                    summary.ok ? NULL : summary.failure_detail))
        return JOB_FATAL;

    atomic_store(&g_last_advance_height, (int64_t)next_h);
    c->cursor_out = c->cursor_in + 1;
    return JOB_ADVANCED;
}

bool utxo_apply_stage_init(struct main_state *ms)
{
    if (!ms) LOG_FAIL("utxo_apply", "init: NULL main_state");

    sqlite3 *db = progress_store_db();
    if (!db) LOG_FAIL("utxo_apply", "init: progress_store not open");

    pthread_mutex_lock(&g_lock);
    if (g_stage != NULL) {
        bool same = (g_ms == ms);
        pthread_mutex_unlock(&g_lock);
        if (!same)
            LOG_FAIL("utxo_apply",
                "init: already bound to a different main_state");
        return true;
    }

    if (!ensure_log_schema(db)) {
        pthread_mutex_unlock(&g_lock);
        return false;
    }

    GetDataDir(true, g_datadir, sizeof(g_datadir));

    stage_t *s = stage_create(STAGE_NAME, step_apply, NULL);
    if (!s) {
        pthread_mutex_unlock(&g_lock);
        LOG_FAIL("utxo_apply", "init: stage_create failed");
    }

    g_ms = ms;
    g_stage = s;
    pthread_mutex_unlock(&g_lock);

    fprintf(stderr,  // obs-ok:utxo-apply-lifecycle
            "[utxo_apply] stage initialised (shadow mode)\n");
    return true;
}

job_result_t utxo_apply_stage_step_once(void)
{
    if (!g_stage) return JOB_IDLE;
    sqlite3 *db = progress_store_db();
    if (!db) return JOB_IDLE;
    return stage_run_once(g_stage, db);
}

int utxo_apply_stage_drain(int max_steps)
{
    if (max_steps <= 0) return 0;
    int advanced = 0;
    for (int i = 0; i < max_steps; i++) {
        job_result_t r = utxo_apply_stage_step_once();
        if (r != JOB_ADVANCED) break;
        advanced++;
    }
    return advanced;
}

void utxo_apply_stage_shutdown(void)
{
    pthread_mutex_lock(&g_lock);
    if (g_stage) {
        stage_destroy(g_stage);
        g_stage = NULL;
    }
    g_ms = NULL;
    g_datadir[0] = '\0';
    g_reader = NULL;
    g_reader_user = NULL;
    g_lookup = NULL;
    g_lookup_user = NULL;
    g_live_check = NULL;
    g_live_check_user = NULL;
    atomic_store(&g_verified_total, (uint64_t)0);
    atomic_store(&g_spend_unknown_total, (uint64_t)0);
    atomic_store(&g_utxo_collision_total, (uint64_t)0);
    atomic_store(&g_value_overflow_total, (uint64_t)0);
    atomic_store(&g_delta_diverged_total, (uint64_t)0);
    atomic_store(&g_upstream_failed_total, (uint64_t)0);
    atomic_store(&g_internal_error_total, (uint64_t)0);
    atomic_store(&g_total_outputs_added, (uint64_t)0);
    atomic_store(&g_total_outputs_spent, (uint64_t)0);
    atomic_store(&g_last_step_unix, (int64_t)0);
    atomic_store(&g_last_blocked_unix, (int64_t)0);
    atomic_store(&g_last_advance_height, (int64_t)-1);
    pthread_mutex_unlock(&g_lock);
}

void utxo_apply_stage_set_reader(utxo_apply_reader_fn fn, void *user)
{
    pthread_mutex_lock(&g_lock);
    g_reader = fn;
    g_reader_user = user;
    pthread_mutex_unlock(&g_lock);
}

void utxo_apply_stage_set_lookup(utxo_apply_lookup_fn fn, void *user)
{
    pthread_mutex_lock(&g_lock);
    g_lookup = fn;
    g_lookup_user = user;
    pthread_mutex_unlock(&g_lock);
}

void utxo_apply_stage_set_live_checker(utxo_apply_live_check_fn fn, void *user)
{
    pthread_mutex_lock(&g_lock);
    g_live_check = fn;
    g_live_check_user = user;
    pthread_mutex_unlock(&g_lock);
}

uint64_t utxo_apply_stage_cursor(void)
{
    return g_stage ? stage_cursor(g_stage) : 0;
}

uint64_t utxo_apply_stage_verified_total(void)
{
    return atomic_load(&g_verified_total);
}

uint64_t utxo_apply_stage_spend_unknown_total(void)
{
    return atomic_load(&g_spend_unknown_total);
}

uint64_t utxo_apply_stage_utxo_collision_total(void)
{
    return atomic_load(&g_utxo_collision_total);
}

uint64_t utxo_apply_stage_value_overflow_total(void)
{
    return atomic_load(&g_value_overflow_total);
}

uint64_t utxo_apply_stage_delta_diverged_total(void)
{
    return atomic_load(&g_delta_diverged_total);
}

uint64_t utxo_apply_stage_upstream_failed_total(void)
{
    return atomic_load(&g_upstream_failed_total);
}

uint64_t utxo_apply_stage_internal_error_total(void)
{
    return atomic_load(&g_internal_error_total);
}

uint64_t utxo_apply_stage_outputs_added_total(void)
{
    return atomic_load(&g_total_outputs_added);
}

uint64_t utxo_apply_stage_outputs_spent_total(void)
{
    return atomic_load(&g_total_outputs_spent);
}

bool utxo_apply_dump_state_json(struct json_value *out, const char *key)
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
    json_push_kv_int (out, "verified_total",
                      (int64_t)atomic_load(&g_verified_total));
    json_push_kv_int (out, "spend_unknown_total",
                      (int64_t)atomic_load(&g_spend_unknown_total));
    json_push_kv_int (out, "utxo_collision_total",
                      (int64_t)atomic_load(&g_utxo_collision_total));
    json_push_kv_int (out, "value_overflow_total",
                      (int64_t)atomic_load(&g_value_overflow_total));
    json_push_kv_int (out, "delta_diverged_total",
                      (int64_t)atomic_load(&g_delta_diverged_total));
    json_push_kv_int (out, "upstream_failed_total",
                      (int64_t)atomic_load(&g_upstream_failed_total));
    json_push_kv_int (out, "internal_error_total",
                      (int64_t)atomic_load(&g_internal_error_total));
    json_push_kv_int (out, "outputs_added_total",
                      (int64_t)atomic_load(&g_total_outputs_added));
    json_push_kv_int (out, "outputs_spent_total",
                      (int64_t)atomic_load(&g_total_outputs_spent));
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
