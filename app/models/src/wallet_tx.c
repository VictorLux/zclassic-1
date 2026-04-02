/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * ActiveRecord models: WalletTx, WalletUTXO, SaplingNote
 *
 * WalletTx:
 *   validates :txid, presence: true
 *   validates :time_received, positive: true
 *   has_many :wallet_utxos, :sapling_notes
 *   belongs_to :block
 *
 * WalletUTXO:
 *   validates :txid, presence: true
 *   validates :value, money_range: [0, MAX_MONEY]
 *   belongs_to :wallet_key
 *
 * SaplingNote:
 *   validates :txid, :ivk, :nullifier, :cm, :pk_d, :diversifier, :rcm, presence
 *   validates :value, money_range: [0, MAX_MONEY]
 *   belongs_to :sapling_key */

#include "models/wallet_tx.h"
#include "models/block.h"
#include "models/wallet_key.h"
#include "wallet/sapling_keys.h"
#include "chain/chainparams.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>

static const uint8_t ZERO_HASH[32] = {0};

/* ── Callbacks ─────────────────────────────────────────────────── */

DEFINE_MODEL_CALLBACKS(wallet_tx)
DEFINE_MODEL_CALLBACKS(wallet_utxo)
DEFINE_MODEL_CALLBACKS(sapling_note)

/* ── Validation ────────────────────────────────────────────────── */

bool db_wallet_tx_validate(const struct db_wallet_tx *t, struct ar_errors *errors)
{
    ar_errors_clear(errors);
    validates_presence_of(errors, t, txid);
    validates_positive(errors, t, time_received);
    validates_custom(errors,
        !(t->raw_tx && t->raw_tx_len == 0),
        "raw_tx_len", "must be positive when raw_tx present");
    validates_custom(errors,
        t->raw_tx_len <= (size_t)INT32_MAX,
        "raw_tx_len", "exceeds max size");
    if (t->has_block) {
        validates_non_negative(errors, t, block_height);
        validates_presence_of(errors, t, block_hash);
    }
    validates_non_negative(errors, t, fee);
    return !ar_errors_any(errors);
}

bool db_wallet_utxo_validate(const struct db_wallet_utxo *u,
                              struct ar_errors *errors)
{
    ar_errors_clear(errors);
    validates_presence_of(errors, u, txid);
    validates_money_range(errors, u, value, 2100000000000000LL);
    validates_non_negative(errors, u, height);
    validates_custom(errors,
        !(u->script && u->script_len == 0),
        "script_len", "must be positive when script present");
    validates_max(errors, u, script_len, 10000);
    validates_custom(errors,
        !(u->script_len > 0 && !u->script),
        "script", "null pointer with nonzero length");
    if (u->is_spent) {
        static const uint8_t z[32] = {0};
        if (memcmp(u->spent_txid, z, 32) == 0)
            ar_errors_add(errors, "spent_txid", "can't be blank when spent");
    }
    return !ar_errors_any(errors);
}

bool db_sapling_note_validate(const struct db_sapling_note *n,
                               struct ar_errors *errors)
{
    ar_errors_clear(errors);
    validates_presence_of(errors, n, txid);
    validates_money_range(errors, n, value, 2100000000000000LL);
    validates_presence_of(errors, n, ivk);
    validates_presence_of(errors, n, nullifier);
    validates_presence_of(errors, n, cm);
    validates_presence_of(errors, n, pk_d);
    validates_presence_of(errors, n, diversifier);
    validates_presence_of(errors, n, rcm);
    validates_max(errors, n, memo_len, 512);
    validates_non_negative(errors, n, block_height);
    if (n->is_spent) {
        static const uint8_t z[32] = {0};
        if (memcmp(n->spent_txid, z, 32) == 0)
            ar_errors_add(errors, "spent_txid", "can't be blank when spent");
    }
    return !ar_errors_any(errors);
}

/* ── Row Deserialization ──────────────────────────────────────── */

static void db_wallet_tx_read_row(sqlite3_stmt *s, int col,
                                  struct db_wallet_tx *out)
{
    memset(out, 0, sizeof(*out));
    AR_READ_BLOB(s, col, out->txid, 32);                  col++;

    /* raw_tx: variable-length blob, needs malloc */
    out->raw_tx_len = (size_t)AR_COL_BYTES(s, col);
    const void *rt = sqlite3_column_blob(s, col++);
    if (rt && out->raw_tx_len > 0) {
        out->raw_tx = malloc(out->raw_tx_len);
        if (!out->raw_tx) { out->raw_tx_len = 0; return; }
        memcpy(out->raw_tx, rt, out->raw_tx_len);
    }

