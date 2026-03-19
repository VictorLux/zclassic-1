/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * ZSLP token model — CRUD operations for SLP tokens and transfers. */

#include "models/zslp.h"
#include <string.h>
#include <stdio.h>

bool db_zslp_token_save(struct node_db *ndb, const uint8_t token_id[32],
                         const char *ticker, const char *name,
                         int decimals, const char *document_url,
                         int genesis_height, int64_t initial_quantity)
{
    if (!ndb || !ndb->open) return false;

    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(ndb->db,
            "INSERT OR IGNORE INTO zslp_tokens"
            "(token_id,ticker,name,decimals,document_url,"
            "genesis_height,total_minted) VALUES(?,?,?,?,?,?,?)",
            -1, &s, NULL) != SQLITE_OK || !s)
        return false;

    sqlite3_bind_blob(s, 1, token_id, 32, SQLITE_STATIC);
    sqlite3_bind_text(s, 2, ticker ? ticker : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 3, name ? name : "", -1, SQLITE_STATIC);
    sqlite3_bind_int(s, 4, decimals);
    sqlite3_bind_text(s, 5, document_url ? document_url : "", -1, SQLITE_STATIC);
    sqlite3_bind_int(s, 6, genesis_height);
    sqlite3_bind_int64(s, 7, initial_quantity);

    bool ok = sqlite3_step(s) == SQLITE_DONE;
    sqlite3_finalize(s);
    return ok;
}

bool db_zslp_transfer_save(struct node_db *ndb, const uint8_t txid[32],
                            int block_height, const uint8_t token_id[32],
                            int tx_type, int64_t amount, int vout,
                            const uint8_t *to_addr)
{
    if (!ndb || !ndb->open) return false;

    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(ndb->db,
            "INSERT OR IGNORE INTO zslp_transfers"
            "(txid,block_height,token_id,tx_type,amount,vout,to_addr)"
            " VALUES(?,?,?,?,?,?,?)",
            -1, &s, NULL) != SQLITE_OK || !s)
        return false;

    sqlite3_bind_blob(s, 1, txid, 32, SQLITE_STATIC);
    sqlite3_bind_int(s, 2, block_height);
    sqlite3_bind_blob(s, 3, token_id, 32, SQLITE_STATIC);
    sqlite3_bind_int(s, 4, tx_type);
    sqlite3_bind_int64(s, 5, amount);
    sqlite3_bind_int(s, 6, vout);
    if (to_addr)
        sqlite3_bind_blob(s, 7, to_addr, 20, SQLITE_STATIC);
    else
        sqlite3_bind_null(s, 7);

    bool ok = sqlite3_step(s) == SQLITE_DONE;
    sqlite3_finalize(s);
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
    if (sqlite3_step(s) == SQLITE_ROW)
        count = sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);
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
    if (sqlite3_step(s) == SQLITE_ROW)
        count = sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);
    return count;
}

void db_zslp_clear_all(struct node_db *ndb)
{
    if (!ndb || !ndb->open) return;
    node_db_exec(ndb, "DELETE FROM zslp_tokens");
    node_db_exec(ndb, "DELETE FROM zslp_transfers");
}
