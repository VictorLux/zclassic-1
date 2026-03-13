/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "models/wallet_tx.h"
#include "models/block.h"
#include "models/wallet_key.h"
#include <string.h>
#include <stdlib.h>

static const uint8_t ZERO_HASH[32] = {0};

/* ── Callbacks ─────────────────────────────────────────────────── */

static struct ar_callbacks wtx_cbs, wutxo_cbs, snote_cbs;
static bool wtx_cbs_init, wutxo_cbs_init, snote_cbs_init;

struct ar_callbacks *db_wallet_tx_callbacks(void)
{
    if (!wtx_cbs_init) { ar_callbacks_init(&wtx_cbs); wtx_cbs_init = true; }
    return &wtx_cbs;
}

struct ar_callbacks *db_wallet_utxo_callbacks(void)
{
    if (!wutxo_cbs_init) { ar_callbacks_init(&wutxo_cbs); wutxo_cbs_init = true; }
    return &wutxo_cbs;
}

struct ar_callbacks *db_sapling_note_callbacks(void)
{
    if (!snote_cbs_init) { ar_callbacks_init(&snote_cbs); snote_cbs_init = true; }
    return &snote_cbs;
}

/* ── Validation ────────────────────────────────────────────────── */

bool db_wallet_tx_validate(const struct db_wallet_tx *t, struct ar_errors *errors)
{
    ar_errors_clear(errors);
    validates_presence_of(errors, t, txid);
    if (t->time_received <= 0)
        ar_errors_add(errors, "time_received", "must be positive");
    return !ar_errors_any(errors);
}

static void db_wallet_tx_read_row(sqlite3_stmt *s, int col,
                                  struct db_wallet_tx *out)
{
    memset(out, 0, sizeof(*out));

    const void *t = sqlite3_column_blob(s, col++);
    if (t && sqlite3_column_bytes(s, col - 1) >= 32)
        memcpy(out->txid, t, 32);

    out->raw_tx_len = (size_t)sqlite3_column_bytes(s, col);
    const void *rt = sqlite3_column_blob(s, col++);
    if (rt && out->raw_tx_len > 0) {
        out->raw_tx = malloc(out->raw_tx_len);
        if (out->raw_tx)
            memcpy(out->raw_tx, rt, out->raw_tx_len);
    }

    const void *bh = sqlite3_column_blob(s, col);
    if (bh && sqlite3_column_bytes(s, col) >= 32) {
        memcpy(out->block_hash, bh, 32);
        out->has_block = memcmp(out->block_hash, ZERO_HASH, 32) != 0;
    }
    col++;

    if (sqlite3_column_type(s, col) != SQLITE_NULL) {
        out->block_height = sqlite3_column_int(s, col);
        out->has_block = true;
    }
    col++;

    out->time_received = sqlite3_column_int64(s, col++);
    out->from_me = sqlite3_column_int(s, col++) != 0;
    out->fee = sqlite3_column_int64(s, col++);
}

bool db_wallet_utxo_validate(const struct db_wallet_utxo *u,
                              struct ar_errors *errors)
{
    ar_errors_clear(errors);
    validates_presence_of(errors, u, txid);
    validates_non_negative(errors, u, value);
    validates_non_negative(errors, u, height);
    return !ar_errors_any(errors);
}

bool db_sapling_note_validate(const struct db_sapling_note *n,
                               struct ar_errors *errors)
{
    ar_errors_clear(errors);
    validates_presence_of(errors, n, txid);
    validates_non_negative(errors, n, value);
    validates_presence_of(errors, n, ivk);
    validates_presence_of(errors, n, nullifier);
    return !ar_errors_any(errors);
}

