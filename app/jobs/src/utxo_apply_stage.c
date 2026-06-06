/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * utxo_apply_stage — implementation. See jobs/utxo_apply_stage.h.
 *
 * Consumes proof_validate_log and computes a transparent UTXO delta.
 * It writes only utxo_apply_log plus its stage cursor in progress.kv. */

#include "platform/time_compat.h"
#include "jobs/utxo_apply_stage.h"
#include "jobs/utxo_apply_delta.h"
#include "jobs/stage_helpers.h"

#include "chain/chain.h"
#include "core/uint256.h"
#include "event/event.h"
#include "json/json.h"
#include "primitives/block.h"
#include "primitives/transaction.h"
#include "storage/disk_block_io.h"
#include "storage/progress_store.h"
#include "storage/utxo_projection.h"
#include "coins/coins.h"
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

struct proof_validate_row {
    int ok;
};

/* struct delta_entry / struct delta_summary plus inverse-delta persistence
 * and reorg-unwind machinery live in jobs/utxo_apply_delta.h /
 * utxo_apply_delta.c. */

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct main_state *g_ms = NULL;
static stage_t *g_stage = NULL;
static char g_datadir[2048] = {0};
static utxo_apply_reader_fn g_reader = NULL;
static void *g_reader_user = NULL;
static utxo_apply_lookup_fn g_lookup = NULL;
static void *g_lookup_user = NULL;

static _Atomic uint64_t g_verified_total = 0;
static _Atomic uint64_t g_spend_unknown_total = 0;
static _Atomic uint64_t g_utxo_collision_total = 0;
static _Atomic uint64_t g_value_overflow_total = 0;
static _Atomic uint64_t g_upstream_failed_total = 0;
static _Atomic uint64_t g_internal_error_total = 0;
static _Atomic uint64_t g_reorg_unwound_total = 0;
static _Atomic uint64_t g_total_outputs_added = 0;
static _Atomic uint64_t g_total_outputs_spent = 0;
static _Atomic int64_t  g_last_step_unix = 0;
static _Atomic int64_t  g_last_blocked_unix = 0;
static _Atomic int64_t  g_last_advance_height = -1;

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
        LOG_WARN("utxo_apply", "[utxo_apply] schema ensure failed: %s", err ? err : "(no message)");
        if (err) sqlite3_free(err);
        return false;
    }
    return true;
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
        LOG_WARN("utxo_apply", "[utxo_apply] proof_validate_log prepare failed: %s", sqlite3_errmsg(db));
        return -1;  // raw-return-ok:logged-above
    }
    sqlite3_bind_int(st, 1, height);
    int found = 0;
    int rc = sqlite3_step(st);  // raw-sql-ok:progress-kv-kernel-store
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
        LOG_WARN("utxo_apply", "[utxo_apply] prepare insert failed: %s", sqlite3_errmsg(db));
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
    sqlite3_bind_int64(stmt, 9, (sqlite3_int64)platform_time_wall_unix());
    rc = sqlite3_step(stmt);  // raw-sql-ok:progress-kv-kernel-store
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        LOG_WARN("utxo_apply", "[utxo_apply] insert height=%d rc=%d", height, rc);
        return false;
    }
    return true;
}

/* The delta structs, free_delta(_arr), and the block-delta builder
 * (utxo_apply_compute_block_delta) live in utxo_apply_delta.c, shared
 * with the inverse-delta persistence + reorg-unwind path. */

/* Author EV_UTXO_ADD/SPEND from a validated block delta. Called only when
 * the stage holds authority (UTXO_AUTHOR_STAGE) and the block verified. Adds
 * are emitted before spends: every UTXO key created in this block is unique
 * (compute_block_delta rejects collisions), and the only intra-block key
 * interaction is create-then-spend of the same output — which add-then-spend
 * resolves to "absent", matching the legacy per-tx order. The projection is a
 * set, so the resulting final state matches legacy interleaved emission
 * (proven empirically by test_utxo_apply_authorship_parity). */
static void emit_delta(const struct delta_summary *s, uint32_t height)
{
    for (size_t i = 0; i < s->added_count; i++)
        utxo_projection_emit_add(s->added[i].txid.data, s->added[i].vout,
                                 s->added[i].value, height,
                                 s->added[i].is_coinbase,
                                 s->added[i].script, s->added[i].script_len);
    for (size_t i = 0; i < s->spent_count; i++)
        utxo_projection_emit_spend(s->spent[i].txid.data, s->spent[i].vout);
}

/* compute_block_delta now lives in utxo_apply_delta.c as
 * utxo_apply_compute_block_delta (it owns the delta structs + the
 * persistence/inversion of the same arrays). */

