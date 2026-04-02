/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * ActiveRecord model: ZSLP Token & Transfer
 *
 * Token:
 *   validates :token_id, presence: true
 *   validates :ticker, presence: true, length: { max: 10 }
 *   validates :decimals, range: [0, 8]
 *   validates :genesis_height, numericality: { >= 0 }
 *   validates :initial_quantity, numericality: { >= 0 }
 *
 * Transfer:
 *   validates :txid, :token_id, presence: true
 *   validates :block_height, :amount, :vout, numericality: { >= 0 }
 *   validates :tx_type, range: [1, 3] */

#include "models/zslp.h"
#include "models/activerecord.h"
#include "event/event.h"
#include <string.h>
#include <stdio.h>

/* ── Callbacks ─────────────────────────────────────────────────── */

DEFINE_MODEL_CALLBACKS(zslp_token)
DEFINE_MODEL_CALLBACKS(zslp_transfer)

/* ── Token Validation ─────────────────────────────────────────── */

/* Use a temporary struct for validates_* macros */
struct zslp_token_record {
    uint8_t token_id[32];
    int decimals;
    int genesis_height;
    int64_t initial_quantity;
};

static bool validate_token(const uint8_t token_id[32], const char *ticker,
                           int decimals, int genesis_height,
                           int64_t initial_quantity)
{
    struct ar_errors errors;
    ar_errors_clear(&errors);

    struct zslp_token_record rec;
    memcpy(rec.token_id, token_id, 32);
    rec.decimals = decimals;
    rec.genesis_height = genesis_height;
    rec.initial_quantity = initial_quantity;

    validates_presence_of(&errors, &rec, token_id);
    validates_string_present(&errors, ticker, "ticker");
    validates_custom(&errors, !ticker || strlen(ticker) <= 10,
                     "ticker", "exceeds max length 10");
    validates_range(&errors, &rec, decimals, 0, 8);
    validates_non_negative(&errors, &rec, genesis_height);
    validates_non_negative(&errors, &rec, initial_quantity);

    if (ar_errors_any(&errors)) {
        AR_LOG_VALIDATION_FAILURE("zslp_token", &errors);
        return false;
    }
    return true;
}

/* ── Transfer Validation ──────────────────────────────────────── */

struct zslp_transfer_record {
    uint8_t txid[32];
    uint8_t token_id[32];
    int block_height;
    int tx_type;
    int64_t amount;
    int vout;
};

static bool validate_transfer(const uint8_t txid[32], int block_height,
                              const uint8_t token_id[32], int tx_type,
                              int64_t amount, int vout)
{
    struct ar_errors errors;
    ar_errors_clear(&errors);

    struct zslp_transfer_record rec;
    memcpy(rec.txid, txid, 32);
    memcpy(rec.token_id, token_id, 32);
    rec.block_height = block_height;
    rec.tx_type = tx_type;
    rec.amount = amount;
    rec.vout = vout;

    validates_presence_of(&errors, &rec, txid);
    validates_presence_of(&errors, &rec, token_id);
    validates_non_negative(&errors, &rec, block_height);
    validates_range(&errors, &rec, tx_type, 1, 3);
    validates_non_negative(&errors, &rec, amount);
    validates_non_negative(&errors, &rec, vout);

    if (ar_errors_any(&errors)) {
        AR_LOG_VALIDATION_FAILURE("zslp_transfer", &errors);
        return false;
    }
    return true;
}

/* ── CRUD ──────────────────────────────────────────────────────── */

