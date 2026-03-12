/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "db/model_tx.h"
#include "db/model_block.h"
#include <string.h>

/* ── Callbacks ─────────────────────────────────────────────────── */

static struct ar_callbacks tx_cbs;
static bool tx_cbs_init = false;

struct ar_callbacks *db_tx_callbacks(void)
{
    if (!tx_cbs_init) {
        ar_callbacks_init(&tx_cbs);
        tx_cbs_init = true;
    }
    return &tx_cbs;
}

/* ── Validation ────────────────────────────────────────────────── */

bool db_tx_validate(const struct db_tx_index *t, struct ar_errors *errors)
{
    ar_errors_clear(errors);
    validates_presence_of(errors, t, txid);
    validates_presence_of(errors, t, block_hash);
    validates_non_negative(errors, t, block_height);
    validates_non_negative(errors, t, tx_index);
    return !ar_errors_any(errors);
}

/* ── CRUD ──────────────────────────────────────────────────────── */

bool db_tx_save(struct node_db *ndb, const struct db_tx_index *t)
{
    if (!ndb->open) return false;

    struct ar_errors errors;
    if (!db_tx_validate(t, &errors)) return false;

    struct ar_callbacks *cbs = db_tx_callbacks();
    if (!ar_run_before_save(cbs, (void *)t)) return false;

    sqlite3_stmt *s = ndb->stmt_tx_insert;
    sqlite3_reset(s);
    sqlite3_bind_blob(s, 1, t->txid, 32, SQLITE_STATIC);
    sqlite3_bind_blob(s, 2, t->block_hash, 32, SQLITE_STATIC);
    sqlite3_bind_int(s, 3, t->block_height);
    sqlite3_bind_int(s, 4, t->tx_index);
    sqlite3_bind_int(s, 5, t->file_num);
    sqlite3_bind_int(s, 6, t->file_pos);
    sqlite3_bind_int(s, 7, t->is_coinbase ? 1 : 0);

    bool ok = sqlite3_step(s) == SQLITE_DONE;
    if (ok) ar_run_after_save(cbs, (void *)t);
    return ok;
}

bool db_tx_find(struct node_db *ndb, const uint8_t txid[32],
                struct db_tx_index *out)
{
    if (!ndb->open) return false;
    sqlite3_stmt *s = ndb->stmt_tx_find;
    sqlite3_reset(s);
    sqlite3_bind_blob(s, 1, txid, 32, SQLITE_STATIC);
    if (sqlite3_step(s) != SQLITE_ROW) return false;
    memset(out, 0, sizeof(*out));
    memcpy(out->txid, txid, 32);
    const void *bh = sqlite3_column_blob(s, 0);
    if (bh) memcpy(out->block_hash, bh, 32);
    out->block_height = sqlite3_column_int(s, 1);
    out->tx_index = sqlite3_column_int(s, 2);
    out->file_num = sqlite3_column_int(s, 3);
    out->file_pos = sqlite3_column_int(s, 4);
    out->is_coinbase = sqlite3_column_int(s, 5) != 0;
    return true;
}

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
    sqlite3_bind_blob(s, 1, txid, 32, SQLITE_STATIC);
    int rc = sqlite3_step(s);
    sqlite3_finalize(s);

    bool ok = rc == SQLITE_DONE;
    if (ok) ar_run_after_destroy(cbs, &t);
    return ok;
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
    sqlite3_bind_blob(s, 1, block_hash, 32, SQLITE_STATIC);
    int count = 0;
    while (sqlite3_step(s) == SQLITE_ROW && (size_t)count < max) {
        memset(&out[count], 0, sizeof(out[count]));
        const void *t = sqlite3_column_blob(s, 0);
        if (t) memcpy(out[count].txid, t, 32);
        memcpy(out[count].block_hash, block_hash, 32);
        out[count].block_height = sqlite3_column_int(s, 1);
        out[count].tx_index = sqlite3_column_int(s, 2);
        out[count].file_num = sqlite3_column_int(s, 3);
        out[count].file_pos = sqlite3_column_int(s, 4);
        out[count].is_coinbase = sqlite3_column_int(s, 5) != 0;
        count++;
    }
    sqlite3_finalize(s);
    return count;
}

/* ── Relationships ─────────────────────────────────────────────── */

/* belongs_to :block */
bool db_tx_block(struct node_db *ndb, const struct db_tx_index *t,
                 struct db_block *out)
{
    return db_block_find_by_hash(ndb, t->block_hash, out);
}
