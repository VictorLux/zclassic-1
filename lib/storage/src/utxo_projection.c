/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * utxo_projection — Phase 4b event-log consumer for the UTXO set.
 *
 * See `storage/utxo_projection.h` for the contract. This file is the
 * first PRODUCTION consumer of the Phase 4a event log primitive — the
 * second-most-critical authoritative cutover after Wave S C-8 (both
 * touch the UTXO set). Shadow mode only: legacy `update_coins()` still
 * writes coins.db; we additionally emit + consume events, and the
 * shadow-diff MCP tool gates the 24h cutover soak.
 *
 * Design notes
 * ------------
 * - Schema mirrors the legacy `utxos` table column-for-column so the
 *   commitment SHA3 is byte-identical (canonical serialisation comes
 *   from `lib/coins/src/utxo_commitment.c:utxo_commitment_sha3_compute_table`).
 *
 * - `catch_up` wraps the entire stream in a single SQLite IMMEDIATE
 *   txn so a crash mid-batch either replays cleanly (cursor unchanged)
 *   or commits atomically. There is no partial-state on disk.
 *
 * - "REPLACE collision" is logged but not fatal — chain reorgs may
 *   replay ADDs after their corresponding SPENDs in some orderings,
 *   and the projection layer's contract is to be lenient (the emitter
 *   is the authority on event ordering).
 *
 * - Raw `sqlite3_step` calls below carry `// raw-sql-ok:projection-primitive`
 *   markers, matching the convention already established by
 *   `progress_store.c` and `peers_projection.c`. Projections sit BELOW
 *   the AR lifecycle (the UTXO row is not an AR model in this layer).
 */

#include "storage/utxo_projection.h"

#include "crypto/sha3.h"
#include "json/json.h"
#include "platform/time_compat.h"
#include "storage/event_log_payloads.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"

#include <inttypes.h>
#include <pthread.h>
#include <sqlite3.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The event log frames each event with a 16-byte header + 16-byte
 * sentinel (see storage/event_log.h). The stream callback only sees
 * `(offset, type, payload, len)`, so to compute "next offset after
 * this event" we add this constant. event_log.h locks the framing
 * size; if it ever changes, this constant must move with it (and the
 * peers_projection already encodes the same number — keep in sync). */
#define EVENT_LOG_FRAME_OVERHEAD 32u

/* Commit cursor checkpoint every N consumed events inside catch_up.
 * The whole catch_up is a single IMMEDIATE txn so this is not a
 * durability checkpoint — it just bounds the WAL size for very long
 * replays. 1000 matches the assignment spec. */
#define CATCHUP_CHECKPOINT_INTERVAL 1000u

#define UTXO_PROJECTION_SCHEMA_VERSION 1

struct utxo_projection {
    sqlite3 *db;
    event_log_t *log;

    /* Cached so dump_state_json doesn't have to re-query the meta
     * table. Updated whenever `meta_set_u64` succeeds. */
    uint64_t last_consumed_offset;

    /* Cumulative counters — reset on every process restart (intentional;
     * the per-process delta is what's interesting for observability). */
    uint64_t events_consumed_total;
    uint64_t ev_utxo_add_total;
    uint64_t ev_utxo_spend_total;
    uint64_t replace_collisions_total;
    uint64_t last_catch_up_ms;

    char path[1024];
};

/* ── Process-global accessors ──────────────────────────────────────── */

static _Atomic(utxo_projection_t *) g_projection = NULL;
static _Atomic(event_log_t *)       g_event_log  = NULL;

/* Shadow-emission counters (independent of consumer counters so we can
 * tell emit failures from consume failures in observability). */
static _Atomic uint64_t g_emit_add_total   = 0;
static _Atomic uint64_t g_emit_spend_total = 0;
static _Atomic uint64_t g_emit_fail_total  = 0;

utxo_projection_t *utxo_projection_get_global(void)
{
    return atomic_load_explicit(&g_projection, memory_order_acquire);
}

