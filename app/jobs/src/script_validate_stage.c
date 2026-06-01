/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * script_validate_stage — implementation. See jobs/script_validate_stage.h.
 *
 * Consumes body_persist_log and replays script verification over block
 * bodies already proven readable by body_persist. It writes
 * script_validate_log plus its stage cursor in progress.kv. */

#include "platform/time_compat.h"
#include "jobs/script_validate_stage.h"
#include "jobs/stage_helpers.h"

#include "chain/chain.h"
#include "chain/chainparams.h"
#include "consensus/upgrades.h"
#include "core/uint256.h"
#include "event/event.h"
#include "json/json.h"
#include "primitives/block.h"
#include "script/interpreter.h"
#include "script/script_flags.h"
#include "storage/disk_block_io.h"
#include "storage/event_log.h"
#include "storage/event_log_payloads.h"
#include "storage/event_log_singleton.h"
#include "jobs/block_header_emit.h"
#include "storage/progress_store.h"
#include "storage/txdb.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"
#include "util/stage.h"
#include "util/util.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"
#include "validation/sighash.h"
#include "validation/tx_verifier.h"

#include <pthread.h>
#include <sqlite3.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define STAGE_NAME "script_validate"

extern struct block_tree_db *g_active_block_tree;

struct body_persist_row {
    int ok;
    char source[64];
};

struct validate_summary {
    int ok;
    int internal_error;
    size_t tx_count;
    size_t input_count;
    struct uint256 first_failure_txid;
    int first_failure_vin;
};

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct main_state *g_ms = NULL;
static stage_t *g_stage = NULL;
static char g_datadir[2048] = {0};
static script_validate_reader_fn g_reader = NULL;
static void *g_reader_user = NULL;
static script_validate_prevout_fn g_prevout = NULL;
static void *g_prevout_user = NULL;

static _Atomic uint64_t g_verified_total = 0;
static _Atomic uint64_t g_script_invalid_total = 0;
static _Atomic uint64_t g_internal_error_total = 0;
static _Atomic uint64_t g_upstream_failed_total = 0;
static _Atomic uint64_t g_inputs_verified_total = 0;
static _Atomic uint64_t g_inputs_failed_total = 0;
static _Atomic int64_t  g_last_step_unix = 0;
static _Atomic int64_t  g_last_blocked_unix = 0;
static _Atomic int64_t  g_last_advance_height = -1;
static _Atomic uint64_t g_header_event_emit_total = 0;
static _Atomic uint64_t g_header_event_emit_fail_total = 0;

static bool read_tx_from_index(const struct uint256 *txid,
                               struct transaction *tx)
{
    if (!g_active_block_tree || !g_ms || !g_ms->fTxIndex)
        return false;

    struct disk_tx_pos pos;
    disk_tx_pos_init(&pos);
    if (!block_tree_db_read_tx_index(g_active_block_tree, txid, &pos))
        return false;

    disk_block_io_lock();
    FILE *f = open_block_file(g_datadir, &pos.block_pos, true);
    if (!f) {
        disk_block_io_unlock();
        return false;
    }

    bool ok = false;
    if (fseek(f, (long)pos.block_pos.nPos + (long)pos.nTxOffset,
              SEEK_SET) == 0) {
        unsigned char tx_buf[2 * 1024 * 1024];
        size_t tx_read = fread(tx_buf, 1, sizeof(tx_buf), f); // disk-io-lock: held
        if (tx_read > 0) {
            struct byte_stream s;
            stream_init_from_data(&s, tx_buf, tx_read);
            transaction_free(tx);
            transaction_init(tx);
            ok = transaction_deserialize(tx, &s);
        }
    }
    disk_block_io_release_handle(f);
    disk_block_io_unlock();
    return ok;
}

static bool default_prevout(const struct outpoint *prevout,
                            struct tx_out *out, void *user)
{
    (void)user;
    if (!prevout || !out)
        return false;
    struct transaction tx;
    transaction_init(&tx);
    bool ok = read_tx_from_index(&prevout->hash, &tx);
    if (ok && prevout->n < tx.num_vout) {
        *out = tx.vout[prevout->n];
    } else {
        ok = false;
    }
    transaction_free(&tx);
    return ok;
}