    AR_READ_BLOB(s, col, out->block_hash, 32);
    out->has_block = memcmp(out->block_hash, ZERO_HASH, 32) != 0;
    col++;

    if (sqlite3_column_type(s, col) != SQLITE_NULL) {
        out->block_height = (int)AR_COL_INT(s, col);
        out->has_block = true;
    }
    col++;

    out->time_received = AR_COL_INT(s, col++);
    out->from_me = AR_COL_INT(s, col++) != 0;
    out->fee = AR_COL_INT(s, col++);
}

static void db_wallet_utxo_read_row(sqlite3_stmt *s, int col,
                                     struct db_wallet_utxo *out)
{
    memset(out, 0, sizeof(*out));
    AR_READ_BLOB(s, col, out->txid, 32);                   col++;
    out->vout = (uint32_t)AR_COL_INT(s, col++);
    out->value = AR_COL_INT(s, col++);
    AR_READ_BLOB(s, col, out->address_hash, 20);           col++;
    /* script: variable-length, needs malloc */
    out->script_len = (size_t)AR_COL_BYTES(s, col);
    const void *sc = sqlite3_column_blob(s, col);
    if (sc && out->script_len > 0) {
        out->script = malloc(out->script_len);
        if (out->script)
            memcpy(out->script, sc, out->script_len);
    } else {
        out->script = NULL;
    }
    col++;
    out->height = (int)AR_COL_INT(s, col++);
    out->is_coinbase = AR_COL_INT(s, col++) != 0;
}

static void db_sapling_note_read_row(sqlite3_stmt *s, int col,
                                      struct db_sapling_note *out)
{
    memset(out, 0, sizeof(*out));
    AR_READ_BLOB(s, col, out->txid, 32);                   col++;
    out->output_index = (uint32_t)AR_COL_INT(s, col++);
    out->value = AR_COL_INT(s, col++);
    AR_READ_BLOB(s, col, out->rcm, 32);                    col++;
    /* memo: variable-length, capped at 512 */
    {
        int memo_len = AR_COL_BYTES(s, col);
        const void *memo = sqlite3_column_blob(s, col);
        if (memo && memo_len > 0) {
            size_t ml = (size_t)memo_len < 512 ? (size_t)memo_len : 512;
            memcpy(out->memo, memo, ml);
            out->memo_len = ml;
        }
    }
    col++;
    AR_READ_BLOB(s, col, out->ivk, 32);                    col++;
    AR_READ_BLOB(s, col, out->diversifier, 11);             col++;
    AR_READ_BLOB(s, col, out->pk_d, 32);                   col++;
    AR_READ_BLOB(s, col, out->cm, 32);                     col++;
    AR_READ_BLOB(s, col, out->nullifier, 32);              col++;
    if (sqlite3_column_type(s, col) != SQLITE_NULL)
        out->block_height = (int)AR_COL_INT(s, col);

    /* Derive bech32 z-address from diversifier+pk_d for in-memory use. */
    const struct chain_params *cp = chain_params_get();
    if (cp)
        sapling_encode_payment_address(out->diversifier, out->pk_d,
            cp->bech32HRPs[BECH32_SAPLING_PAYMENT_ADDRESS],
            out->address, sizeof(out->address));
}

/* Read spent_txid from a column after the standard note columns */
static void read_spent_txid(sqlite3_stmt *s, int col, struct db_sapling_note *n)
{
    const void *st = sqlite3_column_blob(s, col);
    if (st && sqlite3_column_bytes(s, col) >= 32) {
        memcpy(n->spent_txid, st, 32);
        n->is_spent = true;
    }
}

/* Read spent_txid/spent_vin from columns after standard UTXO columns */
static void read_utxo_spent(sqlite3_stmt *s, int col, struct db_wallet_utxo *u)
{
    const void *st = sqlite3_column_blob(s, col);
    if (st && sqlite3_column_bytes(s, col) >= 32) {
        memcpy(u->spent_txid, st, 32);
        u->is_spent = true;
    }
    if (sqlite3_column_type(s, col + 1) != SQLITE_NULL)
        u->spent_vin = (int)AR_COL_INT(s, col + 1);
}

