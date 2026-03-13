/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "models/mempool_entry.h"
#include <string.h>
#include <stdlib.h>

/* ── Callbacks ─────────────────────────────────────────────────── */

static struct ar_callbacks mempool_cbs;
static bool mempool_cbs_init;

struct ar_callbacks *db_mempool_callbacks(void)
{
    if (!mempool_cbs_init) {
        ar_callbacks_init(&mempool_cbs);
        mempool_cbs_init = true;
    }
    return &mempool_cbs;
}

bool db_mempool_validate(const struct db_mempool_entry *e,
                         struct ar_errors *errors)
{
    ar_errors_clear(errors);
    validates_presence_of(errors, e, txid);
    if (e->size <= 0)
        ar_errors_add(errors, "size", "must be positive");
    return !ar_errors_any(errors);
}

bool db_mempool_save(struct node_db *ndb, const struct db_mempool_entry *e)
{
    if (!ndb->open) return false;
    if (!ar_run_before_save(&mempool_cbs, (void *)e)) return false;
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ndb->db,
        "INSERT OR REPLACE INTO mempool"
        "(txid,raw_tx,fee,size,time_added,height_added,spends_coinbase)"
        " VALUES(?,?,?,?,?,?,?)",
        -1, &s, NULL);
    sqlite3_bind_blob(s, 1, e->txid, 32, SQLITE_STATIC);
    sqlite3_bind_blob(s, 2, e->raw_tx, (int)e->raw_tx_len, SQLITE_STATIC);
    sqlite3_bind_int64(s, 3, e->fee);
    sqlite3_bind_int(s, 4, e->size);
    sqlite3_bind_int64(s, 5, e->time_added);
    sqlite3_bind_int(s, 6, e->height_added);
    sqlite3_bind_int(s, 7, e->spends_coinbase ? 1 : 0);
    int rc = sqlite3_step(s);
    sqlite3_finalize(s);
    bool ok = rc == SQLITE_DONE;
    if (ok) ar_run_after_save(&mempool_cbs, (void *)e);
    return ok;
}

bool db_mempool_find(struct node_db *ndb, const uint8_t txid[32],
                     struct db_mempool_entry *out)
{
    if (!ndb->open) return false;
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ndb->db,
        "SELECT raw_tx,fee,size,time_added,height_added,spends_coinbase"
        " FROM mempool WHERE txid=?",
        -1, &s, NULL);
    sqlite3_bind_blob(s, 1, txid, 32, SQLITE_STATIC);
    if (sqlite3_step(s) != SQLITE_ROW) {
        sqlite3_finalize(s);
        return false;
    }
    memset(out, 0, sizeof(*out));
    memcpy(out->txid, txid, 32);
    out->raw_tx_len = (size_t)sqlite3_column_bytes(s, 0);
    const void *rt = sqlite3_column_blob(s, 0);
    if (rt && out->raw_tx_len > 0) {
        out->raw_tx = malloc(out->raw_tx_len);
        if (out->raw_tx)
            memcpy(out->raw_tx, rt, out->raw_tx_len);
    }
    out->fee = sqlite3_column_int64(s, 1);
    out->size = sqlite3_column_int(s, 2);
    out->time_added = sqlite3_column_int64(s, 3);
    out->height_added = sqlite3_column_int(s, 4);
    out->spends_coinbase = sqlite3_column_int(s, 5) != 0;
    sqlite3_finalize(s);
    return true;
}

bool db_mempool_delete(struct node_db *ndb, const uint8_t txid[32])
{
    if (!ndb->open) return false;
    db_mempool_remove_spends(ndb, txid);
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ndb->db,
        "DELETE FROM mempool WHERE txid=?", -1, &s, NULL);
    sqlite3_bind_blob(s, 1, txid, 32, SQLITE_STATIC);
    int rc = sqlite3_step(s);
    sqlite3_finalize(s);
    return rc == SQLITE_DONE;
}