bool db_wallet_tx_save(struct node_db *ndb, const struct db_wallet_tx *t)
{
    if (!ndb->open) return false;
    struct ar_errors errors;
    if (!db_wallet_tx_validate(t, &errors)) return false;
    if (!ar_run_before_save(&wtx_cbs, (void *)t)) return false;
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ndb->db,
        "INSERT OR REPLACE INTO wallet_transactions"
        "(txid,raw_tx,block_hash,block_height,time_received,from_me,fee)"
        " VALUES(?,?,?,?,?,?,?)",
        -1, &s, NULL);
    sqlite3_bind_blob(s, 1, t->txid, 32, SQLITE_STATIC);
    sqlite3_bind_blob(s, 2, t->raw_tx, (int)t->raw_tx_len, SQLITE_STATIC);
    if (t->has_block)
        sqlite3_bind_blob(s, 3, t->block_hash, 32, SQLITE_STATIC);
    else
        sqlite3_bind_null(s, 3);
    if (t->has_block)
        sqlite3_bind_int(s, 4, t->block_height);
    else
        sqlite3_bind_null(s, 4);
    sqlite3_bind_int64(s, 5, t->time_received);
    sqlite3_bind_int(s, 6, t->from_me ? 1 : 0);
    sqlite3_bind_int64(s, 7, t->fee);
    int rc = sqlite3_step(s);
    sqlite3_finalize(s);
    bool ok = rc == SQLITE_DONE;
    if (ok) ar_run_after_save(&wtx_cbs, (void *)t);
    return ok;
}

bool db_wallet_tx_find(struct node_db *ndb, const uint8_t txid[32],
                       struct db_wallet_tx *out)
{
    if (!ndb->open) return false;
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ndb->db,
        "SELECT txid,raw_tx,block_hash,block_height,time_received,from_me,fee"
        " FROM wallet_transactions WHERE txid=?",
        -1, &s, NULL);
    sqlite3_bind_blob(s, 1, txid, 32, SQLITE_STATIC);
    if (sqlite3_step(s) != SQLITE_ROW) {
        sqlite3_finalize(s);
        return false;
    }
    db_wallet_tx_read_row(s, 0, out);
    sqlite3_finalize(s);
    return true;
}

bool db_wallet_tx_delete(struct node_db *ndb, const uint8_t txid[32])
{
    if (!ndb->open) return false;

    struct db_wallet_tx t;
    memset(&t, 0, sizeof(t));
    memcpy(t.txid, txid, 32);
    if (!ar_run_before_destroy(&wtx_cbs, &t)) return false;

    /* dependent: :destroy — delete child wallet_utxos */
    sqlite3_stmt *du = NULL;
    sqlite3_prepare_v2(ndb->db,
        "DELETE FROM wallet_utxos WHERE txid=?", -1, &du, NULL);
    sqlite3_bind_blob(du, 1, txid, 32, SQLITE_STATIC);
    sqlite3_step(du);
    sqlite3_finalize(du);

    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ndb->db,
        "DELETE FROM wallet_transactions WHERE txid=?", -1, &s, NULL);
    sqlite3_bind_blob(s, 1, txid, 32, SQLITE_STATIC);
    int rc = sqlite3_step(s);
    sqlite3_finalize(s);

    bool ok = rc == SQLITE_DONE;
    if (ok) ar_run_after_destroy(&wtx_cbs, &t);
    return ok;
}

int db_wallet_tx_count(struct node_db *ndb)
{
    if (!ndb->open) return 0;
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ndb->db,
        "SELECT COUNT(*) FROM wallet_transactions", -1, &s, NULL);
    int c = 0;
    if (sqlite3_step(s) == SQLITE_ROW)
        c = sqlite3_column_int(s, 0);
    sqlite3_finalize(s);
    return c;
}

void db_wallet_tx_free(struct db_wallet_tx *t)
{
    if (!t) return;
    free(t->raw_tx);
    t->raw_tx = NULL;
    t->raw_tx_len = 0;
}

int db_wallet_tx_recent(struct node_db *ndb, struct db_wallet_tx *out,
                        size_t max)
{
    return db_wallet_tx_list(ndb, out, max, 0);
}

int db_wallet_tx_list(struct node_db *ndb, struct db_wallet_tx *out,
                      size_t max, size_t offset)
{
    if (!ndb->open || !out || max == 0) return 0;
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ndb->db,
        "SELECT txid,raw_tx,block_hash,block_height,time_received,from_me,fee"
        " FROM wallet_transactions"
        " ORDER BY time_received DESC, txid DESC"
        " LIMIT ? OFFSET ?",
        -1, &s, NULL);
    sqlite3_bind_int(s, 1, (int)max);
    sqlite3_bind_int64(s, 2, (sqlite3_int64)offset);
    int count = 0;
    while (sqlite3_step(s) == SQLITE_ROW && (size_t)count < max) {
        db_wallet_tx_read_row(s, 0, &out[count]);
        count++;
    }
    sqlite3_finalize(s);
    return count;
}