/* ── WalletTx CRUD ────────────────────────────────────────────── */

bool db_wallet_tx_save(struct node_db *ndb, const struct db_wallet_tx *t)
{
    if (!ndb->open) return false;
    if (t->time_received == 0)
        ((struct db_wallet_tx *)t)->time_received = (int64_t)time(NULL);

    struct ar_errors errors;
    if (!db_wallet_tx_validate(t, &errors)) {
        AR_LOG_VALIDATION_FAILURE("wallet_tx", &errors);
        return false;
    }

    struct ar_callbacks *cbs = db_wallet_tx_callbacks();
    if (!ar_run_before_save(cbs, (void *)t)) return false;

    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ndb->db,
        "INSERT OR REPLACE INTO wallet_transactions"
        "(txid,raw_tx,block_hash,block_height,time_received,from_me,fee)"
        " VALUES(?,?,?,?,?,?,?)",
        -1, &s, NULL);
    if (!s) return false;
    AR_BIND_BLOB(s, 1, t->txid, 32);
    AR_BIND_BLOB(s, 2, t->raw_tx, (int)t->raw_tx_len);
    if (t->has_block)
        AR_BIND_BLOB(s, 3, t->block_hash, 32);
    else
        AR_BIND_NULL(s, 3);
    if (t->has_block)
        AR_BIND_INT(s, 4, t->block_height);
    else
        AR_BIND_NULL(s, 4);
    AR_BIND_INT(s, 5, t->time_received);
    AR_BIND_INT(s, 6, t->from_me ? 1 : 0);
    AR_BIND_INT(s, 7, t->fee);
    bool ok = AR_STEP_DONE(s);
    AR_FINALIZE(s);

    if (ok) ar_run_after_save(cbs, (void *)t);
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
    if (!s) return false;
    AR_BIND_BLOB(s, 1, txid, 32);
    if (!AR_STEP_ROW(s)) { AR_FINALIZE(s); return false; }
    db_wallet_tx_read_row(s, 0, out);
    AR_FINALIZE(s);
    return true;
}

bool db_wallet_tx_delete(struct node_db *ndb, const uint8_t txid[32])
{
    if (!ndb->open) return false;

    struct ar_callbacks *cbs = db_wallet_tx_callbacks();
    struct db_wallet_tx t;
    memset(&t, 0, sizeof(t));
    memcpy(t.txid, txid, 32);
    if (!ar_run_before_destroy(cbs, &t)) return false;

    /* dependent: :destroy — delete child wallet_utxos */
    sqlite3_stmt *du = NULL;
    sqlite3_prepare_v2(ndb->db,
        "DELETE FROM wallet_utxos WHERE txid=?", -1, &du, NULL);
    if (du) {
        AR_BIND_BLOB(du, 1, txid, 32);
        (void)AR_STEP_DONE(du);
        AR_FINALIZE(du);
    }

    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ndb->db,
        "DELETE FROM wallet_transactions WHERE txid=?", -1, &s, NULL);
    if (!s) return false;
    AR_BIND_BLOB(s, 1, txid, 32);
    bool ok = AR_STEP_DONE(s);
    AR_FINALIZE(s);

    if (ok) ar_run_after_destroy(cbs, &t);
    return ok;
}

int db_wallet_tx_count(struct node_db *ndb)
{
    if (!ndb->open) return 0;
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ndb->db,
        "SELECT COUNT(*) FROM wallet_transactions", -1, &s, NULL);
    int c = 0;
    if (s && AR_STEP_ROW(s))
        c = (int)AR_COL_INT(s, 0);
    AR_FINALIZE(s);
    return c;
}

void db_wallet_tx_free(struct db_wallet_tx *t)
{
    if (!t) return;
    free(t->raw_tx);
    t->raw_tx = NULL;
    t->raw_tx_len = 0;
}

void db_wallet_utxo_free(struct db_wallet_utxo *u)
{
    if (!u) return;
    free(u->script);
    u->script = NULL;
    u->script_len = 0;
}

void db_sapling_note_free(struct db_sapling_note *n)
{
    if (!n) return;
    free(n->witness_data);
    n->witness_data = NULL;
    n->witness_data_len = 0;
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
    if (!s) return 0;
    AR_BIND_INT(s, 1, (int)max);
    AR_BIND_INT(s, 2, (int64_t)offset);
    int count = 0;
    while (AR_STEP_ROW(s) && (size_t)count < max) {
        db_wallet_tx_read_row(s, 0, &out[count]);
        count++;
    }
    AR_FINALIZE(s);
    return count;
}