static bool ensure_log_schema(sqlite3 *db)
{
    static const char *const sql =
        "CREATE TABLE IF NOT EXISTS script_validate_log ("
        "  height             INTEGER PRIMARY KEY,"
        "  status             TEXT    NOT NULL,"
        "  ok                 INTEGER NOT NULL,"
        "  tx_count           INTEGER NOT NULL,"
        "  input_count        INTEGER NOT NULL,"
        "  first_failure_txid BLOB,"
        "  first_failure_vin  INTEGER,"
        "  validated_at       INTEGER NOT NULL"
        ")";
    char *err = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
        LOG_WARN("script_validate", "[script_validate] schema ensure failed: %s", err ? err : "(no message)");
        if (err) sqlite3_free(err);
        return false;
    }
    return true;
}

static int body_persist_log_at(sqlite3 *db, int height,
                               struct body_persist_row *out)
{
    memset(out, 0, sizeof(*out));
    out->ok = -1;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
        "SELECT source, ok FROM body_persist_log WHERE height = ?",
        -1, &st, NULL) != SQLITE_OK) {
        LOG_WARN("script_validate", "[script_validate] body_persist_log prepare failed: %s", sqlite3_errmsg(db));
        return -1;  // raw-return-ok:logged-above
    }
    sqlite3_bind_int(st, 1, height);
    int found = 0;
    int rc = sqlite3_step(st);  // raw-sql-ok:kernel-primitive
    if (rc == SQLITE_ROW) {
        const unsigned char *src = sqlite3_column_text(st, 0);
        if (src)
            snprintf(out->source, sizeof(out->source), "%s",
                     (const char *)src);
        out->ok = sqlite3_column_int(st, 1);
        found = 1;
    }
    sqlite3_finalize(st);
    return found;
}

static bool log_insert(sqlite3 *db, int height, const char *status, bool ok,
                       size_t tx_count, size_t input_count,
                       const struct uint256 *first_failure_txid,
                       int first_failure_vin)
{
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
        "INSERT OR REPLACE INTO script_validate_log "
        "(height, status, ok, tx_count, input_count, first_failure_txid, "
        " first_failure_vin, validated_at) VALUES (?,?,?,?,?,?,?,?)",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        LOG_WARN("script_validate", "[script_validate] prepare insert failed: %s", sqlite3_errmsg(db));
        return false;
    }
    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)height);
    sqlite3_bind_text (stmt, 2, status, -1, SQLITE_STATIC);
    sqlite3_bind_int  (stmt, 3, ok ? 1 : 0);
    sqlite3_bind_int64(stmt, 4, (sqlite3_int64)tx_count);
    sqlite3_bind_int64(stmt, 5, (sqlite3_int64)input_count);
    if (first_failure_txid)
        sqlite3_bind_blob(stmt, 6, first_failure_txid->data, 32,
                          SQLITE_STATIC);
    else
        sqlite3_bind_null(stmt, 6);
    if (first_failure_vin >= 0)
        sqlite3_bind_int(stmt, 7, first_failure_vin);
    else
        sqlite3_bind_null(stmt, 7);
    sqlite3_bind_int64(stmt, 8, (sqlite3_int64)platform_time_wall_unix());
    rc = sqlite3_step(stmt);  // raw-sql-ok:kernel-primitive
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        LOG_WARN("script_validate", "[script_validate] insert height=%d rc=%d", height, rc);
        return false;
    }
    return true;
}

static void validate_summary_init(struct validate_summary *s)
{
    memset(s, 0, sizeof(*s));
    s->ok = 1;
    s->first_failure_vin = -1;
    uint256_set_null(&s->first_failure_txid);
}

static void validate_block_scripts(const struct block *blk, int height,
                                   struct validate_summary *out)
{
    validate_summary_init(out);
    if (!blk) {
        out->ok = 0;
        out->internal_error = 1;
        return;
    }