int db_wallet_tx_at_height(struct node_db *ndb, int height,
                           struct db_wallet_tx *out, size_t max)
{
    if (!ndb->open) return 0;
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ndb->db,
        "SELECT txid,raw_tx,block_hash,time_received,from_me,fee"
        " FROM wallet_transactions WHERE block_height=?",
        -1, &s, NULL);
    sqlite3_bind_int(s, 1, height);
    int count = 0;
    while (sqlite3_step(s) == SQLITE_ROW && (size_t)count < max) {
        memset(&out[count], 0, sizeof(out[count]));
        const void *t = sqlite3_column_blob(s, 0);
        if (t) memcpy(out[count].txid, t, 32);
        out[count].raw_tx_len = (size_t)sqlite3_column_bytes(s, 1);
        const void *rt = sqlite3_column_blob(s, 1);
        if (rt && out[count].raw_tx_len > 0) {
            out[count].raw_tx = malloc(out[count].raw_tx_len);
            if (out[count].raw_tx)
                memcpy(out[count].raw_tx, rt, out[count].raw_tx_len);
        }
        out[count].has_block = true;
        out[count].block_height = height;
        const void *bh = sqlite3_column_blob(s, 2);
        if (bh && sqlite3_column_bytes(s, 2) >= 32)
            memcpy(out[count].block_hash, bh, 32);
        out[count].time_received = sqlite3_column_int64(s, 3);
        out[count].from_me = sqlite3_column_int(s, 4) != 0;
        out[count].fee = sqlite3_column_int64(s, 5);
        count++;
    }
    sqlite3_finalize(s);
    return count;
}

/* Wallet UTXOs */

bool db_wallet_utxo_save(struct node_db *ndb, const struct db_wallet_utxo *u)
{
    if (!ndb->open) return false;
    struct ar_errors errors;
    if (!db_wallet_utxo_validate(u, &errors)) return false;
    if (!ar_run_before_save(&wutxo_cbs, (void *)u)) return false;
    sqlite3_stmt *s = ndb->stmt_wallet_utxo_insert;
    sqlite3_reset(s);
    sqlite3_bind_blob(s, 1, u->txid, 32, SQLITE_STATIC);
    sqlite3_bind_int(s, 2, (int)u->vout);
    sqlite3_bind_int64(s, 3, u->value);
    sqlite3_bind_blob(s, 4, u->address_hash, 20, SQLITE_STATIC);
    sqlite3_bind_blob(s, 5, u->script, (int)u->script_len, SQLITE_STATIC);
    sqlite3_bind_int(s, 6, u->height);
    sqlite3_bind_int(s, 7, u->is_coinbase ? 1 : 0);
    bool ok = sqlite3_step(s) == SQLITE_DONE;
    if (ok) ar_run_after_save(&wutxo_cbs, (void *)u);
    return ok;
}

bool db_wallet_utxo_mark_spent(struct node_db *ndb,
                               const uint8_t txid[32], uint32_t vout,
                               const uint8_t spent_by[32], int vin)
{
    if (!ndb->open) return false;
    sqlite3_stmt *s = ndb->stmt_wallet_utxo_spend;
    sqlite3_reset(s);
    sqlite3_bind_blob(s, 1, spent_by, 32, SQLITE_STATIC);
    sqlite3_bind_int(s, 2, vin);
    sqlite3_bind_blob(s, 3, txid, 32, SQLITE_STATIC);
    sqlite3_bind_int(s, 4, (int)vout);
    if (sqlite3_step(s) != SQLITE_DONE) return false;
    return sqlite3_changes(ndb->db) > 0;
}