int db_wallet_tx_at_height(struct node_db *ndb, int height,
                           struct db_wallet_tx *out, size_t max)
{
    if (!ndb->open) return 0;
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ndb->db,
        "SELECT txid,raw_tx,block_hash,block_height,time_received,from_me,fee"
        " FROM wallet_transactions WHERE block_height=?",
        -1, &s, NULL);
    if (!s) return 0;
    AR_BIND_INT(s, 1, height);
    int count = 0;
    while (AR_STEP_ROW(s) && (size_t)count < max) {
        db_wallet_tx_read_row(s, 0, &out[count]);
        count++;
    }
    AR_FINALIZE(s);
    return count;
}

/* ── WalletUTXO CRUD ─────────────────────────────────────────── */

bool db_wallet_utxo_save(struct node_db *ndb, const struct db_wallet_utxo *u)
{
    if (!ndb->open) return false;
    struct ar_errors errors;
    if (!db_wallet_utxo_validate(u, &errors)) {
        AR_LOG_VALIDATION_FAILURE("wallet_utxo", &errors);
        return false;
    }

    struct ar_callbacks *cbs = db_wallet_utxo_callbacks();
    if (!ar_run_before_save(cbs, (void *)u)) return false;

    sqlite3_stmt *s = ndb->stmt_wallet_utxo_insert;
    AR_RESET(s);
    AR_BIND_BLOB(s, 1, u->txid, 32);
    AR_BIND_INT(s, 2, (int)u->vout);
    AR_BIND_INT(s, 3, u->value);
    AR_BIND_BLOB(s, 4, u->address_hash, 20);
    AR_BIND_BLOB(s, 5, u->script, (int)u->script_len);
    AR_BIND_INT(s, 6, u->height);
    AR_BIND_INT(s, 7, u->is_coinbase ? 1 : 0);
    bool ok = AR_STEP_DONE(s);

    if (ok) ar_run_after_save(cbs, (void *)u);
    return ok;
}

bool db_wallet_utxo_mark_spent(struct node_db *ndb,
                               const uint8_t txid[32], uint32_t vout,
                               const uint8_t spent_by[32], int vin)
{
    if (!ndb->open) return false;
    sqlite3_stmt *s = ndb->stmt_wallet_utxo_spend;
    AR_RESET(s);
    AR_BIND_BLOB(s, 1, spent_by, 32);
    AR_BIND_INT(s, 2, vin);
    AR_BIND_BLOB(s, 3, txid, 32);
    AR_BIND_INT(s, 4, (int)vout);
    if (!AR_STEP_DONE(s)) return false;
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
    if (!s) return false;
    AR_BIND_BLOB(s, 1, txid, 32);
    AR_BIND_INT(s, 2, (int)vout);
    if (!AR_STEP_ROW(s)) { AR_FINALIZE(s); return false; }

    memset(out, 0, sizeof(*out));
    memcpy(out->txid, txid, 32);
    out->vout = vout;
    out->value = AR_COL_INT(s, 0);
    AR_READ_BLOB(s, 1, out->address_hash, 20);
    out->script_len = (size_t)AR_COL_BYTES(s, 2);
    const void *sc2 = sqlite3_column_blob(s, 2);
    if (sc2 && out->script_len > 0) {
        out->script = malloc(out->script_len);
        if (out->script)
            memcpy(out->script, sc2, out->script_len);
    } else {
        out->script = NULL;
    }
    out->height = (int)AR_COL_INT(s, 3);
    const void *st = sqlite3_column_blob(s, 4);
    if (st && sqlite3_column_bytes(s, 4) >= 32) {
        memcpy(out->spent_txid, st, 32);
        out->is_spent = true;
    }
    if (sqlite3_column_type(s, 5) != SQLITE_NULL)
        out->spent_vin = (int)AR_COL_INT(s, 5);
    out->is_coinbase = AR_COL_INT(s, 6) != 0;
    AR_FINALIZE(s);
    return true;
}

int64_t db_wallet_utxo_balance(struct node_db *ndb)
{
    if (!ndb->open) return 0;
    sqlite3_stmt *s = ndb->stmt_wallet_balance;
    AR_RESET(s);
    int64_t bal = 0;
    if (AR_STEP_ROW(s))
        bal = AR_COL_INT(s, 0);
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
    if (!s) return 0;
    int count = 0;
    while (AR_STEP_ROW(s) && (size_t)count < max) {
        db_wallet_utxo_read_row(s, 0, &out[count]);
        count++;
    }
    AR_FINALIZE(s);
    return count;
}