    uint32_t flags = SCRIPT_VERIFY_P2SH | SCRIPT_VERIFY_CHECKLOCKTIMEVERIFY;
    uint32_t branch_id = consensus_current_epoch_branch_id(
        height, &chain_params_get()->consensus);

    for (size_t ti = 0; ti < blk->num_vtx; ti++) {
        const struct transaction *tx = &blk->vtx[ti];
        out->tx_count++;
        if (transaction_is_coinbase(tx))
            continue;

        struct precomputed_tx_data txdata;
        precompute_tx_data(tx, &txdata);

        for (size_t vi = 0; vi < tx->num_vin; vi++) {
            struct tx_out prev;
            tx_out_set_null(&prev);
            script_validate_prevout_fn resolver =
                g_prevout ? g_prevout : default_prevout;
            if (!resolver(&tx->vin[vi].prevout, &prev, g_prevout_user)) {
                out->ok = 0;
                out->internal_error = 1;
                if (out->first_failure_vin < 0) {
                    out->first_failure_txid = tx->hash;
                    out->first_failure_vin = (int)vi;
                }
                return;
            }

            struct tx_sig_checker tsc;
            tx_sig_checker_init(&tsc, tx, (unsigned int)vi, prev.value,
                                branch_id, &txdata);
            struct sig_checker checker = tx_make_sig_checker(&tsc);
            ScriptError serror = SCRIPT_ERR_OK;
            out->input_count++;
            bool ok = verify_script(&tx->vin[vi].script_sig,
                                    &prev.script_pub_key, flags, &checker,
                                    branch_id, &serror);
            if (ok) {
                atomic_fetch_add(&g_inputs_verified_total, 1);
            } else {
                atomic_fetch_add(&g_inputs_failed_total, 1);
                out->ok = 0;
                if (out->first_failure_vin < 0) {
                    out->first_failure_txid = tx->hash;
                    out->first_failure_vin = (int)vi;
                }
                return;
            }
        }
    }
}