bool db_wallet_utxo_find(struct node_db *ndb,
                         const uint8_t txid[32], uint32_t vout,
                         struct db_wallet_utxo *out)
{
    if (!ndb->open) return false;
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ndb->db,
        "SELECT value,address_hash,script,height,spent_txid,spent_vin,is_coinbase"
        " FROM wallet_utxos WHERE txid=? AND vout=?",
        -1, &s, NULL);
    sqlite3_bind_blob(s, 1, txid, 32, SQLITE_STATIC);
    sqlite3_bind_int(s, 2, (int)vout);
    if (sqlite3_step(s) != SQLITE_ROW) {
        sqlite3_finalize(s);
        return false;
    }
    memset(out, 0, sizeof(*out));
    memcpy(out->txid, txid, 32);
    out->vout = vout;
    out->value = sqlite3_column_int64(s, 0);
    const void *ah = sqlite3_column_blob(s, 1);
    if (ah) memcpy(out->address_hash, ah, 20);
    out->script_len = (size_t)sqlite3_column_bytes(s, 2);
    out->script = NULL;
    out->height = sqlite3_column_int(s, 3);
    const void *st = sqlite3_column_blob(s, 4);
    if (st && sqlite3_column_bytes(s, 4) >= 32) {
        memcpy(out->spent_txid, st, 32);
        out->is_spent = true;
    }
    if (sqlite3_column_type(s, 5) != SQLITE_NULL)
        out->spent_vin = sqlite3_column_int(s, 5);
    out->is_coinbase = sqlite3_column_int(s, 6) != 0;
    sqlite3_finalize(s);
    return true;
}

int64_t db_wallet_utxo_balance(struct node_db *ndb)
{
    if (!ndb->open) return 0;
    sqlite3_stmt *s = ndb->stmt_wallet_balance;
    sqlite3_reset(s);
    int64_t bal = 0;
    if (sqlite3_step(s) == SQLITE_ROW)
        bal = sqlite3_column_int64(s, 0);
    return bal;
}

int db_wallet_utxo_list_unspent(struct node_db *ndb,
                                struct db_wallet_utxo *out, size_t max)
{
    if (!ndb->open) return 0;
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ndb->db,
        "SELECT txid,vout,value,address_hash,script,height,is_coinbase"
        " FROM wallet_utxos WHERE spent_txid IS NULL"
        " ORDER BY value DESC",
        -1, &s, NULL);
    int count = 0;
    while (sqlite3_step(s) == SQLITE_ROW && (size_t)count < max) {
        memset(&out[count], 0, sizeof(out[count]));
        const void *t = sqlite3_column_blob(s, 0);
        if (t) memcpy(out[count].txid, t, 32);
        out[count].vout = (uint32_t)sqlite3_column_int(s, 1);
        out[count].value = sqlite3_column_int64(s, 2);
        const void *ah = sqlite3_column_blob(s, 3);
        if (ah) memcpy(out[count].address_hash, ah, 20);
        out[count].script_len = (size_t)sqlite3_column_bytes(s, 4);
        out[count].script = NULL;
        out[count].height = sqlite3_column_int(s, 5);
        out[count].is_coinbase = sqlite3_column_int(s, 6) != 0;
        count++;
    }
    sqlite3_finalize(s);
    return count;
}

int db_wallet_utxo_select_coins(struct node_db *ndb, int64_t target,
                                int current_height,
                                struct db_wallet_utxo *out, size_t max)
{
    if (!ndb->open) return 0;
    /* Select unspent, excluding immature coinbase (< 100 confirmations) */
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ndb->db,
        "SELECT txid,vout,value,address_hash,script,height,is_coinbase"
        " FROM wallet_utxos"
        " WHERE spent_txid IS NULL"
        "   AND (is_coinbase=0 OR height <= ?)"
        " ORDER BY value DESC",
        -1, &s, NULL);
    sqlite3_bind_int(s, 1, current_height - 100);
    int count = 0;
    int64_t total = 0;
    while (sqlite3_step(s) == SQLITE_ROW && (size_t)count < max) {
        memset(&out[count], 0, sizeof(out[count]));
        const void *t = sqlite3_column_blob(s, 0);
        if (t) memcpy(out[count].txid, t, 32);
        out[count].vout = (uint32_t)sqlite3_column_int(s, 1);
        out[count].value = sqlite3_column_int64(s, 2);
        const void *ah = sqlite3_column_blob(s, 3);
        if (ah) memcpy(out[count].address_hash, ah, 20);
        out[count].script_len = (size_t)sqlite3_column_bytes(s, 4);
        out[count].script = NULL;
        out[count].height = sqlite3_column_int(s, 5);
        out[count].is_coinbase = sqlite3_column_int(s, 6) != 0;
        total += out[count].value;
        count++;
        if (total >= target) break;
    }
    sqlite3_finalize(s);
    return count;
}