int db_wallet_utxo_list_all(struct node_db *ndb,
                            struct db_wallet_utxo *out, size_t max)
{
    if (!ndb->open) return 0;
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ndb->db,
        "SELECT txid,vout,value,address_hash,script,height,is_coinbase,"
        "spent_txid,spent_vin"
        " FROM wallet_utxos ORDER BY height ASC",
        -1, &s, NULL);
    if (!s) return 0;
    int count = 0;
    while (AR_STEP_ROW(s) && (size_t)count < max) {
        db_wallet_utxo_read_row(s, 0, &out[count]);
        read_utxo_spent(s, 7, &out[count]);
        count++;
    }
    AR_FINALIZE(s);
    return count;
}

int db_wallet_utxo_select_coins(struct node_db *ndb, int64_t target,
                                int current_height,
                                struct db_wallet_utxo *out, size_t max)
{
    if (!ndb->open) return 0;
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ndb->db,
        "SELECT txid,vout,value,address_hash,script,height,is_coinbase"
        " FROM wallet_utxos"
        " WHERE spent_txid IS NULL"
        "   AND (is_coinbase=0 OR height <= ?)"
        " ORDER BY value DESC",
        -1, &s, NULL);
    if (!s) return 0;
    AR_BIND_INT(s, 1, current_height - 100);
    int count = 0;
    int64_t total = 0;
    while (AR_STEP_ROW(s) && (size_t)count < max) {
        db_wallet_utxo_read_row(s, 0, &out[count]);
        total += out[count].value;
        count++;
        if (total >= target) break;
    }
    AR_FINALIZE(s);
    return count;
}

bool db_wallet_utxo_delete(struct node_db *ndb,
                            const uint8_t txid[32], uint32_t vout)
{
    if (!ndb->open) return false;

    struct ar_callbacks *cbs = db_wallet_utxo_callbacks();
    struct db_wallet_utxo u;
    memset(&u, 0, sizeof(u));
    memcpy(u.txid, txid, 32);
    u.vout = vout;
    if (!ar_run_before_destroy(cbs, &u)) return false;

    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ndb->db,
        "DELETE FROM wallet_utxos WHERE txid=? AND vout=?",
        -1, &s, NULL);
    if (!s) return false;
    AR_BIND_BLOB(s, 1, txid, 32);
    AR_BIND_INT(s, 2, (int)vout);
    bool ok = AR_STEP_DONE(s) && sqlite3_changes(ndb->db) > 0;
    AR_FINALIZE(s);

    if (ok) ar_run_after_destroy(cbs, &u);
    return ok;
}

int db_wallet_utxo_count_for_tx(struct node_db *ndb,
                                 const uint8_t txid[32])
{
    if (!ndb->open) return 0;
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ndb->db,
        "SELECT COUNT(*) FROM wallet_utxos WHERE txid=?",
        -1, &s, NULL);
    if (!s) return 0;
    AR_BIND_BLOB(s, 1, txid, 32);
    int c = 0;
    if (AR_STEP_ROW(s))
        c = (int)AR_COL_INT(s, 0);
    AR_FINALIZE(s);
    return c;
}

bool db_wallet_utxo_delete_all(struct node_db *ndb)
{
    if (!ndb->open) return false;
    return node_db_exec(ndb, "DELETE FROM wallet_utxos");
}

bool db_wallet_tx_delete_all(struct node_db *ndb)
{
    if (!ndb->open) return false;
    return node_db_exec(ndb, "DELETE FROM wallet_transactions");
}

/* ── SaplingNote CRUD ─────────────────────────────────────────── */

