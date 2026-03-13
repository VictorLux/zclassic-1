/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "models/peer.h"
#include <string.h>

/* ── Callbacks ─────────────────────────────────────────────────── */

static struct ar_callbacks peer_cbs;
static bool peer_cbs_init;

struct ar_callbacks *db_peer_callbacks(void)
{
    if (!peer_cbs_init) {
        ar_callbacks_init(&peer_cbs);
        peer_cbs_init = true;
    }
    return &peer_cbs;
}

bool db_peer_validate(const struct db_peer *p, struct ar_errors *errors)
{
    ar_errors_clear(errors);
    validates_presence_of(errors, p, ip);
    if (p->port == 0)
        ar_errors_add(errors, "port", "can't be zero");
    return !ar_errors_any(errors);
}

bool db_peer_save(struct node_db *ndb, const struct db_peer *p)
{
    if (!ndb->open) return false;
    if (!ar_run_before_save(&peer_cbs, (void *)p)) return false;
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ndb->db,
        "INSERT OR REPLACE INTO peers"
        "(ip,port,services,last_seen,last_try,attempts,source)"
        " VALUES(?,?,?,?,?,?,?)",
        -1, &s, NULL);
    sqlite3_bind_blob(s, 1, p->ip, 16, SQLITE_STATIC);
    sqlite3_bind_int(s, 2, p->port);
    sqlite3_bind_int64(s, 3, (int64_t)p->services);
    sqlite3_bind_int64(s, 4, p->last_seen);
    sqlite3_bind_int64(s, 5, p->last_try);
    sqlite3_bind_int(s, 6, p->attempts);
    if (p->has_source)
        sqlite3_bind_blob(s, 7, p->source, 16, SQLITE_STATIC);
    else
        sqlite3_bind_null(s, 7);
    int rc = sqlite3_step(s);
    sqlite3_finalize(s);
    bool ok = rc == SQLITE_DONE;
    if (ok) ar_run_after_save(&peer_cbs, (void *)p);
    return ok;
}

static void row_to_peer(sqlite3_stmt *s, struct db_peer *out, int col_offset)
{
    out->id = sqlite3_column_int64(s, col_offset);
    const void *ip = sqlite3_column_blob(s, col_offset + 1);
    if (ip) memcpy(out->ip, ip, 16);
    out->port = (uint16_t)sqlite3_column_int(s, col_offset + 2);
    out->services = (uint64_t)sqlite3_column_int64(s, col_offset + 3);
    out->last_seen = sqlite3_column_int64(s, col_offset + 4);
    out->last_try = sqlite3_column_int64(s, col_offset + 5);
    out->attempts = sqlite3_column_int(s, col_offset + 6);
    const void *src = sqlite3_column_blob(s, col_offset + 7);
    if (src && sqlite3_column_bytes(s, col_offset + 7) >= 16) {
        memcpy(out->source, src, 16);
        out->has_source = true;
    }
}

bool db_peer_find_by_addr(struct node_db *ndb,
                          const uint8_t ip[16], uint16_t port,
                          struct db_peer *out)
{
    if (!ndb->open) return false;
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ndb->db,
        "SELECT id,ip,port,services,last_seen,last_try,attempts,source"
        " FROM peers WHERE ip=? AND port=?",
        -1, &s, NULL);
    sqlite3_bind_blob(s, 1, ip, 16, SQLITE_STATIC);
    sqlite3_bind_int(s, 2, port);
    if (sqlite3_step(s) != SQLITE_ROW) {
        sqlite3_finalize(s);
        return false;
    }
    memset(out, 0, sizeof(*out));
    row_to_peer(s, out, 0);
    sqlite3_finalize(s);
    return true;
}

bool db_peer_delete(struct node_db *ndb, const uint8_t ip[16], uint16_t port)
{
    if (!ndb->open) return false;

    struct ar_callbacks *cbs = db_peer_callbacks();
    struct db_peer p;
    memset(&p, 0, sizeof(p));
    memcpy(p.ip, ip, 16);
    p.port = port;
    if (!ar_run_before_destroy(cbs, &p)) return false;

    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ndb->db,
        "DELETE FROM peers WHERE ip=? AND port=?", -1, &s, NULL);
    sqlite3_bind_blob(s, 1, ip, 16, SQLITE_STATIC);
    sqlite3_bind_int(s, 2, port);
    int rc = sqlite3_step(s);
    sqlite3_finalize(s);

    bool ok = rc == SQLITE_DONE;
    if (ok) ar_run_after_destroy(cbs, &p);
    return ok;
}

int db_peer_count(struct node_db *ndb)
{
    if (!ndb->open) return 0;
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ndb->db,
        "SELECT COUNT(*) FROM peers", -1, &s, NULL);
    int c = 0;
    if (sqlite3_step(s) == SQLITE_ROW)
        c = sqlite3_column_int(s, 0);
    sqlite3_finalize(s);
    return c;
}

int db_peer_recent(struct node_db *ndb, struct db_peer *out, size_t max)
{
    if (!ndb->open) return 0;
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ndb->db,
        "SELECT id,ip,port,services,last_seen,last_try,attempts,source"
        " FROM peers ORDER BY last_seen DESC LIMIT ?",
        -1, &s, NULL);
    sqlite3_bind_int(s, 1, (int)max);
    int count = 0;
    while (sqlite3_step(s) == SQLITE_ROW && (size_t)count < max) {
        memset(&out[count], 0, sizeof(out[count]));
        row_to_peer(s, &out[count], 0);
        count++;
    }
    sqlite3_finalize(s);
    return count;
}

int db_peer_to_try(struct node_db *ndb, struct db_peer *out, size_t max)
{
    if (!ndb->open) return 0;
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ndb->db,
        "SELECT id,ip,port,services,last_seen,last_try,attempts,source"
        " FROM peers ORDER BY last_try ASC, last_seen DESC LIMIT ?",
        -1, &s, NULL);
    sqlite3_bind_int(s, 1, (int)max);
    int count = 0;
    while (sqlite3_step(s) == SQLITE_ROW && (size_t)count < max) {
        memset(&out[count], 0, sizeof(out[count]));
        row_to_peer(s, &out[count], 0);
        count++;
    }
    sqlite3_finalize(s);
    return count;
}

bool db_peer_mark_tried(struct node_db *ndb,
                        const uint8_t ip[16], uint16_t port)
{
    if (!ndb->open) return false;
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ndb->db,
        "UPDATE peers SET last_try=strftime('%%s','now'),"
        " attempts=attempts+1 WHERE ip=? AND port=?",
        -1, &s, NULL);
    sqlite3_bind_blob(s, 1, ip, 16, SQLITE_STATIC);
    sqlite3_bind_int(s, 2, port);
    int rc = sqlite3_step(s);
    sqlite3_finalize(s);
    return rc == SQLITE_DONE;
}

bool db_peer_mark_seen(struct node_db *ndb,
                       const uint8_t ip[16], uint16_t port,
                       int64_t now)
{
    if (!ndb->open) return false;
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ndb->db,
        "UPDATE peers SET last_seen=?,attempts=0 WHERE ip=? AND port=?",
        -1, &s, NULL);
    sqlite3_bind_int64(s, 1, now);
    sqlite3_bind_blob(s, 2, ip, 16, SQLITE_STATIC);
    sqlite3_bind_int(s, 3, port);
    int rc = sqlite3_step(s);
    sqlite3_finalize(s);
    return rc == SQLITE_DONE;
}
