/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * ActiveRecord model: Transaction (tx_index)
 *
 * validates :txid, :block_hash, presence: true
 * validates :block_height, :tx_index, :file_num, :file_pos, numericality: { >= 0 }
 *
 * belongs_to :block */

#include "models/tx_index.h"
#include "models/block.h"
#include <string.h>

/* ── Callbacks ─────────────────────────────────────────────────── */

DEFINE_MODEL_CALLBACKS(tx)

/* ── Validation ────────────────────────────────────────────────── */

bool db_tx_validate(const struct db_tx_index *t, struct ar_errors *errors)
{
    ar_errors_clear(errors);
    validates_presence_of(errors, t, txid);
    validates_presence_of(errors, t, block_hash);
    validates_non_negative(errors, t, block_height);
    validates_non_negative(errors, t, tx_index);
    validates_non_negative(errors, t, file_num);
    validates_non_negative(errors, t, file_pos);
    return !ar_errors_any(errors);
}

/* ── Save ──────────────────────────────────────────────────────── */

bool db_tx_save(struct node_db *ndb, const struct db_tx_index *t)
{
    if (!ndb->open) return false;

    struct ar_callbacks *cbs = db_tx_callbacks();
    AR_VALIDATE_RECORD(cbs, "tx_index", t, db_tx_validate);
    if (!ar_run_before_save(cbs, (void *)t)) return false;

    sqlite3_stmt *s = ndb->stmt_tx_insert;
    sqlite3_reset(s);
    AR_BIND_BLOB(s, 1, t->txid, 32);
    AR_BIND_BLOB(s, 2, t->block_hash, 32);
    AR_BIND_INT(s, 3, t->block_height);
    AR_BIND_INT(s, 4, t->tx_index);
    AR_BIND_INT(s, 5, t->file_num);
    AR_BIND_INT(s, 6, t->file_pos);
    AR_BIND_INT(s, 7, t->is_coinbase ? 1 : 0);

    bool ok = AR_STEP_DONE(s);
    if (ok) ar_run_after_save(cbs, (void *)t);
    return ok;
}

/* ── Find ──────────────────────────────────────────────────────── */

bool db_tx_find(struct node_db *ndb, const uint8_t txid[32],
                struct db_tx_index *out)
{
    if (!ndb->open) return false;
    sqlite3_stmt *s = ndb->stmt_tx_find;
    sqlite3_reset(s);
    AR_BIND_BLOB(s, 1, txid, 32);
    if (!AR_STEP_ROW(s)) return false;
    memset(out, 0, sizeof(*out));
    memcpy(out->txid, txid, 32);
    AR_READ_BLOB(s, 0, out->block_hash, 32);
    out->block_height = (int)AR_COL_INT(s, 1);
    out->tx_index = (int)AR_COL_INT(s, 2);
    out->file_num = (int)AR_COL_INT(s, 3);
    out->file_pos = (int)AR_COL_INT(s, 4);
    out->is_coinbase = AR_COL_INT(s, 5) != 0;
    return true;
}

/* ── Delete ────────────────────────────────────────────────────── */

bool db_tx_delete(struct node_db *ndb, const uint8_t txid[32])
{
    if (!ndb->open) return false;

    struct ar_callbacks *cbs = db_tx_callbacks();
    struct db_tx_index t;
    memcpy(t.txid, txid, 32);
    if (!ar_run_before_destroy(cbs, &t)) return false;

    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ndb->db, "DELETE FROM transactions WHERE txid=?",
                       -1, &s, NULL);
    if (!s) return false;
    AR_BIND_BLOB(s, 1, txid, 32);
    bool ok = AR_STEP_DONE(s);
    AR_FINALIZE(s);
    if (ok) ar_run_after_destroy(cbs, &t);
    return ok;
}

/* ── Queries ───────────────────────────────────────────────────── */

int db_tx_count(struct node_db *ndb)
{
    if (!ndb->open) return 0;
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ndb->db, "SELECT COUNT(*) FROM transactions",
                       -1, &s, NULL);
    int count = 0;
    if (AR_STEP_ROW(s))
        count = (int)AR_COL_INT(s, 0);
    AR_FINALIZE(s);
    return count;
}

bool db_tx_save_batch(struct node_db *ndb, const struct db_tx_index *txs,
                      size_t count)
{
    for (size_t i = 0; i < count; i++) {
        if (!db_tx_save(ndb, &txs[i]))
            return false;
    }
    return true;
}

int db_tx_find_by_block(struct node_db *ndb, const uint8_t block_hash[32],
                        struct db_tx_index *out, size_t max)
{
    if (!ndb->open) return 0;
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ndb->db,
        "SELECT txid,block_height,tx_index,file_num,file_pos,is_coinbase"
        " FROM transactions WHERE block_hash=? ORDER BY tx_index",
        -1, &s, NULL);
    AR_BIND_BLOB(s, 1, block_hash, 32);
    int count = 0;
    while (AR_STEP_ROW(s) && (size_t)count < max) {
        memset(&out[count], 0, sizeof(out[count]));
        AR_READ_BLOB(s, 0, out[count].txid, 32);
        memcpy(out[count].block_hash, block_hash, 32);
        out[count].block_height = (int)AR_COL_INT(s, 1);
        out[count].tx_index = (int)AR_COL_INT(s, 2);
        out[count].file_num = (int)AR_COL_INT(s, 3);
        out[count].file_pos = (int)AR_COL_INT(s, 4);
        out[count].is_coinbase = AR_COL_INT(s, 5) != 0;
        count++;
    }
    AR_FINALIZE(s);
    return count;
}

/* ── Relationships ─────────────────────────────────────────────── */

/* belongs_to :block */
bool db_tx_block(struct node_db *ndb, const struct db_tx_index *t,
                 struct db_block *out)
{
    return db_block_find_by_hash(ndb, t->block_hash, out);
}