static job_result_t step_validate(struct stage_step_ctx *c)
{
    atomic_store(&g_last_step_unix, platform_time_wall_unix());

    struct main_state *ms = g_ms;
    if (!ms) return JOB_IDLE;
    sqlite3 *db = progress_store_db();
    if (!db) return JOB_IDLE;

    int next_h = (int)c->cursor_in;
    if (next_h < 0) return JOB_FATAL;

    uint64_t bp_cursor = stage_cursor_persisted(db, "body_persist",
                                               STAGE_NAME);
    if ((uint64_t)next_h >= bp_cursor) {
        atomic_store(&g_last_blocked_unix, platform_time_wall_unix());
        return JOB_IDLE;
    }

    struct body_persist_row upstream;
    int found = body_persist_log_at(db, next_h, &upstream);
    if (found < 0) return JOB_FATAL;
    if (found == 0) {
        atomic_store(&g_last_blocked_unix, platform_time_wall_unix());
        return JOB_IDLE;
    }

    if (upstream.ok == 0) {
        if (!log_insert(db, next_h, "upstream_failed", false, 0, 0,
                        NULL, -1))
            return JOB_FATAL;
        atomic_fetch_add(&g_upstream_failed_total, 1);
        atomic_store(&g_last_advance_height, (int64_t)next_h);
        c->cursor_out = c->cursor_in + 1;
        return JOB_ADVANCED;
    }

    struct block_index *bi = active_chain_at(&ms->chain_active, next_h);
    if (!bi || !(bi->nStatus & BLOCK_HAVE_DATA)) {
        atomic_store(&g_last_blocked_unix, platform_time_wall_unix());
        return JOB_IDLE;
    }

    struct block blk;
    block_init(&blk);
    script_validate_reader_fn reader = g_reader ? g_reader
                                                : stage_default_block_reader;
    if (!reader(&blk, bi, g_datadir, g_reader_user)) {
        block_free(&blk);
        atomic_store(&g_last_blocked_unix, platform_time_wall_unix());
        return JOB_IDLE;
    }

    struct validate_summary summary;
    validate_block_scripts(&blk, next_h, &summary);
    block_free(&blk);

    const char *status = "verified";
    bool ok = true;
    const struct uint256 *fail_txid = NULL;
    int fail_vin = -1;
    if (!summary.ok && summary.internal_error) {
        status = "internal_error";
        ok = false;
        fail_txid = &summary.first_failure_txid;
        fail_vin = summary.first_failure_vin;
        atomic_fetch_add(&g_internal_error_total, 1);
        event_emitf(EV_BLOCK_REJECTED, 0,
                    "script_validate internal_error height=%d source=%s",
                    next_h, upstream.source);
    } else if (!summary.ok) {
        status = "script_invalid";
        ok = false;
        fail_txid = &summary.first_failure_txid;
        fail_vin = summary.first_failure_vin;
        atomic_fetch_add(&g_script_invalid_total, 1);
        event_emitf(EV_BLOCK_REJECTED, 0,
                    "script_validate script_invalid height=%d vin=%d",
                    next_h, fail_vin);
    } else {
        atomic_fetch_add(&g_verified_total, 1);
        /* Raise the in-memory validity level to BLOCK_VALID_SCRIPTS, which
         * tip_finalize.preconditions_ok requires before publication. This is
         * a validity LEVEL stored in the low BLOCK_VALID_MASK bits, not an
         * OR-able flag, so clear the mask before setting it. Re-emit
         * EV_BLOCK_HEADER so the projection persists the new nStatus across
         * restart. bi is the live in-memory entry from active_chain_at; blk
         * was freed above but bi is independent. */
        bi->nStatus = (bi->nStatus & ~(unsigned)BLOCK_VALID_MASK)
                      | BLOCK_VALID_SCRIPTS;
        block_index_emit_header_event(bi, "script_validate", &g_header_event_emit_total, &g_header_event_emit_fail_total);
    }

    if (!log_insert(db, next_h, status, ok, summary.tx_count,
                    summary.input_count, fail_txid, fail_vin))
        return JOB_FATAL;

    atomic_store(&g_last_advance_height, (int64_t)next_h);
    c->cursor_out = c->cursor_in + 1;
    return JOB_ADVANCED;
}

bool script_validate_stage_init(struct main_state *ms)
{
    if (!ms) LOG_FAIL("script_validate", "init: NULL main_state");

    sqlite3 *db = progress_store_db();
    if (!db) LOG_FAIL("script_validate",
        "init: progress_store not open");

    pthread_mutex_lock(&g_lock);
    if (g_stage != NULL) {
        bool same = (g_ms == ms);
        pthread_mutex_unlock(&g_lock);
        if (!same)
            LOG_FAIL("script_validate",
                "init: already bound to a different main_state");
        return true;
    }

    if (!ensure_log_schema(db)) {
        pthread_mutex_unlock(&g_lock);
        return false;
    }

    GetDataDir(true, g_datadir, sizeof(g_datadir));

    stage_t *s = stage_create(STAGE_NAME, step_validate, NULL);
    if (!s) {
        pthread_mutex_unlock(&g_lock);
        LOG_FAIL("script_validate", "init: stage_create failed");
    }

    g_ms = ms;
    g_stage = s;
    pthread_mutex_unlock(&g_lock);

    LOG_INFO("script_validate", "[script_validate] stage initialised");
    return true;
}

job_result_t script_validate_stage_step_once(void)
{
    if (!g_stage) return JOB_IDLE;
    sqlite3 *db = progress_store_db();
    if (!db) return JOB_IDLE;
    return stage_run_once(g_stage, db);
}

STAGE_DRAIN_IMPL(script_validate)