bool db_zslp_token_save(struct node_db *ndb, const uint8_t token_id[32],
                         const char *ticker, const char *name,
                         int decimals, const char *document_url,
                         int genesis_height, int64_t initial_quantity)
{
    if (!ndb || !ndb->open) return false;
    if (!validate_token(token_id, ticker, decimals, genesis_height,
                        initial_quantity))
        return false;

    struct zslp_token_record rec;
    memcpy(rec.token_id, token_id, 32);
    rec.decimals = decimals;
    rec.genesis_height = genesis_height;
    rec.initial_quantity = initial_quantity;
    if (!ar_run_before_save(db_zslp_token_callbacks(), &rec)) return false;

    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(ndb->db,
            "INSERT OR IGNORE INTO zslp_tokens"
            "(token_id,ticker,name,decimals,document_url,"
            "genesis_height,total_minted) VALUES(?,?,?,?,?,?,?)",
            -1, &s, NULL) != SQLITE_OK || !s)
        return false;

    AR_BIND_BLOB(s, 1, token_id, 32);
    AR_BIND_TEXT(s, 2, ticker);
    AR_BIND_TEXT(s, 3, name ? name : "");
    AR_BIND_INT(s, 4, decimals);
    AR_BIND_TEXT(s, 5, document_url ? document_url : "");
    AR_BIND_INT(s, 6, genesis_height);
    AR_BIND_INT(s, 7, initial_quantity);

    bool ok = AR_STEP_DONE(s);
    AR_FINALIZE(s);

    if (ok) {
        ar_run_after_save(db_zslp_token_callbacks(), &rec);
        event_emitf(EV_MODEL_SAVED, 0, "model=zslp_token ticker=%s", ticker);
    }
    return ok;
}

bool db_zslp_transfer_save(struct node_db *ndb, const uint8_t txid[32],
                            int block_height, const uint8_t token_id[32],
                            int tx_type, int64_t amount, int vout,
                            const uint8_t *to_addr)
{
    if (!ndb || !ndb->open) return false;
    if (!validate_transfer(txid, block_height, token_id, tx_type,
                           amount, vout))
        return false;

    struct zslp_transfer_record rec;
    memcpy(rec.txid, txid, 32);
    memcpy(rec.token_id, token_id, 32);
    rec.block_height = block_height;
    rec.tx_type = tx_type;
    rec.amount = amount;
    rec.vout = vout;
    if (!ar_run_before_save(db_zslp_transfer_callbacks(), &rec)) return false;

    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(ndb->db,
            "INSERT OR IGNORE INTO zslp_transfers"
            "(txid,block_height,token_id,tx_type,amount,vout,to_addr)"
            " VALUES(?,?,?,?,?,?,?)",
            -1, &s, NULL) != SQLITE_OK || !s)
        return false;

    AR_BIND_BLOB(s, 1, txid, 32);
    AR_BIND_INT(s, 2, block_height);
    AR_BIND_BLOB(s, 3, token_id, 32);
    AR_BIND_INT(s, 4, tx_type);
    AR_BIND_INT(s, 5, amount);
    AR_BIND_INT(s, 6, vout);
    if (to_addr)
        AR_BIND_BLOB(s, 7, to_addr, 20);
    else
        AR_BIND_NULL(s, 7);

    bool ok = AR_STEP_DONE(s);
    AR_FINALIZE(s);

    if (ok)
        ar_run_after_save(db_zslp_transfer_callbacks(), &rec);
    return ok;
}

int64_t db_zslp_token_count(struct node_db *ndb)
{
    if (!ndb || !ndb->open) return 0;
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(ndb->db,
            "SELECT count(*) FROM zslp_tokens",
            -1, &s, NULL) != SQLITE_OK || !s)
        return 0;
    int64_t count = 0;
    if (AR_STEP_ROW(s))
        count = AR_COL_INT(s, 0);
    AR_FINALIZE(s);
    return count;
}

int64_t db_zslp_transfer_count(struct node_db *ndb)
{
    if (!ndb || !ndb->open) return 0;
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(ndb->db,
            "SELECT count(*) FROM zslp_transfers",
            -1, &s, NULL) != SQLITE_OK || !s)
        return 0;
    int64_t count = 0;
    if (AR_STEP_ROW(s))
        count = AR_COL_INT(s, 0);
    AR_FINALIZE(s);
    return count;
}

void db_zslp_clear_all(struct node_db *ndb)
{
    if (!ndb || !ndb->open) return;
    node_db_exec(ndb, "DELETE FROM zslp_tokens");
    node_db_exec(ndb, "DELETE FROM zslp_transfers");
}