static job_result_t step_apply(struct stage_step_ctx *c)
{
    atomic_store(&g_last_step_unix, platform_time_wall_unix());

    struct main_state *ms = g_ms;
    if (!ms) return JOB_IDLE;
    sqlite3 *db = progress_store_db();
    if (!db) return JOB_IDLE;

    int next_h = (int)c->cursor_in;
    if (next_h < 0) return JOB_FATAL;

    uint64_t pv_cursor = stage_cursor_persisted(db, "proof_validate",
                                               STAGE_NAME);
    if ((uint64_t)next_h >= pv_cursor) {
        atomic_store(&g_last_blocked_unix, platform_time_wall_unix());
        return JOB_IDLE;
    }

    struct proof_validate_row upstream;
    int found = proof_validate_log_at(db, next_h, &upstream);
    if (found < 0) return JOB_FATAL;
    if (found == 0) {
        atomic_store(&g_last_blocked_unix, platform_time_wall_unix());
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
        atomic_store(&g_last_blocked_unix, platform_time_wall_unix());
        return JOB_IDLE;
    }

    struct block blk;
    block_init(&blk);
    utxo_apply_reader_fn reader = g_reader ? g_reader
                                           : stage_default_block_reader;
    if (!reader(&blk, bi, g_datadir, g_reader_user)) {
        block_free(&blk);
        atomic_store(&g_last_blocked_unix, platform_time_wall_unix());
        return JOB_IDLE;
    }

    struct delta_summary summary;
    utxo_apply_compute_block_delta(&blk, (uint32_t)next_h,
                                   g_lookup, g_lookup_user, &summary);

    /* Author the UTXO projection from this validated delta when the
     * stage holds projection authority. Scripts in `summary.added` alias
     * into `blk`, so emit before block_free. */
    if (summary.ok && utxo_projection_get_author() == UTXO_AUTHOR_STAGE)
        emit_delta(&summary, (uint32_t)next_h);

    /* Persist the per-block inverse-delta so a later disconnect can be
     * reconstructed without re-reading legacy undo files. Stamped with the
     * OLD branch hash so a fork at the same height is distinguishable. Inside
     * the stage txn (stage_run_once's BEGIN IMMEDIATE), so the delta + log row
     * + cursor land atomically. Persisted only on a successful apply; failure
     * rows have nothing to invert. */
    if (summary.ok) {
        if (!utxo_apply_delta_persist(db, next_h, bi->phashBlock, &summary)) {
            free_delta(&summary);
            block_free(&blk);
            return JOB_FATAL;
        }
    }

    free_delta(&summary);
    block_free(&blk);

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

/* Production prevout resolver for utxo_apply, the init-time default for
 * g_lookup — the analogue of script_validate's created_index_prevout
 * self-default, but with the CORRECT semantics for utxo_apply: it must mean
 * "currently UNSPENT". The utxo_projection DELETEs a coin on spend, so a hit
 * from utxo_projection_get_coins == the coin is live/unspent. This is the
 * double-spend-safe source; a creation index (which never deletes spent rows)
 * would report found=true for an already-spent coin and let utxo_apply accept
 * a double-spend (monetary inflation / hard fork) AND false-trip BIP30
 * collision — so it MUST NOT be used here. The full pre-image
 * (value/height/is_coinbase/script) is required for the inverse-delta
 * restore-ADD. A genuine miss returns found=false (compute_block_delta then
 * records spend_unknown_utxo with the exact outpoint — never a silent pass).
 *
 * FRESHNESS CONTRACT: this reads the projection's `utxo` table, which is folded
 * from the event log by utxo_projection_catch_up. utxo_apply_stage_step_once
 * drives that catch_up after every advancing step so a coin created earlier in
 * the SAME drain is visible to a later block's spend. Without that the read
 * would be stale and could false-accept a cross-block double-spend. */
static bool projection_live_lookup(const struct uint256 *txid, uint32_t vout,
                                   struct utxo_apply_lookup *out, void *user)
{
    (void)user;
    if (!txid || !out)
        return false;
    memset(out, 0, sizeof(*out));

    utxo_projection_t *p = utxo_projection_get_global();
    if (!p)
        return true;   /* projection not open yet → treat as absent (found=0),
                        * matching the lookup==NULL "all external absent"
                        * contract; never a false-accept. */

    struct coins c;
    coins_init(&c);
    if (!utxo_projection_get_coins(p, txid->data, &c)) {
        coins_free(&c);
        return true;   /* no live output at this txid → found stays false */
    }

    bool ok = true;
    if (vout < c.num_vout && !tx_out_is_null(&c.vout[vout])) {
        const struct tx_out *o = &c.vout[vout];
        size_t slen = o->script_pub_key.size;
        if (slen > UTXO_APPLY_SCRIPT_MAX) {
            /* Contract violation (a UTXO scriptPubKey is <= MAX_SCRIPT_SIZE ==
             * UTXO_APPLY_SCRIPT_MAX). Fail the resolver (compute_block_delta
             * turns this into an internal_error) rather than truncate or
             * over-read a consensus script. */
            ok = false;
        } else {
            out->found       = true;
            out->value       = o->value;
            out->height      = (uint32_t)(c.height < 0 ? 0 : c.height);
            out->is_coinbase = c.is_coinbase;
            out->script_len  = (uint32_t)slen;
            if (slen)
                memcpy(out->script, o->script_pub_key.data, slen);
        }
    }
    coins_free(&c);
    return ok;
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
    if (!utxo_apply_ensure_delta_schema(db)) {
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
    /* Wire the production UTXO-set resolver unless a caller (e.g. a test)
     * already installed one. Without it g_lookup stays NULL and
     * utxo_apply_compute_block_delta treats EVERY external coin as absent,
     * rejecting every cross-block transparent spend as spend_unknown_utxo
     * (live-wedge blocker #5). Symmetric with script_validate's
     * created_index_prevout self-default (script_validate_stage.c). */
    if (!g_lookup)
        g_lookup = projection_live_lookup;
    pthread_mutex_unlock(&g_lock);

    LOG_INFO("utxo_apply", "[utxo_apply] stage initialised");
    return true;
}

job_result_t utxo_apply_stage_step_once(void)
{
    if (!g_stage) return JOB_IDLE;
    sqlite3 *db = progress_store_db();
    if (!db) return JOB_IDLE;
    /* Chain-extender: keep the visible chain[] window extended to the
     * most-work candidate so both the reorg-unwind detection and the
     * forward-apply below (each reads active_chain_at) see the winning
     * branch. This runs only when the stage owns UTXO projection authorship;
     * otherwise it leaves the active-chain window untouched. */
    reducer_extend_window_to_candidate(
        g_ms, utxo_projection_get_author() == UTXO_AUTHOR_STAGE);
    /* Drain any pending stage-side reorg disconnect BEFORE the next
     * forward apply (and before tip_finalize, which the supervisor drains
     * after us, reads our cursor). Self-contained txn; on failure the
     * cursor is untouched so the next tick retries. */
    progress_store_tx_lock();
    bool unwind_ok =
        utxo_apply_reorg_unwind_if_needed(db, g_stage, g_ms,
                                          &g_reorg_unwound_total,
                                          &g_last_blocked_unix);
    if (!unwind_ok) {
        progress_store_tx_unlock();
        return JOB_FATAL;
    }
    job_result_t r = stage_run_once(g_stage, db);
    progress_store_tx_unlock();
    /* CODE1 freshness: after an advancing step, fold the UTXO events this step
     * just emitted into the projection's read `utxo` table, so a coin CREATED
     * by this block is visible to projection_live_lookup (utxo_apply's prevout
     * resolver) when a LATER block in the SAME drain spends it. The read table
     * is otherwise folded only at boot (utxo_projection_catch_up's sole other
     * caller), so without this a cross-block create-then-spend within one
     * pre-restart drain would read a stale table and could false-accept a
     * double-spend. catch_up runs its OWN txn on the SEPARATE projection DB
     * handle (not progress.kv), so it does not nest with the lock just
     * released. Idempotent + crash-safe (the log commit is independent of the
     * stage cursor txn). Covers BOTH drivers — reducer ingest AND the
     * supervisor drain — since both reach the chain via this function. */
    if (r == JOB_ADVANCED) {
        utxo_projection_t *proj = utxo_projection_get_global();
        if (proj)
            (void)utxo_projection_catch_up(proj);
    }
    return r;
}

STAGE_DRAIN_IMPL(utxo_apply)

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
    atomic_store(&g_verified_total, (uint64_t)0);
    atomic_store(&g_spend_unknown_total, (uint64_t)0);
    atomic_store(&g_utxo_collision_total, (uint64_t)0);
    atomic_store(&g_value_overflow_total, (uint64_t)0);
    atomic_store(&g_upstream_failed_total, (uint64_t)0);
    atomic_store(&g_internal_error_total, (uint64_t)0);
    atomic_store(&g_reorg_unwound_total, (uint64_t)0);
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

uint64_t utxo_apply_stage_cursor(void)
{
    return g_stage ? stage_cursor(g_stage) : 0;
}

bool utxo_apply_stage_succeeded_at(int height)
{
    if (height < 0)
        return false;
    sqlite3 *db = progress_store_db();
    if (!db)
        return false;
    progress_store_tx_lock();
    sqlite3_stmt *st = NULL;
    bool ok = false;
    if (sqlite3_prepare_v2(db,
            "SELECT ok FROM utxo_apply_log WHERE height = ?",
            -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int(st, 1, height);
        if (sqlite3_step(st) == SQLITE_ROW)  // raw-sql-ok:progress-kv-kernel-store
            ok = sqlite3_column_int(st, 0) == 1;
        sqlite3_finalize(st);
    }
    progress_store_tx_unlock();
    return ok;
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

uint64_t utxo_apply_stage_upstream_failed_total(void)
{
    return atomic_load(&g_upstream_failed_total);
}

uint64_t utxo_apply_stage_internal_error_total(void)
{
    return atomic_load(&g_internal_error_total);
}

uint64_t utxo_apply_stage_reorg_unwound_total(void)
{
    return atomic_load(&g_reorg_unwound_total);
}

uint64_t utxo_apply_stage_outputs_added_total(void)
{
    return atomic_load(&g_total_outputs_added);
}

uint64_t utxo_apply_stage_outputs_spent_total(void)
{
    return atomic_load(&g_total_outputs_spent);
}

/* Surface the lowest ok=0 row (status/reason kind/txid/vout) into `out`,
 * mirroring the validate_headers_report failure-summary query convention.
 * The reason kind is utxo_apply's first_failure_kind (e.g. lookup_spend,
 * spend_unknown_utxo); the txid|vout is decoded from the 36-byte detail
 * blob. No-op if the db is unavailable or there is no failing row. Takes
 * its own tx lock since dump_state runs outside any stage txn. */
static void dump_first_failure(struct json_value *out, sqlite3 *db)
{
    if (!db) return;
    progress_store_tx_lock();
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
        "SELECT height, COALESCE(status,''), "
        "       COALESCE(first_failure_kind,''), first_failure_detail "
        "  FROM utxo_apply_log WHERE ok=0 "
        " ORDER BY height ASC LIMIT 1",
        -1, &st, NULL) == SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW) {  // raw-sql-ok:progress-kv-kernel-store
        json_push_kv_int(out, "first_failure_height",
                         sqlite3_column_int64(st, 0));
        const unsigned char *status = sqlite3_column_text(st, 1);
        const unsigned char *kind = sqlite3_column_text(st, 2);
        json_push_kv_str(out, "first_failure_status",
                         status ? (const char *)status : "");
        json_push_kv_str(out, "first_failure_kind",
                         kind ? (const char *)kind : "");
        const uint8_t *d = sqlite3_column_blob(st, 3);
        char hex[65] = {0};
        int64_t vout = -1;
        if (d && sqlite3_column_bytes(st, 3) == 36) {
            struct uint256 t;
            memcpy(t.data, d, 32);
            uint256_get_hex(&t, hex);
            vout = (int64_t)d[32] | ((int64_t)d[33] << 8) |
                   ((int64_t)d[34] << 16) | ((int64_t)d[35] << 24);
        }
        json_push_kv_str(out, "first_failure_txid", hex);
        json_push_kv_int(out, "first_failure_vout", vout);
    }
    sqlite3_finalize(st);
    progress_store_tx_unlock();
}

bool utxo_apply_dump_state_json(struct json_value *out, const char *key)
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
    json_push_kv_int (out, "spend_unknown_total",
                      (int64_t)atomic_load(&g_spend_unknown_total));
    json_push_kv_int (out, "utxo_collision_total",
                      (int64_t)atomic_load(&g_utxo_collision_total));
    json_push_kv_int (out, "value_overflow_total",
                      (int64_t)atomic_load(&g_value_overflow_total));
    json_push_kv_int (out, "upstream_failed_total",
                      (int64_t)atomic_load(&g_upstream_failed_total));
    json_push_kv_int (out, "internal_error_total",
                      (int64_t)atomic_load(&g_internal_error_total));
    json_push_kv_int (out, "reorg_unwound_total",
                      (int64_t)atomic_load(&g_reorg_unwound_total));
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
    json_push_kv_int (out, "log_rows",
                      db ? stage_log_row_count(db, STAGE_NAME,
                                               "utxo_apply_log") : 0);
    dump_first_failure(out, db);
    stage_dump_counters(out, g_stage);
    return true;
}