int db_mempool_count(struct node_db *ndb)
{
    if (!ndb->open) return 0;
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ndb->db,
        "SELECT COUNT(*) FROM mempool", -1, &s, NULL);
    int c = 0;
    if (sqlite3_step(s) == SQLITE_ROW)
        c = sqlite3_column_int(s, 0);
    sqlite3_finalize(s);
    return c;
}

int64_t db_mempool_total_size(struct node_db *ndb)
{
    if (!ndb->open) return 0;
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ndb->db,
        "SELECT COALESCE(SUM(size),0) FROM mempool", -1, &s, NULL);
    int64_t sz = 0;
    if (sqlite3_step(s) == SQLITE_ROW)
        sz = sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);
    return sz;
}

bool db_mempool_clear(struct node_db *ndb)
{
    if (!ndb->open) return false;
    node_db_exec(ndb, "DELETE FROM mempool_spends");
    return node_db_exec(ndb, "DELETE FROM mempool");
}

bool db_mempool_is_spent(struct node_db *ndb,
                         const uint8_t txid[32], uint32_t vout)
{
    if (!ndb->open) return false;
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ndb->db,
        "SELECT 1 FROM mempool_spends WHERE spent_txid=? AND spent_vout=?",
        -1, &s, NULL);
    sqlite3_bind_blob(s, 1, txid, 32, SQLITE_STATIC);
    sqlite3_bind_int(s, 2, (int)vout);
    bool found = sqlite3_step(s) == SQLITE_ROW;
    sqlite3_finalize(s);
    return found;
}

bool db_mempool_add_spend(struct node_db *ndb,
                          const uint8_t spending_txid[32],
                          const uint8_t spent_txid[32], uint32_t spent_vout)
{
    if (!ndb->open) return false;
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ndb->db,
        "INSERT OR REPLACE INTO mempool_spends(txid,spent_txid,spent_vout)"
        " VALUES(?,?,?)",
        -1, &s, NULL);
    sqlite3_bind_blob(s, 1, spending_txid, 32, SQLITE_STATIC);
    sqlite3_bind_blob(s, 2, spent_txid, 32, SQLITE_STATIC);
    sqlite3_bind_int(s, 3, (int)spent_vout);
    int rc = sqlite3_step(s);
    sqlite3_finalize(s);
    return rc == SQLITE_DONE;
}

bool db_mempool_remove_spends(struct node_db *ndb, const uint8_t txid[32])
{
    if (!ndb->open) return false;
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ndb->db,
        "DELETE FROM mempool_spends WHERE txid=?", -1, &s, NULL);
    sqlite3_bind_blob(s, 1, txid, 32, SQLITE_STATIC);
    int rc = sqlite3_step(s);
    sqlite3_finalize(s);
    return rc == SQLITE_DONE;
}

int db_mempool_each(struct node_db *ndb, mempool_entry_cb cb, void *ctx)
{
    if (!ndb->open) return 0;
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ndb->db,
        "SELECT txid,raw_tx,fee,size,time_added,height_added,spends_coinbase"
        " FROM mempool ORDER BY fee DESC",
        -1, &s, NULL);
    int count = 0;
    while (sqlite3_step(s) == SQLITE_ROW) {
        struct db_mempool_entry e;
        memset(&e, 0, sizeof(e));
        const void *t = sqlite3_column_blob(s, 0);
        if (t) memcpy(e.txid, t, 32);
        e.raw_tx_len = (size_t)sqlite3_column_bytes(s, 1);
        const void *rt = sqlite3_column_blob(s, 1);
        if (rt && e.raw_tx_len > 0) {
            e.raw_tx = malloc(e.raw_tx_len);
            if (e.raw_tx)
                memcpy(e.raw_tx, rt, e.raw_tx_len);
        }
        e.fee = sqlite3_column_int64(s, 2);
        e.size = sqlite3_column_int(s, 3);
        e.time_added = sqlite3_column_int64(s, 4);
        e.height_added = sqlite3_column_int(s, 5);
        e.spends_coinbase = sqlite3_column_int(s, 6) != 0;
        cb(&e, ctx);
        free(e.raw_tx);
        count++;
    }
    sqlite3_finalize(s);
    return count;
}