/* Sapling notes */

bool db_sapling_note_save(struct node_db *ndb, const struct db_sapling_note *n)
{
    if (!ndb->open) return false;
    struct ar_errors errors;
    if (!db_sapling_note_validate(n, &errors)) return false;
    if (!ar_run_before_save(&snote_cbs, (void *)n)) return false;
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ndb->db,
        "INSERT OR REPLACE INTO wallet_sapling_notes"
        "(txid,output_index,value,rcm,memo,ivk,diversifier,pk_d,cm,"
        "nullifier,block_height,spent_txid)"
        " VALUES(?,?,?,?,?,?,?,?,?,?,?,?)",
        -1, &s, NULL);
    sqlite3_bind_blob(s, 1, n->txid, 32, SQLITE_STATIC);
    sqlite3_bind_int(s, 2, (int)n->output_index);
    sqlite3_bind_int64(s, 3, n->value);
    sqlite3_bind_blob(s, 4, n->rcm, 32, SQLITE_STATIC);
    if (n->memo_len > 0)
        sqlite3_bind_blob(s, 5, n->memo, (int)n->memo_len, SQLITE_STATIC);
    else
        sqlite3_bind_null(s, 5);
    sqlite3_bind_blob(s, 6, n->ivk, 32, SQLITE_STATIC);
    sqlite3_bind_blob(s, 7, n->diversifier, 11, SQLITE_STATIC);
    sqlite3_bind_blob(s, 8, n->pk_d, 32, SQLITE_STATIC);
    sqlite3_bind_blob(s, 9, n->cm, 32, SQLITE_STATIC);
    sqlite3_bind_blob(s, 10, n->nullifier, 32, SQLITE_STATIC);
    if (n->block_height > 0)
        sqlite3_bind_int(s, 11, n->block_height);
    else
        sqlite3_bind_null(s, 11);
    if (n->is_spent)
        sqlite3_bind_blob(s, 12, n->spent_txid, 32, SQLITE_STATIC);
    else
        sqlite3_bind_null(s, 12);
    int rc = sqlite3_step(s);
    sqlite3_finalize(s);
    bool ok2 = rc == SQLITE_DONE;
    if (ok2) ar_run_after_save(&snote_cbs, (void *)n);
    return ok2;
}

bool db_sapling_note_mark_spent(struct node_db *ndb,
                                const uint8_t nullifier[32],
                                const uint8_t spent_by[32])
{
    if (!ndb->open) return false;
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ndb->db,
        "UPDATE wallet_sapling_notes SET spent_txid=?"
        " WHERE nullifier=?",
        -1, &s, NULL);
    sqlite3_bind_blob(s, 1, spent_by, 32, SQLITE_STATIC);
    sqlite3_bind_blob(s, 2, nullifier, 32, SQLITE_STATIC);
    int rc = sqlite3_step(s);
    sqlite3_finalize(s);
    return rc == SQLITE_DONE;
}

bool db_sapling_note_is_nullifier_spent(struct node_db *ndb,
                                        const uint8_t nullifier[32])
{
    if (!ndb->open) return false;
    /* Check global nullifier set */
    sqlite3_stmt *s = ndb->stmt_nullifier_exists;
    sqlite3_reset(s);
    sqlite3_bind_blob(s, 1, nullifier, 32, SQLITE_STATIC);
    return sqlite3_step(s) == SQLITE_ROW;
}

