/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "storage/small_projections.h"

#include "util/safe_alloc.h"

#include <sqlite3.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct onion_ann_projection {
    sqlite3 *db;
    event_log_t *log;
    uint64_t last_consumed_offset;
    uint64_t events_consumed_total;
    char path[1024];
};

static _Atomic(event_log_t *) g_event_log = NULL;
static _Atomic(onion_ann_projection_t *) g_projection = NULL;

static bool exec_sql(sqlite3 *db, const char *sql, const char *ctx)
{
    char *err = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        fprintf(stderr,  // obs-ok:onion-ann-projection-sql
                "[onion_ann_projection] %s failed: %s\n",
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
        "CREATE TABLE IF NOT EXISTS onion_announcements ("
        " onion_address TEXT PRIMARY KEY,"
        " announced_at INTEGER NOT NULL,"
        " script_hex TEXT NOT NULL DEFAULT ''"
        ") WITHOUT ROWID",
        "create onion_announcements") &&
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

onion_ann_projection_t *onion_ann_projection_open(const char *path,
                                                  event_log_t *log)
{
    if (!path || !path[0] || !log) {
        fprintf(stderr,  // obs-ok:onion-ann-projection-open
                "[onion_ann_projection] open: invalid args path=%p log=%p\n",
                (const void *)path, (void *)log);
        return NULL;
    }

    sqlite3 *db = NULL;
    int rc = sqlite3_open_v2(path, &db,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr,  // obs-ok:onion-ann-projection-open
                "[onion_ann_projection] sqlite open failed: %s\n",
                db ? sqlite3_errmsg(db) : sqlite3_errstr(rc));
        if (db) sqlite3_close(db);
        return NULL;
    }
    if (!apply_pragmas(db) || !ensure_schema(db)) {
        sqlite3_close(db);
        return NULL;
    }

    onion_ann_projection_t *p =
        zcl_malloc(sizeof(*p), "onion_ann_projection");
    if (!p) {
        sqlite3_close(db);
        return NULL;
    }
    memset(p, 0, sizeof(*p));
    p->db = db;
    p->log = log;
    p->last_consumed_offset = meta_get_u64(db, "last_consumed_offset");
    snprintf(p->path, sizeof(p->path), "%s", path);
    atomic_store_explicit(&g_projection, p, memory_order_release);
    return p;
}

void onion_ann_projection_close(onion_ann_projection_t *p)
{
    if (!p) return;
    onion_ann_projection_t *cur =
        atomic_load_explicit(&g_projection, memory_order_acquire);
    if (cur == p)
        atomic_store_explicit(&g_projection, NULL, memory_order_release);
    if (p->db) {
        sqlite3_exec(p->db, "PRAGMA wal_checkpoint(TRUNCATE)",
                     NULL, NULL, NULL);
        sqlite3_close(p->db);
    }
    free(p);
}

uint64_t onion_ann_projection_catch_up(onion_ann_projection_t *p)
{
    if (!p || !p->db || !p->log) return UINT64_MAX;
    return p->last_consumed_offset;
}

uint64_t onion_ann_projection_count(onion_ann_projection_t *p)
{
    if (!p || !p->db) return UINT64_MAX;
    sqlite3_stmt *s = NULL;
    uint64_t count = UINT64_MAX;
    if (sqlite3_prepare_v2(p->db,
                           "SELECT COUNT(*) FROM onion_announcements",
                           -1, &s, NULL) != SQLITE_OK)
        return UINT64_MAX;
    if (sqlite3_step(s) == SQLITE_ROW)  // raw-sql-ok:projection-primitive
        count = (uint64_t)sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);
    return count;
}

void onion_ann_projection_set_event_log(event_log_t *log)
{
    atomic_store_explicit(&g_event_log, log, memory_order_release);
}

onion_ann_projection_t *onion_ann_projection_current(void)
{
    return atomic_load_explicit(&g_projection, memory_order_acquire);
}

bool onion_ann_projection_emit(const char *onion_address,
                               uint32_t announced_at,
                               const char *script_hex)
{
    (void)onion_address;
    (void)announced_at;
    (void)script_hex;
    return true;
}

bool onion_ann_projection_dump_state_json(struct json_value *out,
                                          const char *key)
{
    (void)out;
    (void)key;
    return false;
}
