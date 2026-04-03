/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * ActiveRecord model: MempoolEntry
 *
 * validates :txid, presence: true
 * validates :size, positive: true, max: 2_000_000
 * validates :fee, numericality: { >= 0 }
 * validates :time_added, positive: true
 * validates :height_added, numericality: { >= 0 } */

#include "models/mempool_entry.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>

/* ── Callbacks ─────────────────────────────────────────────────── */

DEFINE_MODEL_CALLBACKS(mempool)

/* ── Validation ────────────────────────────────────────────────── */

bool db_mempool_validate(const struct db_mempool_entry *e,
                         struct ar_errors *errors)
{
    ar_errors_clear(errors);
    validates_presence_of(errors, e, txid);
    validates_positive(errors, e, size);
    validates_max(errors, e, size, 2000000);
    validates_custom(errors,
        !(e->raw_tx && e->raw_tx_len == 0),
        "raw_tx_len", "must be positive when raw_tx present");
    validates_custom(errors,
        e->raw_tx_len <= (size_t)INT32_MAX,
        "raw_tx_len", "exceeds max size");
    validates_non_negative(errors, e, fee);
    validates_positive(errors, e, time_added);
    validates_non_negative(errors, e, height_added);
    return !ar_errors_any(errors);
}

/* ── Save ─────────────────────────────────────────────────────── */

bool db_mempool_save(struct node_db *ndb, const struct db_mempool_entry *e)
{
    if (!ndb->open) return false;
    if (e->time_added == 0)
        ((struct db_mempool_entry *)e)->time_added = (int64_t)time(NULL);

    struct ar_callbacks *cbs = db_mempool_callbacks();
    AR_BEGIN_SAVE(cbs, "mempool_entry", e, db_mempool_validate);

    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ndb->db,
        "INSERT OR REPLACE INTO mempool"
        "(txid,raw_tx,fee,size,time_added,height_added,spends_coinbase)"
        " VALUES(?,?,?,?,?,?,?)",
        -1, &s, NULL);
    if (!s) return false;
    AR_BIND_BLOB(s, 1, e->txid, 32);
    AR_BIND_BLOB(s, 2, e->raw_tx, (int)e->raw_tx_len);
    AR_BIND_INT(s, 3, e->fee);
    AR_BIND_INT(s, 4, e->size);
    AR_BIND_INT(s, 5, e->time_added);
    AR_BIND_INT(s, 6, e->height_added);
    AR_BIND_INT(s, 7, e->spends_coinbase ? 1 : 0);
    bool ok = AR_STEP_DONE(s);
    AR_FINALIZE(s);
    AR_FINISH_SAVE(cbs, e, ok);
}

/* ── Find ─────────────────────────────────────────────────────── */

bool db_mempool_find(struct node_db *ndb, const uint8_t txid[32],
                     struct db_mempool_entry *out)
{
    if (!ndb->open) return false;
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ndb->db,
        "SELECT raw_tx,fee,size,time_added,height_added,spends_coinbase"
        " FROM mempool WHERE txid=?",
        -1, &s, NULL);
    if (!s) return false;
    AR_BIND_BLOB(s, 1, txid, 32);
    if (!AR_STEP_ROW(s)) { AR_FINALIZE(s); return false; }

    memset(out, 0, sizeof(*out));
    memcpy(out->txid, txid, 32);
    out->raw_tx_len = (size_t)AR_COL_BYTES(s, 0);
    const void *rt = sqlite3_column_blob(s, 0);
    if (rt && out->raw_tx_len > 0) {
        out->raw_tx = malloc(out->raw_tx_len);
        if (out->raw_tx)
            memcpy(out->raw_tx, rt, out->raw_tx_len);
    }
    out->fee = AR_COL_INT(s, 1);
    out->size = (int)AR_COL_INT(s, 2);
    out->time_added = AR_COL_INT(s, 3);
    out->height_added = (int)AR_COL_INT(s, 4);
    out->spends_coinbase = AR_COL_INT(s, 5) != 0;
    AR_FINALIZE(s);
    return true;
}

void db_mempool_entry_free(struct db_mempool_entry *e)
{
    if (!e) return;
    free(e->raw_tx);
    e->raw_tx = NULL;
    e->raw_tx_len = 0;
}