int64_t db_sapling_note_balance(struct node_db *ndb)
{
    if (!ndb->open) return 0;
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ndb->db,
        "SELECT COALESCE(SUM(value),0) FROM wallet_sapling_notes"
        " WHERE spent_txid IS NULL",
        -1, &s, NULL);
    int64_t bal = 0;
    if (sqlite3_step(s) == SQLITE_ROW)
        bal = sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);
    return bal;
}

int64_t db_sapling_note_balance_for_ivk(struct node_db *ndb,
                                        const uint8_t ivk[32])
{
    if (!ndb->open) return 0;
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ndb->db,
        "SELECT COALESCE(SUM(value),0) FROM wallet_sapling_notes"
        " WHERE ivk=? AND spent_txid IS NULL",
        -1, &s, NULL);
    sqlite3_bind_blob(s, 1, ivk, 32, SQLITE_STATIC);
    int64_t bal = 0;
    if (sqlite3_step(s) == SQLITE_ROW)
        bal = sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);
    return bal;
}

int db_sapling_note_list_unspent(struct node_db *ndb,
                                 struct db_sapling_note *out, size_t max)
{
    if (!ndb->open) return 0;
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ndb->db,
        "SELECT txid,output_index,value,rcm,memo,ivk,diversifier,pk_d,"
        "cm,nullifier,block_height"
        " FROM wallet_sapling_notes WHERE spent_txid IS NULL"
        " ORDER BY value DESC",
        -1, &s, NULL);
    int count = 0;
    while (sqlite3_step(s) == SQLITE_ROW && (size_t)count < max) {
        memset(&out[count], 0, sizeof(out[count]));
        const void *t = sqlite3_column_blob(s, 0);
        if (t) memcpy(out[count].txid, t, 32);
        out[count].output_index = (uint32_t)sqlite3_column_int(s, 1);
        out[count].value = sqlite3_column_int64(s, 2);
        const void *rcm = sqlite3_column_blob(s, 3);
        if (rcm) memcpy(out[count].rcm, rcm, 32);
        int memo_len = sqlite3_column_bytes(s, 4);
        const void *memo = sqlite3_column_blob(s, 4);
        if (memo && memo_len > 0) {
            size_t ml = (size_t)memo_len < 512 ? (size_t)memo_len : 512;
            memcpy(out[count].memo, memo, ml);
            out[count].memo_len = ml;
        }
        const void *ivk = sqlite3_column_blob(s, 5);
        if (ivk) memcpy(out[count].ivk, ivk, 32);
        const void *div = sqlite3_column_blob(s, 6);
        if (div) memcpy(out[count].diversifier, div, 11);
        const void *pkd = sqlite3_column_blob(s, 7);
        if (pkd) memcpy(out[count].pk_d, pkd, 32);
        const void *cm = sqlite3_column_blob(s, 8);
        if (cm) memcpy(out[count].cm, cm, 32);
        const void *nf = sqlite3_column_blob(s, 9);
        if (nf) memcpy(out[count].nullifier, nf, 32);
        if (sqlite3_column_type(s, 10) != SQLITE_NULL)
            out[count].block_height = sqlite3_column_int(s, 10);
        count++;
    }
    sqlite3_finalize(s);
    return count;
}

/* ── Relationships ─────────────────────────────────────────────── */

/* WalletTx has_many :wallet_utxos */
int db_wallet_tx_utxos(struct node_db *ndb, const uint8_t txid[32],
                        struct db_wallet_utxo *out, size_t max)
{
    if (!ndb->open) return 0;
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ndb->db,
        "SELECT vout,value,address_hash,script,height,is_coinbase,"
        "spent_txid,spent_vin"
        " FROM wallet_utxos WHERE txid=? ORDER BY vout",
        -1, &s, NULL);
    sqlite3_bind_blob(s, 1, txid, 32, SQLITE_STATIC);
    int count = 0;
    while (sqlite3_step(s) == SQLITE_ROW && (size_t)count < max) {
        memset(&out[count], 0, sizeof(out[count]));
        memcpy(out[count].txid, txid, 32);
        out[count].vout = (uint32_t)sqlite3_column_int(s, 0);
        out[count].value = sqlite3_column_int64(s, 1);
        const void *ah = sqlite3_column_blob(s, 2);
        if (ah) memcpy(out[count].address_hash, ah, 20);
        out[count].script_len = (size_t)sqlite3_column_bytes(s, 3);
        out[count].script = NULL;
        out[count].height = sqlite3_column_int(s, 4);
        out[count].is_coinbase = sqlite3_column_int(s, 5) != 0;
        const void *st = sqlite3_column_blob(s, 6);
        if (st && sqlite3_column_bytes(s, 6) >= 32) {
            memcpy(out[count].spent_txid, st, 32);
            out[count].is_spent = true;
        }
        if (sqlite3_column_type(s, 7) != SQLITE_NULL)
            out[count].spent_vin = sqlite3_column_int(s, 7);
        count++;
    }
    sqlite3_finalize(s);
    return count;
}