void script_validate_stage_shutdown(void)
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
    g_prevout = NULL;
    g_prevout_user = NULL;
    atomic_store(&g_verified_total, (uint64_t)0);
    atomic_store(&g_script_invalid_total, (uint64_t)0);
    atomic_store(&g_internal_error_total, (uint64_t)0);
    atomic_store(&g_upstream_failed_total, (uint64_t)0);
    atomic_store(&g_inputs_verified_total, (uint64_t)0);
    atomic_store(&g_inputs_failed_total, (uint64_t)0);
    atomic_store(&g_last_step_unix, (int64_t)0);
    atomic_store(&g_last_blocked_unix, (int64_t)0);
    atomic_store(&g_last_advance_height, (int64_t)-1);
    atomic_store(&g_header_event_emit_total, (uint64_t)0);
    atomic_store(&g_header_event_emit_fail_total, (uint64_t)0);
    pthread_mutex_unlock(&g_lock);
}

void script_validate_stage_set_reader(script_validate_reader_fn fn,
                                      void *user)
{
    pthread_mutex_lock(&g_lock);
    g_reader = fn;
    g_reader_user = user;
    pthread_mutex_unlock(&g_lock);
}

void script_validate_stage_set_prevout_resolver(script_validate_prevout_fn fn,
                                                void *user)
{
    pthread_mutex_lock(&g_lock);
    g_prevout = fn;
    g_prevout_user = user;
    pthread_mutex_unlock(&g_lock);
}

uint64_t script_validate_stage_cursor(void)
{
    return g_stage ? stage_cursor(g_stage) : 0;
}

uint64_t script_validate_stage_verified_total(void)
{
    return atomic_load(&g_verified_total);
}

uint64_t script_validate_stage_script_invalid_total(void)
{
    return atomic_load(&g_script_invalid_total);
}

uint64_t script_validate_stage_internal_error_total(void)
{
    return atomic_load(&g_internal_error_total);
}

uint64_t script_validate_stage_upstream_failed_total(void)
{
    return atomic_load(&g_upstream_failed_total);
}

uint64_t script_validate_stage_inputs_verified_total(void)
{
    return atomic_load(&g_inputs_verified_total);
}

uint64_t script_validate_stage_inputs_failed_total(void)
{
    return atomic_load(&g_inputs_failed_total);
}

bool script_validate_dump_state_json(struct json_value *out, const char *key)
{
    (void)key;
    if (!out) return false;
    json_set_object(out);

    sqlite3 *db = progress_store_db();
    int64_t now = platform_time_wall_unix();
    int64_t last = atomic_load(&g_last_step_unix);

    json_push_kv_bool(out, "initialised", g_stage != NULL);
    json_push_kv_str (out, "stage_name", STAGE_NAME);
    json_push_kv_int (out, "cursor",
                      (int64_t)(g_stage ? stage_cursor(g_stage) : 0));
    json_push_kv_int (out, "verified_total",
                      (int64_t)atomic_load(&g_verified_total));
    json_push_kv_int (out, "script_invalid_total",
                      (int64_t)atomic_load(&g_script_invalid_total));
    json_push_kv_int (out, "internal_error_total",
                      (int64_t)atomic_load(&g_internal_error_total));
    json_push_kv_int (out, "upstream_failed_total",
                      (int64_t)atomic_load(&g_upstream_failed_total));
    json_push_kv_int (out, "inputs_verified_total",
                      (int64_t)atomic_load(&g_inputs_verified_total));
    json_push_kv_int (out, "inputs_failed_total",
                      (int64_t)atomic_load(&g_inputs_failed_total));
    json_push_kv_int (out, "header_event_emit_total",
                      (int64_t)atomic_load(&g_header_event_emit_total));
    json_push_kv_int (out, "header_event_emit_fail_total",
                      (int64_t)atomic_load(&g_header_event_emit_fail_total));
    json_push_kv_int (out, "last_advance_height",
                      atomic_load(&g_last_advance_height));
    json_push_kv_int (out, "last_step_unix", last);
    json_push_kv_int (out, "last_step_age_seconds",
                      last > 0 ? now - last : -1);
    json_push_kv_int (out, "last_blocked_unix",
                      atomic_load(&g_last_blocked_unix));
    json_push_kv_int (out, "log_rows",
                      db ? stage_log_row_count(db, STAGE_NAME,
                                               "script_validate_log") : 0);
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