bool db_sapling_note_save(struct node_db *ndb, const struct db_sapling_note *n)
{
    if (!ndb->open) return false;
    struct ar_errors errors;
    if (!db_sapling_note_validate(n, &errors)) {
        AR_LOG_VALIDATION_FAILURE("sapling_note", &errors);
        return false;
    }

    struct ar_callbacks *cbs = db_sapling_note_callbacks();
    if (!ar_run_before_save(cbs, (void *)n)) return false;

    /* Derive bech32 z-address if not already set */
    struct db_sapling_note *mut = (struct db_sapling_note *)n;
    if (!mut->address[0]) {
        const struct chain_params *cp = chain_params_get();
        if (cp)
            sapling_encode_payment_address(mut->diversifier, mut->pk_d,
                cp->bech32HRPs[BECH32_SAPLING_PAYMENT_ADDRESS],
                mut->address, sizeof(mut->address));
    }

    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ndb->db,
        "INSERT OR REPLACE INTO wallet_sapling_notes"
        "(txid,output_index,value,rcm,memo,ivk,diversifier,pk_d,cm,"
        "nullifier,block_height,spent_txid,address)"
        " VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)",
        -1, &s, NULL);
    if (!s) return false;
    AR_BIND_BLOB(s, 1, n->txid, 32);
    AR_BIND_INT(s, 2, (int)n->output_index);
    AR_BIND_INT(s, 3, n->value);
    AR_BIND_BLOB(s, 4, n->rcm, 32);
    if (n->memo_len > 0)
        AR_BIND_BLOB(s, 5, n->memo, (int)n->memo_len);
    else
        AR_BIND_NULL(s, 5);
    AR_BIND_BLOB(s, 6, n->ivk, 32);
    AR_BIND_BLOB(s, 7, n->diversifier, 11);
    AR_BIND_BLOB(s, 8, n->pk_d, 32);
    AR_BIND_BLOB(s, 9, n->cm, 32);
    AR_BIND_BLOB(s, 10, n->nullifier, 32);
    if (n->block_height > 0)
        AR_BIND_INT(s, 11, n->block_height);
    else
        AR_BIND_NULL(s, 11);
    if (n->is_spent)
        AR_BIND_BLOB(s, 12, n->spent_txid, 32);
    else
        AR_BIND_NULL(s, 12);
    if (n->address[0])
        AR_BIND_TEXT(s, 13, n->address);
    else
        AR_BIND_NULL(s, 13);
    bool ok = AR_STEP_DONE(s);
    AR_FINALIZE(s);

    if (ok) ar_run_after_save(cbs, (void *)n);
    return ok;
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
    if (!s) return false;
    AR_BIND_BLOB(s, 1, spent_by, 32);
    AR_BIND_BLOB(s, 2, nullifier, 32);
    bool ok = AR_STEP_DONE(s);
    AR_FINALIZE(s);
    return ok;
}

bool db_sapling_note_is_nullifier_spent(struct node_db *ndb,
                                        const uint8_t nullifier[32])
{
    if (!ndb->open) return false;
    sqlite3_stmt *s = ndb->stmt_nullifier_exists;
    AR_RESET(s);
    AR_BIND_BLOB(s, 1, nullifier, 32);
    return AR_STEP_ROW(s);
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
    if (s && AR_STEP_ROW(s))
        bal = AR_COL_INT(s, 0);
    AR_FINALIZE(s);
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
    if (!s) return 0;
    AR_BIND_BLOB(s, 1, ivk, 32);
    int64_t bal = 0;
    if (AR_STEP_ROW(s))
        bal = AR_COL_INT(s, 0);
    AR_FINALIZE(s);
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
    if (!s) return 0;
    int count = 0;
    while (AR_STEP_ROW(s) && (size_t)count < max) {
        db_sapling_note_read_row(s, 0, &out[count]);
        count++;
    }
    AR_FINALIZE(s);
    return count;
}

int db_sapling_note_list_unspent_for_ivk(struct node_db *ndb,
                                          const uint8_t ivk[32],
                                          struct db_sapling_note *out, size_t max)
{
    if (!ndb->open) return 0;
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ndb->db,
        "SELECT txid,output_index,value,rcm,memo,ivk,diversifier,pk_d,"
        "cm,nullifier,block_height"
        " FROM wallet_sapling_notes WHERE spent_txid IS NULL AND ivk=?"
        " ORDER BY value DESC",
        -1, &s, NULL);
    if (!s) return 0;
    AR_BIND_BLOB(s, 1, ivk, 32);
    int count = 0;
    while (AR_STEP_ROW(s) && (size_t)count < max) {
        db_sapling_note_read_row(s, 0, &out[count]);
        count++;
    }
    AR_FINALIZE(s);
    return count;
}

