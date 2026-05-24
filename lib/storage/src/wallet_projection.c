/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * wallet_projection — Phase 4d-3 event-log consumer skeleton.
 *
 * Shadow mode only. This projection stores public, rebuildable wallet
 * view state and never stores private keys, seeds, spending keys, or
 * wallet secret material.
 */

#include "storage/wallet_projection.h"

#include "json/json.h"
#include "storage/event_log_payloads.h"
#include "util/safe_alloc.h"

#include <inttypes.h>
#include <sqlite3.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WALLET_PROJECTION_SCHEMA_VERSION 1
#define EVENT_LOG_FRAME_OVERHEAD 32u

struct wallet_projection {
    sqlite3 *db;
    event_log_t *log;
    uint64_t last_consumed_offset;
    char path[1024];
};

static _Atomic(event_log_t *) g_event_log = NULL;
static _Atomic(wallet_projection_t *) g_projection = NULL;

static bool exec_sql(sqlite3 *db, const char *sql, const char *ctx)
{
    char *err = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        fprintf(stderr,  // obs-ok:wallet-projection-sql
                "[wallet_projection] %s failed: %s\n",
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
        "CREATE TABLE IF NOT EXISTS wallet_view_addresses ("
        " pubkey_hash BLOB PRIMARY KEY,"
        " address TEXT NOT NULL,"
        " label TEXT NOT NULL DEFAULT '',"
        " created_unix INTEGER NOT NULL"
        ") WITHOUT ROWID",
        "create wallet_view_addresses") &&
        exec_sql(db,
        "CREATE TABLE IF NOT EXISTS wallet_view_transactions ("
        " txid BLOB PRIMARY KEY,"
        " block_height INTEGER NOT NULL,"
        " fee INTEGER NOT NULL,"
        " from_me INTEGER NOT NULL DEFAULT 0,"
        " seen_unix INTEGER NOT NULL DEFAULT 0"
        ") WITHOUT ROWID",
        "create wallet_view_transactions") &&
        exec_sql(db,
        "CREATE TABLE IF NOT EXISTS wallet_view_utxos ("
        " txid BLOB NOT NULL,"
        " vout INTEGER NOT NULL,"
        " value INTEGER NOT NULL,"
        " address_hash BLOB NOT NULL,"
        " height INTEGER NOT NULL,"
        " is_coinbase INTEGER NOT NULL DEFAULT 0,"
        " PRIMARY KEY(txid, vout)"
        ") WITHOUT ROWID",
        "create wallet_view_utxos") &&
        exec_sql(db,
        "CREATE TABLE IF NOT EXISTS wallet_view_notes ("
        " txid BLOB NOT NULL,"
        " output_index INTEGER NOT NULL,"
        " value INTEGER NOT NULL,"
        " cm BLOB NOT NULL,"
        " block_height INTEGER NOT NULL,"
        " PRIMARY KEY(txid, output_index)"
        ") WITHOUT ROWID",
        "create wallet_view_notes") &&
        exec_sql(db,
        "CREATE TABLE IF NOT EXISTS projection_meta ("
        " k TEXT PRIMARY KEY,"
        " v TEXT NOT NULL"
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

wallet_projection_t *wallet_projection_open(const char *projection_path,
                                            event_log_t *log)
{
    if (!projection_path || !projection_path[0] || !log) {
        fprintf(stderr,  // obs-ok:wallet-projection-open
                "[wallet_projection] open: invalid args path=%p log=%p\n",
                (const void *)projection_path, (void *)log);
        return NULL;
    }

    sqlite3 *db = NULL;
    int rc = sqlite3_open_v2(projection_path, &db,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr,  // obs-ok:wallet-projection-open
                "[wallet_projection] sqlite open failed: %s\n",
                db ? sqlite3_errmsg(db) : sqlite3_errstr(rc));
        if (db) sqlite3_close(db);
        return NULL;
    }
    if (!apply_pragmas(db) || !ensure_schema(db)) {
        sqlite3_close(db);
        return NULL;
    }

    wallet_projection_t *p = zcl_malloc(sizeof(*p), "wallet_projection");
    if (!p) {
        sqlite3_close(db);
        return NULL;
    }
    memset(p, 0, sizeof(*p));
    p->db = db;
    p->log = log;
    p->last_consumed_offset = meta_get_u64(db, "last_consumed_offset");
    snprintf(p->path, sizeof(p->path), "%s", projection_path);
    atomic_store_explicit(&g_projection, p, memory_order_release);
    return p;
}

void wallet_projection_close(wallet_projection_t *p)
{
    if (!p) return;
    wallet_projection_t *cur = atomic_load_explicit(&g_projection,
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

static bool apply_key_add(wallet_projection_t *p,
                          const struct ev_wallet_key_add *ev)
{
    char address[EV_WALLET_ADDRESS_MAX + 1];
    char label[EV_WALLET_LABEL_MAX + 1];
    memset(address, 0, sizeof(address));
    memset(label, 0, sizeof(label));
    if (ev->address_len && ev->address)
        memcpy(address, ev->address, ev->address_len);
    if (ev->label_len && ev->label)
        memcpy(label, ev->label, ev->label_len);

    sqlite3_stmt *s = NULL;
    int rc = sqlite3_prepare_v2(p->db,
        "INSERT OR REPLACE INTO wallet_view_addresses"
        "(pubkey_hash,address,label,created_unix) VALUES(?,?,?,?)",
        -1, &s, NULL);
    if (rc != SQLITE_OK) return false;
    sqlite3_bind_blob(s, 1, ev->pubkey_hash, 20, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 2, address, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 3, label, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s, 4, (sqlite3_int64)ev->created_unix);
    rc = sqlite3_step(s);  // raw-sql-ok:projection-primitive
    sqlite3_finalize(s);
    return rc == SQLITE_DONE;
}

static bool apply_addr_derived(wallet_projection_t *p,
                               const struct ev_wallet_addr_derived *ev)
{
    sqlite3_stmt *s = NULL;
    int rc = sqlite3_prepare_v2(p->db,
        "INSERT OR REPLACE INTO wallet_view_addresses"
        "(pubkey_hash,address,label,created_unix) VALUES(?,'','',?)",
        -1, &s, NULL);
    if (rc != SQLITE_OK) return false;
    sqlite3_bind_blob(s, 1, ev->derived_pubkey_hash, 20, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s, 2, (sqlite3_int64)ev->derived_unix);
    rc = sqlite3_step(s);  // raw-sql-ok:projection-primitive
    sqlite3_finalize(s);
    return rc == SQLITE_DONE;
}

static bool apply_tx_seen(wallet_projection_t *p,
                          const struct ev_wallet_tx_seen *ev)
{
    sqlite3_stmt *s = NULL;
    int rc = sqlite3_prepare_v2(p->db,
        "INSERT OR REPLACE INTO wallet_view_transactions"
        "(txid,block_height,fee,from_me,seen_unix) VALUES(?,?,?,?,0)",
        -1, &s, NULL);
    if (rc != SQLITE_OK) return false;
    sqlite3_bind_blob(s, 1, ev->txid, 32, SQLITE_TRANSIENT);
    sqlite3_bind_int(s, 2, ev->block_height);
    sqlite3_bind_int64(s, 3, (sqlite3_int64)ev->fee);
    sqlite3_bind_int(s, 4, ev->from_me ? 1 : 0);
    rc = sqlite3_step(s);  // raw-sql-ok:projection-primitive
    sqlite3_finalize(s);
    return rc == SQLITE_DONE;
}

static bool apply_note_decrypted(
    wallet_projection_t *p,
    const struct ev_wallet_note_decrypted *ev)
{
    sqlite3_stmt *s = NULL;
    int rc = sqlite3_prepare_v2(p->db,
        "INSERT OR REPLACE INTO wallet_view_notes"
        "(txid,output_index,value,cm,block_height) VALUES(?,?,?,?,?)",
        -1, &s, NULL);
    if (rc != SQLITE_OK) return false;
    sqlite3_bind_blob(s, 1, ev->txid, 32, SQLITE_TRANSIENT);
    sqlite3_bind_int(s, 2, (int)ev->output_index);
    sqlite3_bind_int64(s, 3, (sqlite3_int64)ev->value);
    sqlite3_bind_blob(s, 4, ev->cm, 32, SQLITE_TRANSIENT);
    sqlite3_bind_int(s, 5, ev->block_height);
    rc = sqlite3_step(s);  // raw-sql-ok:projection-primitive
    sqlite3_finalize(s);
    return rc == SQLITE_DONE;
}

struct catchup_ctx {
    wallet_projection_t *p;
    bool ok;
    uint64_t next_offset;
    uint64_t since_commit;
};

static bool catchup_cb(uint64_t offset, enum event_log_type type,
                       const void *payload, size_t len, void *user)
{
    struct catchup_ctx *ctx = user;
    wallet_projection_t *p = ctx->p;
    uint64_t next = offset + EVENT_LOG_FRAME_OVERHEAD + (uint64_t)len;

    if (type == EV_WALLET_KEY_ADD) {
        struct ev_wallet_key_add ev;
        if (!ev_wallet_key_add_parse(payload, len, &ev) ||
            !apply_key_add(p, &ev)) {
            ctx->ok = false;
            return false;
        }
    } else if (type == EV_WALLET_ADDR_DERIVED) {
        struct ev_wallet_addr_derived ev;
        if (!ev_wallet_addr_derived_parse(payload, len, &ev) ||
            !apply_addr_derived(p, &ev)) {
            ctx->ok = false;
            return false;
        }
    } else if (type == EV_WALLET_TX_SEEN) {
        struct ev_wallet_tx_seen ev;
        if (!ev_wallet_tx_seen_parse(payload, len, &ev) ||
            !apply_tx_seen(p, &ev)) {
            ctx->ok = false;
            return false;
        }
    } else if (type == EV_WALLET_NOTE_DECRYPTED) {
        struct ev_wallet_note_decrypted ev;
        if (!ev_wallet_note_decrypted_parse(payload, len, &ev) ||
            !apply_note_decrypted(p, &ev)) {
            ctx->ok = false;
            return false;
        }
    }

    ctx->next_offset = next;
    p->last_consumed_offset = next;
    ctx->since_commit++;
    if (ctx->since_commit >= 100) {
        if (!meta_set_u64(p->db, "last_consumed_offset", next)) {
            ctx->ok = false;
            return false;
        }
        ctx->since_commit = 0;
    }
    return true;
}

uint64_t wallet_projection_catch_up(wallet_projection_t *p)
{
    if (!p || !p->db || !p->log) return UINT64_MAX;
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
    if (!ctx.ok || !finish_ok)
        return UINT64_MAX;
    p->last_consumed_offset = ctx.next_offset;
    return p->last_consumed_offset;
}

static uint64_t count_table(wallet_projection_t *p, const char *sql)
{
    if (!p || !p->db) return 0;
    sqlite3_stmt *s = NULL;
    uint64_t n = 0;
    if (sqlite3_prepare_v2(p->db, sql, -1, &s, NULL) != SQLITE_OK)
        return 0;
    if (sqlite3_step(s) == SQLITE_ROW)  // raw-sql-ok:projection-primitive
        n = (uint64_t)sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);
    return n;
}

uint64_t wallet_projection_address_count(wallet_projection_t *p)
{
    return count_table(p, "SELECT COUNT(*) FROM wallet_view_addresses");
}

uint64_t wallet_projection_tx_count(wallet_projection_t *p)
{
    return count_table(p, "SELECT COUNT(*) FROM wallet_view_transactions");
}

uint64_t wallet_projection_utxo_count(wallet_projection_t *p)
{
    return count_table(p, "SELECT COUNT(*) FROM wallet_view_utxos");
}

uint64_t wallet_projection_note_count(wallet_projection_t *p)
{
    return count_table(p, "SELECT COUNT(*) FROM wallet_view_notes");
}

int64_t wallet_projection_total_value_zat(wallet_projection_t *p)
{
    if (!p || !p->db) return 0;
    sqlite3_stmt *s = NULL;
    int64_t total = 0;
    int rc = sqlite3_prepare_v2(p->db,
        "SELECT "
        " COALESCE((SELECT SUM(value) FROM wallet_view_utxos),0) +"
        " COALESCE((SELECT SUM(value) FROM wallet_view_notes),0)",
        -1, &s, NULL);
    if (rc != SQLITE_OK) return 0;
    if (sqlite3_step(s) == SQLITE_ROW)  // raw-sql-ok:projection-primitive
        total = (int64_t)sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);
    return total;
}

void wallet_projection_set_event_log(event_log_t *log)
{
    atomic_store_explicit(&g_event_log, log, memory_order_release);
}

event_log_t *wallet_projection_event_log(void)
{
    return atomic_load_explicit(&g_event_log, memory_order_acquire);
}

wallet_projection_t *wallet_projection_current(void)
{
    return atomic_load_explicit(&g_projection, memory_order_acquire);
}

bool wallet_projection_dump_state_json(struct json_value *out,
                                       const char *key)
{
    (void)out;
    (void)key;
    return false;
}
