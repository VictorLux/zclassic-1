/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "storage/small_projections.h"

#include "storage/event_log_payloads.h"
#include "util/safe_alloc.h"

#include <inttypes.h>
#include <sqlite3.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CONTACTS_PROJECTION_SCHEMA_VERSION 1
#define EVENT_LOG_FRAME_OVERHEAD 32u

struct contacts_projection {
    sqlite3 *db;
    event_log_t *log;
    uint64_t last_consumed_offset;
    uint64_t events_consumed_total;
    char path[1024];
};

static _Atomic(event_log_t *) g_event_log = NULL;
static _Atomic(contacts_projection_t *) g_projection = NULL;

static bool exec_sql(sqlite3 *db, const char *sql, const char *ctx)
{
    char *err = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        fprintf(stderr,  // obs-ok:contacts-projection-sql
                "[contacts_projection] %s failed: %s\n",
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
        "CREATE TABLE IF NOT EXISTS contacts ("
        " address TEXT PRIMARY KEY,"
        " name TEXT NOT NULL DEFAULT '',"
        " last_used INTEGER NOT NULL DEFAULT 0"
        ") WITHOUT ROWID",
        "create contacts") &&
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

contacts_projection_t *contacts_projection_open(const char *path,
                                                event_log_t *log)
{
    if (!path || !path[0] || !log) {
        fprintf(stderr,  // obs-ok:contacts-projection-open
                "[contacts_projection] open: invalid args path=%p log=%p\n",
                (const void *)path, (void *)log);
        return NULL;
    }

    sqlite3 *db = NULL;
    int rc = sqlite3_open_v2(path, &db,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr,  // obs-ok:contacts-projection-open
                "[contacts_projection] sqlite open failed: %s\n",
                db ? sqlite3_errmsg(db) : sqlite3_errstr(rc));
        if (db) sqlite3_close(db);
        return NULL;
    }
    if (!apply_pragmas(db) || !ensure_schema(db)) {
        sqlite3_close(db);
        return NULL;
    }

    contacts_projection_t *p = zcl_malloc(sizeof(*p), "contacts_projection");
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

void contacts_projection_close(contacts_projection_t *p)
{
    if (!p) return;
    contacts_projection_t *cur = atomic_load_explicit(&g_projection,
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

static bool apply_contact_set(contacts_projection_t *p,
                              const struct ev_contact_set *ev)
{
    sqlite3_stmt *s = NULL;
    int rc = sqlite3_prepare_v2(p->db,
        "INSERT OR REPLACE INTO contacts(address,name,last_used) "
        "VALUES(?,?,COALESCE((SELECT last_used FROM contacts WHERE address=?),0))",
        -1, &s, NULL);
    if (rc != SQLITE_OK) return false;
    sqlite3_bind_text(s, 1, ev->address, ev->address_len, SQLITE_TRANSIENT);
    if (ev->name_len)
        sqlite3_bind_text(s, 2, ev->name, ev->name_len, SQLITE_TRANSIENT);
    else
        sqlite3_bind_text(s, 2, "", 0, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 3, ev->address, ev->address_len, SQLITE_TRANSIENT);
    rc = sqlite3_step(s);  // raw-sql-ok:projection-primitive
    sqlite3_finalize(s);
    return rc == SQLITE_DONE;
}

static bool apply_contact_touched(contacts_projection_t *p,
                                  const struct ev_contact_touched *ev)
{
    sqlite3_stmt *s = NULL;
    int rc = sqlite3_prepare_v2(p->db,
        "UPDATE contacts SET last_used=? WHERE address=?",
        -1, &s, NULL);
    if (rc != SQLITE_OK) return false;
    sqlite3_bind_int64(s, 1, ev->last_used_unix);
    sqlite3_bind_text(s, 2, ev->address, ev->address_len, SQLITE_TRANSIENT);
    rc = sqlite3_step(s);  // raw-sql-ok:projection-primitive
    sqlite3_finalize(s);
    return rc == SQLITE_DONE;
}

static bool apply_contact_delete(contacts_projection_t *p,
                                 const struct ev_contact_delete *ev)
{
    sqlite3_stmt *s = NULL;
    int rc = sqlite3_prepare_v2(p->db,
        "DELETE FROM contacts WHERE address=?",
        -1, &s, NULL);
    if (rc != SQLITE_OK) return false;
    sqlite3_bind_text(s, 1, ev->address, ev->address_len, SQLITE_TRANSIENT);
    rc = sqlite3_step(s);  // raw-sql-ok:projection-primitive
    sqlite3_finalize(s);
    return rc == SQLITE_DONE;
}

struct catchup_ctx {
    contacts_projection_t *p;
    bool ok;
    uint64_t next_offset;
    uint64_t since_commit;
    uint64_t events_consumed;
};

static bool catchup_cb(uint64_t offset, enum event_log_type type,
                       const void *payload, size_t len, void *user)
{
    struct catchup_ctx *ctx = user;
    contacts_projection_t *p = ctx->p;
    uint64_t next = offset + EVENT_LOG_FRAME_OVERHEAD + (uint64_t)len;

    if (type == EV_CONTACT_SET) {
        struct ev_contact_set ev;
        if (!ev_contact_set_parse(payload, len, &ev) ||
            !apply_contact_set(p, &ev)) {
            ctx->ok = false;
            return false;
        }
    } else if (type == EV_CONTACT_TOUCHED) {
        struct ev_contact_touched ev;
        if (!ev_contact_touched_parse(payload, len, &ev) ||
            !apply_contact_touched(p, &ev)) {
            ctx->ok = false;
            return false;
        }
    } else if (type == EV_CONTACT_DELETE) {
        struct ev_contact_delete ev;
        if (!ev_contact_delete_parse(payload, len, &ev) ||
            !apply_contact_delete(p, &ev)) {
            ctx->ok = false;
            return false;
        }
    }

    ctx->next_offset = next;
    p->last_consumed_offset = next;
    ctx->events_consumed++;
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

uint64_t contacts_projection_catch_up(contacts_projection_t *p)
{
    if (!p || !p->db || !p->log) return UINT64_MAX;
    struct catchup_ctx ctx = {
        .p = p,
        .ok = true,
        .next_offset = p->last_consumed_offset,
    };
    if (!exec_sql(p->db, "BEGIN IMMEDIATE", "begin catch_up"))
        return UINT64_MAX;
    if (event_log_stream(p->log, p->last_consumed_offset,
                         catchup_cb, &ctx) != 0)
        ctx.ok = false;
    if (ctx.ok && !meta_set_u64(p->db, "last_consumed_offset",
                                ctx.next_offset))
        ctx.ok = false;
    if (!exec_sql(p->db, ctx.ok ? "COMMIT" : "ROLLBACK",
                  ctx.ok ? "commit catch_up" : "rollback catch_up"))
        return UINT64_MAX;
    if (!ctx.ok)
        return UINT64_MAX;
    p->events_consumed_total += ctx.events_consumed;
    return p->last_consumed_offset;
}

uint64_t contacts_projection_count(contacts_projection_t *p)
{
    if (!p || !p->db) return UINT64_MAX;
    sqlite3_stmt *s = NULL;
    uint64_t count = UINT64_MAX;
    if (sqlite3_prepare_v2(p->db, "SELECT COUNT(*) FROM contacts",
                           -1, &s, NULL) != SQLITE_OK)
        return UINT64_MAX;
    if (sqlite3_step(s) == SQLITE_ROW)  // raw-sql-ok:projection-primitive
        count = (uint64_t)sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);
    return count;
}

void contacts_projection_set_event_log(event_log_t *log)
{
    atomic_store_explicit(&g_event_log, log, memory_order_release);
}

contacts_projection_t *contacts_projection_current(void)
{
    return atomic_load_explicit(&g_projection, memory_order_acquire);
}

bool contacts_projection_emit_set(const char *address, const char *name)
{
    (void)address;
    (void)name;
    return true;
}

bool contacts_projection_emit_touched(const char *address,
                                      uint32_t last_used)
{
    (void)address;
    (void)last_used;
    return true;
}

bool contacts_projection_emit_delete(const char *address)
{
    (void)address;
    return true;
}

bool contacts_projection_dump_state_json(struct json_value *out,
                                         const char *key)
{
    (void)out;
    (void)key;
    return false;
}