int db_sapling_note_list_all(struct node_db *ndb,
                              struct db_sapling_note *out, size_t max)
{
    if (!ndb->open) return 0;
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ndb->db,
        "SELECT txid,output_index,value,rcm,memo,ivk,diversifier,pk_d,"
        "cm,nullifier,block_height,spent_txid"
        " FROM wallet_sapling_notes ORDER BY block_height DESC",
        -1, &s, NULL);
    if (!s) return 0;
    int count = 0;
    while (AR_STEP_ROW(s) && (size_t)count < max) {
        db_sapling_note_read_row(s, 0, &out[count]);
        read_spent_txid(s, 11, &out[count]);
        count++;
    }
    AR_FINALIZE(s);
    return count;
}

bool db_sapling_note_save_witness(struct node_db *ndb,
                                   const uint8_t txid[32], uint32_t output_index,
                                   const uint8_t *witness_blob, size_t blob_len,
                                   int height)
{
    if (!ndb->open) return false;
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ndb->db,
        "UPDATE wallet_sapling_notes SET witness_data=?,witness_height=?"
        " WHERE txid=? AND output_index=?",
        -1, &s, NULL);
    if (!s) return false;
    AR_BIND_BLOB(s, 1, witness_blob, (int)blob_len);
    AR_BIND_INT(s, 2, height);
    AR_BIND_BLOB(s, 3, txid, 32);
    AR_BIND_INT(s, 4, (int)output_index);
    bool ok = AR_STEP_DONE(s);
    AR_FINALIZE(s);
    return ok;
}

bool db_sapling_note_load_witness(struct node_db *ndb,
                                   const uint8_t txid[32], uint32_t output_index,
                                   uint8_t **witness_blob_out, size_t *blob_len_out,
                                   int *height_out)
{
    if (!ndb->open) return false;
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ndb->db,
        "SELECT witness_data,witness_height FROM wallet_sapling_notes"
        " WHERE txid=? AND output_index=?",
        -1, &s, NULL);
    if (!s) return false;
    AR_BIND_BLOB(s, 1, txid, 32);
    AR_BIND_INT(s, 2, (int)output_index);
    if (!AR_STEP_ROW(s)) { AR_FINALIZE(s); return false; }

    int wlen = AR_COL_BYTES(s, 0);
    const void *wdata = sqlite3_column_blob(s, 0);
    if (!wdata || wlen <= 0 || wlen > 8192) { AR_FINALIZE(s); return false; }

    *witness_blob_out = malloc((size_t)wlen);
    if (!*witness_blob_out) { AR_FINALIZE(s); return false; }
    memcpy(*witness_blob_out, wdata, (size_t)wlen);
    *blob_len_out = (size_t)wlen;
    if (height_out)
        *height_out = (int)AR_COL_INT(s, 1);
    AR_FINALIZE(s);
    return true;
}

/* ── Relationships ─────────────────────────────────────────────── */

/* WalletTx has_many :wallet_utxos */
int db_wallet_tx_utxos(struct node_db *ndb, const uint8_t txid[32],
                        struct db_wallet_utxo *out, size_t max)
{
    if (!ndb->open) return 0;
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ndb->db,
        "SELECT txid,vout,value,address_hash,script,height,is_coinbase,"
        "spent_txid,spent_vin"
        " FROM wallet_utxos WHERE txid=? ORDER BY vout",
        -1, &s, NULL);
    if (!s) return 0;
    AR_BIND_BLOB(s, 1, txid, 32);
    int count = 0;
    while (AR_STEP_ROW(s) && (size_t)count < max) {
        db_wallet_utxo_read_row(s, 0, &out[count]);
        read_utxo_spent(s, 7, &out[count]);
        count++;
    }
    AR_FINALIZE(s);
    return count;
}

/* WalletTx has_many :sapling_notes */
int db_wallet_tx_notes(struct node_db *ndb, const uint8_t txid[32],
                        struct db_sapling_note *out, size_t max)
{
    if (!ndb->open) return 0;
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ndb->db,
        "SELECT txid,output_index,value,rcm,memo,ivk,diversifier,pk_d,"
        "cm,nullifier,block_height,spent_txid"
        " FROM wallet_sapling_notes WHERE txid=? ORDER BY output_index",
        -1, &s, NULL);
    if (!s) return 0;
    AR_BIND_BLOB(s, 1, txid, 32);
    int count = 0;
    while (AR_STEP_ROW(s) && (size_t)count < max) {
        db_sapling_note_read_row(s, 0, &out[count]);
        read_spent_txid(s, 11, &out[count]);
        count++;
    }
    AR_FINALIZE(s);
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