/* WalletTx has_many :sapling_notes */
int db_wallet_tx_notes(struct node_db *ndb, const uint8_t txid[32],
                        struct db_sapling_note *out, size_t max)
{
    if (!ndb->open) return 0;
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ndb->db,
        "SELECT output_index,value,rcm,memo,ivk,diversifier,pk_d,"
        "cm,nullifier,block_height,spent_txid"
        " FROM wallet_sapling_notes WHERE txid=? ORDER BY output_index",
        -1, &s, NULL);
    sqlite3_bind_blob(s, 1, txid, 32, SQLITE_STATIC);
    int count = 0;
    while (sqlite3_step(s) == SQLITE_ROW && (size_t)count < max) {
        memset(&out[count], 0, sizeof(out[count]));
        memcpy(out[count].txid, txid, 32);
        out[count].output_index = (uint32_t)sqlite3_column_int(s, 0);
        out[count].value = sqlite3_column_int64(s, 1);
        const void *rcm = sqlite3_column_blob(s, 2);
        if (rcm) memcpy(out[count].rcm, rcm, 32);
        int memo_len = sqlite3_column_bytes(s, 3);
        const void *memo = sqlite3_column_blob(s, 3);
        if (memo && memo_len > 0) {
            size_t ml = (size_t)memo_len < 512 ? (size_t)memo_len : 512;
            memcpy(out[count].memo, memo, ml);
            out[count].memo_len = ml;
        }
        const void *ivk = sqlite3_column_blob(s, 4);
        if (ivk) memcpy(out[count].ivk, ivk, 32);
        const void *div = sqlite3_column_blob(s, 5);
        if (div) memcpy(out[count].diversifier, div, 11);
        const void *pkd = sqlite3_column_blob(s, 6);
        if (pkd) memcpy(out[count].pk_d, pkd, 32);
        const void *cm = sqlite3_column_blob(s, 7);
        if (cm) memcpy(out[count].cm, cm, 32);
        const void *nf = sqlite3_column_blob(s, 8);
        if (nf) memcpy(out[count].nullifier, nf, 32);
        if (sqlite3_column_type(s, 9) != SQLITE_NULL)
            out[count].block_height = sqlite3_column_int(s, 9);
        const void *st = sqlite3_column_blob(s, 10);
        if (st && sqlite3_column_bytes(s, 10) >= 32) {
            memcpy(out[count].spent_txid, st, 32);
            out[count].is_spent = true;
        }
        count++;
    }
    sqlite3_finalize(s);
    return count;
}

/* WalletTx belongs_to :block */
bool db_wallet_tx_block(struct node_db *ndb, const struct db_wallet_tx *t,
                        struct db_block *out)
{
    if (!t->has_block) return false;
    return db_block_find_by_hash(ndb, t->block_hash, out);
}

/* WalletUTXO belongs_to :wallet_key */
bool db_wallet_utxo_key(struct node_db *ndb, const struct db_wallet_utxo *u,
                        struct db_wallet_key *out)
{
    return db_wallet_key_find(ndb, u->address_hash, out);
}

/* SaplingNote belongs_to :sapling_key */
bool db_sapling_note_key(struct node_db *ndb, const struct db_sapling_note *n,
                         struct db_sapling_key *out)
{
    return db_sapling_key_find_by_ivk(ndb, n->ivk, out);
}