/* ── Delete ───────────────────────────────────────────────────── */

bool db_mempool_delete(struct node_db *ndb, const uint8_t txid[32])
{
    if (!ndb->open) return false;

    struct ar_callbacks *cbs = db_mempool_callbacks();
    struct db_mempool_entry e;
    memset(&e, 0, sizeof(e));
    memcpy(e.txid, txid, 32);

    db_mempool_remove_spends(ndb, txid);
    sqlite3_stmt *s = NULL;
    AR_ADHOC_DESTROY(ndb, s, "DELETE FROM mempool WHERE txid=?",
        cbs, &e, AR_BIND_BLOB(s, 1, txid, 32));
}

/* ── Count / Aggregate ────────────────────────────────────────── */

int db_mempool_count(struct node_db *ndb)
{
    if (!ndb->open) return 0;
    AR_QUERY_COUNT_SQL(ndb, "SELECT COUNT(*) FROM mempool");
}

int64_t db_mempool_total_size(struct node_db *ndb)
{
    if (!ndb->open) return 0;
    AR_QUERY_INT64_SQL(ndb, "SELECT COALESCE(SUM(size),0) FROM mempool");
}

bool db_mempool_clear(struct node_db *ndb)
{
    if (!ndb->open) return false;
    node_db_exec(ndb, "DELETE FROM mempool_spends");
    return node_db_exec(ndb, "DELETE FROM mempool");
}

/* ── Spend Tracking ───────────────────────────────────────────── */

bool db_mempool_is_spent(struct node_db *ndb,
                         const uint8_t txid[32], uint32_t vout)
{
    if (!ndb->open) return false;
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ndb->db,
        "SELECT 1 FROM mempool_spends WHERE spent_txid=? AND spent_vout=?",
        -1, &s, NULL);
    if (!s) return false;
    AR_BIND_BLOB(s, 1, txid, 32);
    AR_BIND_INT(s, 2, (int)vout);
    bool found = AR_STEP_ROW(s);
    AR_FINALIZE(s);
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
    if (!s) return false;
    AR_BIND_BLOB(s, 1, spending_txid, 32);
    AR_BIND_BLOB(s, 2, spent_txid, 32);
    AR_BIND_INT(s, 3, (int)spent_vout);
    bool ok = AR_STEP_DONE(s);
    AR_FINALIZE(s);
    return ok;
}

bool db_mempool_remove_spends(struct node_db *ndb, const uint8_t txid[32])
{
    if (!ndb->open) return false;
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ndb->db,
        "DELETE FROM mempool_spends WHERE txid=?", -1, &s, NULL);
    if (!s) return false;
    AR_BIND_BLOB(s, 1, txid, 32);
    bool ok = AR_STEP_DONE(s);
    AR_FINALIZE(s);
    return ok;
}

/* ── Each (iteration) ─────────────────────────────────────────── */

int db_mempool_each(struct node_db *ndb, mempool_entry_cb cb, void *ctx)
{
    if (!ndb->open) return 0;
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ndb->db,
        "SELECT txid,raw_tx,fee,size,time_added,height_added,spends_coinbase"
        " FROM mempool ORDER BY fee DESC",
        -1, &s, NULL);
    if (!s) return 0;
    int count = 0;
    while (AR_STEP_ROW(s)) {
        struct db_mempool_entry e;
        memset(&e, 0, sizeof(e));
        AR_READ_BLOB(s, 0, e.txid, 32);
        e.raw_tx_len = (size_t)AR_COL_BYTES(s, 1);
        const void *rt = sqlite3_column_blob(s, 1);
        if (rt && e.raw_tx_len > 0) {
            e.raw_tx = malloc(e.raw_tx_len);
            if (e.raw_tx)
                memcpy(e.raw_tx, rt, e.raw_tx_len);
        }
        e.fee = AR_COL_INT(s, 2);
        e.size = (int)AR_COL_INT(s, 3);
        e.time_added = AR_COL_INT(s, 4);
        e.height_added = (int)AR_COL_INT(s, 5);
        e.spends_coinbase = AR_COL_INT(s, 6) != 0;
        cb(&e, ctx);
        free(e.raw_tx);
        count++;
    }
    AR_FINALIZE(s);
    return count;
}