void utxo_projection_set_event_log(event_log_t *log)
{
    atomic_store_explicit(&g_event_log, log, memory_order_release);
}

event_log_t *utxo_projection_event_log(void)
{
    return atomic_load_explicit(&g_event_log, memory_order_acquire);
}

/* ── Tiny helpers ──────────────────────────────────────────────────── */

static int64_t now_ms(void)
{
    struct timespec ts;
    platform_time_monotonic_timespec(&ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static bool exec_sql(sqlite3 *db, const char *sql, const char *ctx)
{
    char *err = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        fprintf(stderr,  // obs-ok:utxo-projection-sql
                "[utxo_projection] %s failed: %s\n",
                ctx, err ? err : sqlite3_errmsg(db));
        if (err) sqlite3_free(err);
        return false;
    }
    return true;
}

static bool apply_pragmas(sqlite3 *db)
{
    return exec_sql(db, "PRAGMA journal_mode=WAL", "journal_mode") &&
           exec_sql(db, "PRAGMA synchronous=NORMAL", "synchronous") &&
           exec_sql(db, "PRAGMA busy_timeout=5000", "busy_timeout");
}

static bool ensure_schema(sqlite3 *db)
{
    return exec_sql(db,
        "CREATE TABLE IF NOT EXISTS utxo ("
        "  txid        BLOB    NOT NULL,"
        "  vout        INTEGER NOT NULL,"
        "  value       INTEGER NOT NULL,"
        "  height      INTEGER NOT NULL,"
        "  is_coinbase INTEGER NOT NULL,"
        "  script      BLOB    NOT NULL,"
        "  PRIMARY KEY (txid, vout)"
        ") WITHOUT ROWID",
        "create utxo") &&
        exec_sql(db,
        "CREATE TABLE IF NOT EXISTS projection_meta ("
        "  k TEXT PRIMARY KEY,"
        "  v TEXT NOT NULL"
        ")",
        "create projection_meta") &&
        exec_sql(db,
        "INSERT OR IGNORE INTO projection_meta(k,v) "
        "VALUES('schema_version','1')",
        "insert schema_version") &&
        exec_sql(db,
        "INSERT OR IGNORE INTO projection_meta(k,v) "
        "VALUES('last_consumed_offset','0')",
        "insert last_consumed_offset");
}

static uint64_t meta_get_u64(sqlite3 *db, const char *key)
{
    sqlite3_stmt *s = NULL;
    uint64_t v = 0;
    int rc = sqlite3_prepare_v2(db,
        "SELECT v FROM projection_meta WHERE k=?",
        -1, &s, NULL);
    if (rc != SQLITE_OK) return 0;
    sqlite3_bind_text(s, 1, key, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(s);  // raw-sql-ok:projection-primitive
    if (rc == SQLITE_ROW) {
        const unsigned char *txt = sqlite3_column_text(s, 0);
        if (txt) v = (uint64_t)strtoull((const char *)txt, NULL, 10);
    }
    sqlite3_finalize(s);
    return v;
}

/* meta_set_u64 must run inside the caller's open txn (catch_up wraps
 * the stream in IMMEDIATE) so cursor + UTXO mutations commit
 * atomically. */
static bool meta_set_u64(sqlite3 *db, const char *key, uint64_t value)
{
    sqlite3_stmt *s = NULL;
    char buf[32];
    snprintf(buf, sizeof(buf), "%" PRIu64, value);
    int rc = sqlite3_prepare_v2(db,
        "INSERT OR REPLACE INTO projection_meta(k,v) VALUES(?,?)",
        -1, &s, NULL);
    if (rc != SQLITE_OK) return false;
    sqlite3_bind_text(s, 1, key, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 2, buf, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(s);  // raw-sql-ok:projection-primitive
    sqlite3_finalize(s);
    return rc == SQLITE_DONE;
}

/* ── Open / close ──────────────────────────────────────────────────── */

utxo_projection_t *utxo_projection_open(const char *projection_path,
                                        event_log_t *log)
{
    if (!projection_path || !projection_path[0] || !log) {
        fprintf(stderr,  // obs-ok:utxo-projection-open
                "[utxo_projection] open: invalid args path=%p log=%p\n",
                (const void *)projection_path, (void *)log);
        return NULL;
    }

    sqlite3 *db = NULL;
    int rc = sqlite3_open_v2(projection_path, &db,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr,  // obs-ok:utxo-projection-open
                "[utxo_projection] sqlite open failed: %s\n",
                db ? sqlite3_errmsg(db) : sqlite3_errstr(rc));
        if (db) sqlite3_close(db);
        return NULL;
    }
    if (!apply_pragmas(db) || !ensure_schema(db)) {
        sqlite3_close(db);
        return NULL;
    }

    utxo_projection_t *p = zcl_malloc(sizeof(*p), "utxo_projection");
    if (!p) {
        sqlite3_close(db);
        return NULL;
    }
    memset(p, 0, sizeof(*p));
    p->db  = db;
    p->log = log;
    p->last_consumed_offset = meta_get_u64(db, "last_consumed_offset");
    snprintf(p->path, sizeof(p->path), "%s", projection_path);

    /* Publish the handle for `utxo_projection_get_global`. The first
     * caller of `_open` wins; subsequent overlapping opens (tests) are
     * harmless because the projection_meta row is idempotent. */
    atomic_store_explicit(&g_projection, p, memory_order_release);

    fprintf(stderr,  // obs-ok:utxo-projection-lifecycle
            "[utxo_projection] opened %s (last_consumed_offset=%" PRIu64 ")\n",
            projection_path, p->last_consumed_offset);
    return p;
}

void utxo_projection_close(utxo_projection_t *p)
{
    if (!p) return;
    utxo_projection_t *cur = atomic_load_explicit(&g_projection,
                                                  memory_order_acquire);
    if (cur == p)
        atomic_store_explicit(&g_projection, NULL, memory_order_release);
    if (p->db) {
        sqlite3_exec(p->db, "PRAGMA wal_checkpoint(TRUNCATE)",
                     NULL, NULL, NULL);
        sqlite3_close(p->db);
    }
    free(p);
}

/* ── catch_up ──────────────────────────────────────────────────────── */

/* True if (txid, vout) currently exists in the projection. Used to
 * count REPLACE collisions for observability — a non-zero counter on a
 * non-reorging chain indicates a bug. */
static bool utxo_exists(sqlite3 *db, const uint8_t txid[32], uint32_t vout)
{
    sqlite3_stmt *s = NULL;
    bool found = false;
    if (sqlite3_prepare_v2(db,
            "SELECT 1 FROM utxo WHERE txid=? AND vout=?",
            -1, &s, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_blob(s, 1, txid, 32, SQLITE_TRANSIENT);
    sqlite3_bind_int(s, 2, (int)vout);
    found = sqlite3_step(s) == SQLITE_ROW;  // raw-sql-ok:projection-primitive
    sqlite3_finalize(s);
    return found;
}

static bool apply_utxo_add(utxo_projection_t *p,
                           const struct ev_utxo_add_hdr *hdr,
                           const uint8_t *script_bytes)
{
    if (utxo_exists(p->db, hdr->txid, hdr->vout))
        p->replace_collisions_total++;

    sqlite3_stmt *s = NULL;
    int rc = sqlite3_prepare_v2(p->db,
        "INSERT OR REPLACE INTO utxo"
        "(txid,vout,value,height,is_coinbase,script) VALUES(?,?,?,?,?,?)",
        -1, &s, NULL);
    if (rc != SQLITE_OK) return false;
    sqlite3_bind_blob (s, 1, hdr->txid, 32, SQLITE_TRANSIENT);
    sqlite3_bind_int  (s, 2, (int)hdr->vout);
    sqlite3_bind_int64(s, 3, (sqlite3_int64)hdr->value);
    sqlite3_bind_int64(s, 4, (sqlite3_int64)hdr->height);
    sqlite3_bind_int  (s, 5, hdr->is_coinbase ? 1 : 0);
    if (hdr->script_len > 0 && script_bytes) {
        sqlite3_bind_blob(s, 6, script_bytes, (int)hdr->script_len,
                          SQLITE_TRANSIENT);
    } else {
        /* Schema is NOT NULL; bind empty blob. */
        sqlite3_bind_blob(s, 6, "", 0, SQLITE_STATIC);
    }
    rc = sqlite3_step(s);  // raw-sql-ok:projection-primitive
    sqlite3_finalize(s);
    return rc == SQLITE_DONE;
}

static bool apply_utxo_spend(utxo_projection_t *p,
                             const struct ev_utxo_spend *spend)
{
    sqlite3_stmt *s = NULL;
    int rc = sqlite3_prepare_v2(p->db,
        "DELETE FROM utxo WHERE txid=? AND vout=?",
        -1, &s, NULL);
    if (rc != SQLITE_OK) return false;
    sqlite3_bind_blob(s, 1, spend->txid, 32, SQLITE_TRANSIENT);
    sqlite3_bind_int (s, 2, (int)spend->vout);
    rc = sqlite3_step(s);  // raw-sql-ok:projection-primitive
    sqlite3_finalize(s);
    return rc == SQLITE_DONE;
}

struct catchup_ctx {
    utxo_projection_t *p;
    bool ok;
    uint64_t next_offset;
    uint64_t since_commit;
};

static bool catchup_cb(uint64_t offset, enum event_log_type type,
                       const void *payload, size_t len, void *user)
{
    struct catchup_ctx *ctx = user;
    utxo_projection_t *p    = ctx->p;
    uint64_t next = offset + EVENT_LOG_FRAME_OVERHEAD + (uint64_t)len;

    if (type == EV_UTXO_ADD) {
        struct ev_utxo_add_hdr hdr;
        const uint8_t *script = NULL;
        size_t script_len = 0;
        if (!ev_utxo_add_parse((const uint8_t *)payload, len, &hdr,
                                &script, &script_len) ||
            !apply_utxo_add(p, &hdr, script)) {
            fprintf(stderr,  // obs-ok:utxo-projection-apply
                    "[utxo_projection] EV_UTXO_ADD apply failed off=%" PRIu64 "\n",
                    offset);
            ctx->ok = false;
            return false;
        }
        p->ev_utxo_add_total++;
        p->events_consumed_total++;
    } else if (type == EV_UTXO_SPEND) {
        struct ev_utxo_spend spend;
        if (!ev_utxo_spend_parse(payload, len, &spend) ||
            !apply_utxo_spend(p, &spend)) {
            fprintf(stderr,  // obs-ok:utxo-projection-apply
                    "[utxo_projection] EV_UTXO_SPEND apply failed off=%" PRIu64 "\n",
                    offset);
            ctx->ok = false;
            return false;
        }
        p->ev_utxo_spend_total++;
        p->events_consumed_total++;
    }
    /* Other event types are silently skipped — this projection only
     * cares about UTXO events. The peers_projection ignores UTXO
     * events for the same reason. */

    ctx->next_offset = next;
    p->last_consumed_offset = next;
    ctx->since_commit++;
    if (ctx->since_commit >= CATCHUP_CHECKPOINT_INTERVAL) {
        /* Mid-stream cursor checkpoint inside the outer txn — bounds
         * WAL growth on very long replays. The COMMIT happens at the
         * end of catch_up. */
        if (!meta_set_u64(p->db, "last_consumed_offset", next)) {
            ctx->ok = false;
            return false;
        }
        ctx->since_commit = 0;
    }
    return true;
}

uint64_t utxo_projection_catch_up(utxo_projection_t *p)
{
    if (!p || !p->db || !p->log) return UINT64_MAX;
    int64_t start_ms = now_ms();
    struct catchup_ctx ctx = {
        .p = p,
        .ok = true,
        .next_offset = p->last_consumed_offset,
        .since_commit = 0,
    };

    if (!exec_sql(p->db, "BEGIN IMMEDIATE", "catch_up begin"))
        return UINT64_MAX;
    if (event_log_stream(p->log, p->last_consumed_offset,
                         catchup_cb, &ctx) < 0)
        ctx.ok = false;
    if (ctx.ok)
        ctx.ok = meta_set_u64(p->db, "last_consumed_offset",
                              ctx.next_offset);

    bool finish_ok = exec_sql(p->db, ctx.ok ? "COMMIT" : "ROLLBACK",
                              ctx.ok ? "catch_up commit" :
                                       "catch_up rollback");
    if (!ctx.ok || !finish_ok) {
        /* Rolled back — restore the cached offset to whatever we read
         * at open / last successful catch_up. SQLite already discarded
         * the in-flight writes on ROLLBACK. */
        p->last_consumed_offset = meta_get_u64(p->db, "last_consumed_offset");
        return UINT64_MAX;
    }
    p->last_consumed_offset = ctx.next_offset;
    int64_t elapsed = now_ms() - start_ms;
    p->last_catch_up_ms = elapsed > 0 ? (uint64_t)elapsed : 0;
    return p->last_consumed_offset;
}

/* ── Reads ─────────────────────────────────────────────────────────── */

bool utxo_projection_get(utxo_projection_t *p,
                         const uint8_t txid[32], uint32_t vout,
                         int64_t *value_out,
                         uint8_t *script_out, size_t script_cap,
                         size_t *script_len_out)
{
    if (!p || !p->db || !txid) return false;
    sqlite3_stmt *s = NULL;
    int rc = sqlite3_prepare_v2(p->db,
        "SELECT value, script FROM utxo WHERE txid=? AND vout=?",
        -1, &s, NULL);
    if (rc != SQLITE_OK) return false;
    sqlite3_bind_blob(s, 1, txid, 32, SQLITE_TRANSIENT);
    sqlite3_bind_int (s, 2, (int)vout);
    rc = sqlite3_step(s);  // raw-sql-ok:projection-primitive
    bool found = false;
    if (rc == SQLITE_ROW) {
        found = true;
        if (value_out) *value_out = sqlite3_column_int64(s, 0);
        int slen = sqlite3_column_bytes(s, 1);
        const void *sblob = sqlite3_column_blob(s, 1);
        if (script_len_out) *script_len_out = (size_t)slen;
        if (script_out && script_cap > 0 && sblob && slen > 0) {
            size_t copy = (size_t)slen < script_cap
                        ? (size_t)slen : script_cap;
            memcpy(script_out, sblob, copy);
        }
    }
    sqlite3_finalize(s);
    return found;
}

uint64_t utxo_projection_count(utxo_projection_t *p)
{
    if (!p || !p->db) return 0;
    sqlite3_stmt *s = NULL;
    uint64_t n = 0;
    if (sqlite3_prepare_v2(p->db, "SELECT COUNT(*) FROM utxo",
                           -1, &s, NULL) != SQLITE_OK)
        return 0;
    if (sqlite3_step(s) == SQLITE_ROW)  // raw-sql-ok:projection-primitive
        n = (uint64_t)sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);
    return n;
}

/* Canonical serialisation matches `utxo_commitment_sha3_compute_table`
 * in `lib/coins/src/utxo_commitment.c` exactly so the legacy and
 * projection commitments are bytewise identical when the UTXO sets
 * match. The format is:
 *
 *   txid(32) || vout_le(4) || value_le(8) ||
 *   script_len_le(4) || script(var) ||
 *   height_le(4) || is_coinbase(1)
 *
 * Walks rows in ORDER BY txid, vout. */
int utxo_projection_commitment(utxo_projection_t *p, uint8_t out[32])
{
    if (!p || !p->db || !out) return -1;

    sqlite3_stmt *s = NULL;
    int rc = sqlite3_prepare_v2(p->db,
        "SELECT txid, vout, value, script, height, is_coinbase "
        "FROM utxo ORDER BY txid, vout",
        -1, &s, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr,  // obs-ok:utxo-projection-commitment
                "[utxo_projection] commitment prepare failed: %s\n",
                sqlite3_errmsg(p->db));
        return -1;
    }

    struct sha3_256_ctx ctx;
    sha3_256_init(&ctx);

    while ((rc = sqlite3_step(s)) == SQLITE_ROW) {  // raw-sql-ok:projection-primitive
        const uint8_t *txid = (const uint8_t *)sqlite3_column_blob(s, 0);
        int txid_len = sqlite3_column_bytes(s, 0);
        if (!txid || txid_len < 32) continue;

        uint32_t vout   = (uint32_t)sqlite3_column_int(s, 1);
        int64_t  value  = sqlite3_column_int64(s, 2);
        const uint8_t *script = (const uint8_t *)sqlite3_column_blob(s, 3);
        int script_len = sqlite3_column_bytes(s, 3);
        int32_t  height = sqlite3_column_int(s, 4);
        int cb_int = sqlite3_column_int(s, 5);

        sha3_256_write(&ctx, txid, 32);

        uint8_t le4[4];
        le4[0] = (uint8_t)(vout);       le4[1] = (uint8_t)(vout >>  8);
        le4[2] = (uint8_t)(vout >> 16); le4[3] = (uint8_t)(vout >> 24);
        sha3_256_write(&ctx, le4, 4);

        uint8_t le8[8];
        uint64_t v = (uint64_t)value;
        for (int i = 0; i < 8; i++) le8[i] = (uint8_t)(v >> (8 * i));
        sha3_256_write(&ctx, le8, 8);

        uint32_t slen = (uint32_t)(script_len > 0 ? script_len : 0);
        le4[0] = (uint8_t)(slen);       le4[1] = (uint8_t)(slen >>  8);
        le4[2] = (uint8_t)(slen >> 16); le4[3] = (uint8_t)(slen >> 24);
        sha3_256_write(&ctx, le4, 4);
        if (script && script_len > 0)
            sha3_256_write(&ctx, script, (size_t)script_len);

        uint32_t ht = (uint32_t)height;
        le4[0] = (uint8_t)(ht);       le4[1] = (uint8_t)(ht >>  8);
        le4[2] = (uint8_t)(ht >> 16); le4[3] = (uint8_t)(ht >> 24);
        sha3_256_write(&ctx, le4, 4);

        uint8_t cb_byte = (uint8_t)(cb_int ? 1 : 0);
        sha3_256_write(&ctx, &cb_byte, 1);
    }
    sqlite3_finalize(s);

    if (rc != SQLITE_DONE) {
        fprintf(stderr,  // obs-ok:utxo-projection-commitment
                "[utxo_projection] commitment step rc=%d (%s)\n",
                rc, sqlite3_errstr(rc));
        return -1;
    }

    sha3_256_finalize(&ctx, out);
    return 0;
}

/* ── Shadow emission ───────────────────────────────────────────────── */

bool utxo_projection_emit_add(const uint8_t txid[32], uint32_t vout,
                              int64_t value, uint32_t height,
                              bool is_coinbase,
                              const uint8_t *script_bytes,
                              uint32_t script_len)
{
    event_log_t *log = utxo_projection_event_log();
    if (!log || !txid) return false;
    if (script_len > 0 && !script_bytes) return false;

    struct ev_utxo_add_hdr hdr;
    memset(&hdr, 0, sizeof(hdr));
    memcpy(hdr.txid, txid, 32);
    hdr.vout        = vout;
    hdr.value       = value;
    hdr.height      = height;
    hdr.is_coinbase = is_coinbase ? 1 : 0;
    hdr.script_len  = script_len;

    /* Worst-case payload size is bounded by MAX_SCRIPT_SIZE (10 KB) +
     * header, well under the event_log payload cap. Allocate to avoid a
     * VLA in this path. */
    size_t cap = (size_t)EV_UTXO_ADD_HDR_WIRE_LEN + (size_t)script_len;
    uint8_t *buf = zcl_malloc(cap, "utxo_projection/emit_add");
    if (!buf) {
        atomic_fetch_add_explicit(&g_emit_fail_total, 1,
                                  memory_order_relaxed);
        return false;
    }
    size_t len = 0;
    if (!ev_utxo_add_serialize(&hdr, script_bytes, buf, cap, &len)) {
        free(buf);
        atomic_fetch_add_explicit(&g_emit_fail_total, 1,
                                  memory_order_relaxed);
        return false;
    }
    uint64_t off = event_log_append(log, EV_UTXO_ADD, buf, len);
    free(buf);
    if (off == UINT64_MAX) {
        atomic_fetch_add_explicit(&g_emit_fail_total, 1,
                                  memory_order_relaxed);
        return false;
    }
    atomic_fetch_add_explicit(&g_emit_add_total, 1,
                              memory_order_relaxed);
    return true;
}

bool utxo_projection_emit_spend(const uint8_t txid[32], uint32_t vout)
{
    event_log_t *log = utxo_projection_event_log();
    if (!log || !txid) return false;

    struct ev_utxo_spend spend;
    memset(&spend, 0, sizeof(spend));
    memcpy(spend.txid, txid, 32);
    spend.vout = vout;

    uint8_t buf[EV_UTXO_SPEND_WIRE_LEN];
    if (!ev_utxo_spend_serialize(&spend, buf)) {
        atomic_fetch_add_explicit(&g_emit_fail_total, 1,
                                  memory_order_relaxed);
        return false;
    }
    uint64_t off = event_log_append(log, EV_UTXO_SPEND, buf, sizeof(buf));
    if (off == UINT64_MAX) {
        atomic_fetch_add_explicit(&g_emit_fail_total, 1,
                                  memory_order_relaxed);
        return false;
    }
    atomic_fetch_add_explicit(&g_emit_spend_total, 1,
                              memory_order_relaxed);
    return true;
}

/* ── State dump ────────────────────────────────────────────────────── */

bool utxo_projection_dump_state_json(struct json_value *out, const char *key)
{
    (void)key;
    if (!out) return false;
    json_set_object(out);

    utxo_projection_t *p = atomic_load_explicit(&g_projection,
                                                memory_order_acquire);
    json_push_kv_bool(out, "open", p != NULL);
    json_push_kv_int(out, "emit_add_total",
                     (int64_t)atomic_load_explicit(&g_emit_add_total,
                                                   memory_order_relaxed));
    json_push_kv_int(out, "emit_spend_total",
                     (int64_t)atomic_load_explicit(&g_emit_spend_total,
                                                   memory_order_relaxed));
    json_push_kv_int(out, "emit_fail_total",
                     (int64_t)atomic_load_explicit(&g_emit_fail_total,
                                                   memory_order_relaxed));
    if (!p) return true;

    json_push_kv_str(out, "path", p->path);
    json_push_kv_int(out, "last_consumed_offset",
                     (int64_t)p->last_consumed_offset);
    json_push_kv_int(out, "utxo_count",
                     (int64_t)utxo_projection_count(p));
    json_push_kv_int(out, "events_consumed_total",
                     (int64_t)p->events_consumed_total);
    json_push_kv_int(out, "ev_utxo_add_total",
                     (int64_t)p->ev_utxo_add_total);
    json_push_kv_int(out, "ev_utxo_spend_total",
                     (int64_t)p->ev_utxo_spend_total);
    json_push_kv_int(out, "replace_collisions_total",
                     (int64_t)p->replace_collisions_total);
    json_push_kv_int(out, "last_catch_up_ms",
                     (int64_t)p->last_catch_up_ms);
    return true;
}
