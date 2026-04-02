/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "models/database.h"
#include <stdio.h>
#include <string.h>

static const char *SCHEMA[] = {
    /* Blockchain */
    "CREATE TABLE IF NOT EXISTS blocks ("
    "hash BLOB PRIMARY KEY,height INTEGER NOT NULL,"
    "prev_hash BLOB NOT NULL,version INTEGER NOT NULL,"
    "merkle_root BLOB NOT NULL,time INTEGER NOT NULL,"
    "bits INTEGER NOT NULL,nonce BLOB NOT NULL,"
    "solution BLOB NOT NULL,chain_work BLOB NOT NULL,"
    "status INTEGER NOT NULL DEFAULT 0,"
    "file_num INTEGER,data_pos INTEGER,undo_pos INTEGER,"
    "num_tx INTEGER NOT NULL DEFAULT 0,"
    "sapling_root BLOB,sprout_root BLOB,"
    "sapling_value INTEGER DEFAULT 0,"
    "sprout_value INTEGER DEFAULT 0)",

    "CREATE UNIQUE INDEX IF NOT EXISTS idx_blocks_height"
    " ON blocks(height) WHERE status >= 3",

    "CREATE INDEX IF NOT EXISTS idx_blocks_prev"
    " ON blocks(prev_hash)",

    "CREATE INDEX IF NOT EXISTS idx_blocks_chainwork"
    " ON blocks(chain_work DESC)",

    /* Transaction index */
    "CREATE TABLE IF NOT EXISTS transactions ("
    "txid BLOB PRIMARY KEY,block_hash BLOB NOT NULL,"
    "block_height INTEGER NOT NULL,tx_index INTEGER NOT NULL,"
    "file_num INTEGER NOT NULL,file_pos INTEGER NOT NULL,"
    "is_coinbase INTEGER NOT NULL DEFAULT 0)",

    "CREATE INDEX IF NOT EXISTS idx_tx_block"
    " ON transactions(block_hash)",

    "CREATE INDEX IF NOT EXISTS idx_tx_height"
    " ON transactions(block_height)",

    /* UTXO set */
    "CREATE TABLE IF NOT EXISTS utxos ("
    "txid BLOB NOT NULL,vout INTEGER NOT NULL,"
    "value INTEGER NOT NULL CHECK(value >= 0 AND value <= 2100000000000000),"
    "script BLOB NOT NULL,"
    "script_type INTEGER NOT NULL DEFAULT 0,"
    "address_hash BLOB,height INTEGER NOT NULL CHECK(height >= 0),"
    "is_coinbase INTEGER NOT NULL DEFAULT 0,"
    "PRIMARY KEY (txid,vout))",

    "CREATE INDEX IF NOT EXISTS idx_utxo_address"
    " ON utxos(address_hash) WHERE address_hash IS NOT NULL",

    "CREATE INDEX IF NOT EXISTS idx_utxo_value"
    " ON utxos(value DESC)",

    "CREATE INDEX IF NOT EXISTS idx_utxo_height"
    " ON utxos(height)",

    /* Sapling nullifiers & anchors */
    "CREATE TABLE IF NOT EXISTS sapling_nullifiers ("
    "nullifier BLOB PRIMARY KEY)",

    "CREATE TABLE IF NOT EXISTS sapling_anchors ("
    "anchor BLOB PRIMARY KEY,height INTEGER NOT NULL)",

    "CREATE INDEX IF NOT EXISTS idx_sapling_anchor_height"
    " ON sapling_anchors(height)",

    /* Wallet keys */
    "CREATE TABLE IF NOT EXISTS wallet_keys ("
    "pubkey_hash BLOB PRIMARY KEY,pubkey BLOB NOT NULL,"
    "privkey BLOB NOT NULL,compressed INTEGER NOT NULL DEFAULT 1,"
    "created_at INTEGER NOT NULL DEFAULT 0)",

    "CREATE TABLE IF NOT EXISTS wallet_sapling_keys ("
    "ivk BLOB PRIMARY KEY,xsk BLOB NOT NULL,xfvk BLOB NOT NULL,"
    "diversifier BLOB NOT NULL,pk_d BLOB NOT NULL,"
    "child_index INTEGER NOT NULL,"
    "address TEXT NOT NULL DEFAULT '')",

    "CREATE INDEX IF NOT EXISTS idx_sapling_key_addr"
    " ON wallet_sapling_keys(address)",

    "CREATE TABLE IF NOT EXISTS wallet_scripts ("
    "script_hash BLOB PRIMARY KEY,redeem_script BLOB NOT NULL)",

    "CREATE TABLE IF NOT EXISTS wallet_seed ("
    "id INTEGER PRIMARY KEY CHECK (id=1),"
    "seed BLOB NOT NULL,next_child INTEGER NOT NULL DEFAULT 0)",

    /* Wallet transactions & notes */
    "CREATE TABLE IF NOT EXISTS wallet_transactions ("
    "txid BLOB PRIMARY KEY,raw_tx BLOB NOT NULL,"
    "block_hash BLOB,block_height INTEGER,"
    "time_received INTEGER NOT NULL,"
    "from_me INTEGER NOT NULL DEFAULT 0,fee INTEGER)",

    "CREATE INDEX IF NOT EXISTS idx_wtx_height"
    " ON wallet_transactions(block_height)",

    "CREATE INDEX IF NOT EXISTS idx_wtx_time"
    " ON wallet_transactions(time_received DESC)",

    "CREATE TABLE IF NOT EXISTS wallet_utxos ("
    "txid BLOB NOT NULL,vout INTEGER NOT NULL,"
    "value INTEGER NOT NULL CHECK(value >= 0 AND value <= 2100000000000000),"
    "address_hash BLOB NOT NULL,"
    "script BLOB NOT NULL,height INTEGER NOT NULL CHECK(height >= 0),"
    "spent_txid BLOB,spent_vin INTEGER,"
    "is_coinbase INTEGER NOT NULL DEFAULT 0,"
    "PRIMARY KEY (txid,vout))",

    "CREATE INDEX IF NOT EXISTS idx_wutxo_unspent"
    " ON wallet_utxos(address_hash) WHERE spent_txid IS NULL",

    "CREATE INDEX IF NOT EXISTS idx_wutxo_spent"
    " ON wallet_utxos(spent_txid) WHERE spent_txid IS NOT NULL",

    "CREATE TABLE IF NOT EXISTS wallet_sapling_notes ("
    "txid BLOB NOT NULL,output_index INTEGER NOT NULL,"
    "value INTEGER NOT NULL,rcm BLOB NOT NULL,memo BLOB,"
    "ivk BLOB NOT NULL,diversifier BLOB NOT NULL,"
    "pk_d BLOB NOT NULL,cm BLOB NOT NULL,"
    "nullifier BLOB NOT NULL UNIQUE,"
    "block_height INTEGER,spent_txid BLOB,"
    "address TEXT,"
    "PRIMARY KEY (txid,output_index))",

    "CREATE INDEX IF NOT EXISTS idx_snote_unspent"
    " ON wallet_sapling_notes(ivk) WHERE spent_txid IS NULL",

    "CREATE INDEX IF NOT EXISTS idx_snote_nullifier"
    " ON wallet_sapling_notes(nullifier)",

    "CREATE INDEX IF NOT EXISTS idx_snote_address"
    " ON wallet_sapling_notes(address) WHERE spent_txid IS NULL",

    /* Mempool */
    "CREATE TABLE IF NOT EXISTS mempool ("
    "txid BLOB PRIMARY KEY,raw_tx BLOB NOT NULL,"
    "fee INTEGER NOT NULL,size INTEGER NOT NULL,"
    "time_added INTEGER NOT NULL,height_added INTEGER NOT NULL,"
    "spends_coinbase INTEGER NOT NULL DEFAULT 0)",

    "CREATE INDEX IF NOT EXISTS idx_mempool_fee"
    " ON mempool(fee DESC)",

    "CREATE TABLE IF NOT EXISTS mempool_spends ("
    "txid BLOB NOT NULL,spent_txid BLOB NOT NULL,"
    "spent_vout INTEGER NOT NULL,"
    "PRIMARY KEY (spent_txid,spent_vout))",

    /* Peers */
    "CREATE TABLE IF NOT EXISTS peers ("
    "id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "ip BLOB NOT NULL,port INTEGER NOT NULL,"
    "services INTEGER NOT NULL DEFAULT 0,"
    "last_seen INTEGER NOT NULL,last_try INTEGER DEFAULT 0,"
    "attempts INTEGER DEFAULT 0,source BLOB,"
    "bandwidth_score INTEGER NOT NULL DEFAULT 0,"
    "is_zcl23 INTEGER NOT NULL DEFAULT 0,"
    "UNIQUE(ip,port))",

    "CREATE INDEX IF NOT EXISTS idx_peers_seen"
    " ON peers(last_seen DESC)",

    "CREATE INDEX IF NOT EXISTS idx_peers_zcl23_score"
    " ON peers(is_zcl23 DESC, bandwidth_score DESC)",

    /* File services */
    "CREATE TABLE IF NOT EXISTS file_services ("
    "ip BLOB NOT NULL,port INTEGER NOT NULL,"
    "p2p_port INTEGER,last_seen INTEGER,"
    "is_zcl23 INTEGER DEFAULT 1,"
    "UNIQUE(ip,port))",

    /* Node state */
    "CREATE TABLE IF NOT EXISTS node_state ("
    "key TEXT PRIMARY KEY,value BLOB)",

    "INSERT OR IGNORE INTO node_state(key,value)"
    " VALUES('schema_version',X'01000000')",

    NULL
};

static bool create_schema(struct node_db *ndb)
{
    for (int i = 0; SCHEMA[i] != NULL; i++) {
        char *err = NULL;
        int rc = sqlite3_exec(ndb->db, SCHEMA[i], NULL, NULL, &err);
        if (rc != SQLITE_OK) {
            /* Non-fatal for index/alter on pre-existing tables */
            if (strstr(SCHEMA[i], "CREATE INDEX") ||
                strstr(SCHEMA[i], "ALTER TABLE")) {
                sqlite3_free(err);
                continue;
            }
            fprintf(stderr, "db: schema[%d] failed: %s\n", i, err);
            sqlite3_free(err);
            return false;
        }
    }
    return true;
}

static bool prepare_statements(struct node_db *ndb)
{
    sqlite3 *db = ndb->db;
    int rc;

#define PREP(field, sql) do { \
    rc = sqlite3_prepare_v2(db, sql, -1, &ndb->field, NULL); \
    if (rc != SQLITE_OK) { \
        fprintf(stderr, "db: prepare %s: %s\n", #field, \
                sqlite3_errmsg(db)); \
        return false; \
    } \
} while (0)

    PREP(stmt_utxo_insert,
         "INSERT OR REPLACE INTO utxos"
         "(txid,vout,value,script,script_type,"
         "address_hash,height,is_coinbase)"
         " VALUES(?,?,?,?,?,?,?,?)");

    PREP(stmt_utxo_delete,
         "DELETE FROM utxos WHERE txid=? AND vout=?");

    PREP(stmt_utxo_find,
         "SELECT value,script,script_type,"
         "address_hash,height,is_coinbase"
         " FROM utxos WHERE txid=? AND vout=?");

    PREP(stmt_block_insert,
         "INSERT OR REPLACE INTO blocks"
         "(hash,height,prev_hash,version,merkle_root,"
         "time,bits,nonce,solution,chain_work,status,"
         "file_num,data_pos,undo_pos,num_tx,"
         "sapling_root,sprout_root,"
         "sapling_value,sprout_value)"
         " VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");

    PREP(stmt_block_by_hash,
         "SELECT height,prev_hash,version,merkle_root,"
         "time,bits,nonce,solution,chain_work,status,"
         "file_num,data_pos,undo_pos,num_tx,"
         "sapling_root,sprout_root,"
         "sapling_value,sprout_value"
         " FROM blocks WHERE hash=?");

    PREP(stmt_block_by_height,
         "SELECT hash,prev_hash,version,merkle_root,"
         "time,bits,nonce,solution,chain_work,status,"
         "file_num,data_pos,undo_pos,num_tx,"
         "sapling_root,sprout_root,"
         "sapling_value,sprout_value"
         " FROM blocks WHERE height=?"
         " AND status>=3 LIMIT 1");

    PREP(stmt_tx_insert,
         "INSERT OR REPLACE INTO transactions"
         "(txid,block_hash,block_height,"
         "tx_index,file_num,file_pos,is_coinbase)"
         " VALUES(?,?,?,?,?,?,?)");

    PREP(stmt_tx_find,
         "SELECT block_hash,block_height,tx_index,"
         "file_num,file_pos,is_coinbase"
         " FROM transactions WHERE txid=?");

    PREP(stmt_wallet_utxo_insert,
         "INSERT OR REPLACE INTO wallet_utxos"
         "(txid,vout,value,address_hash,"
         "script,height,is_coinbase)"
         " VALUES(?,?,?,?,?,?,?)");

    PREP(stmt_wallet_utxo_spend,
         "UPDATE wallet_utxos"
         " SET spent_txid=?,spent_vin=?"
         " WHERE txid=? AND vout=?");

    PREP(stmt_wallet_balance,
         "SELECT COALESCE(SUM(value),0)"
         " FROM wallet_utxos"
         " WHERE spent_txid IS NULL");

    PREP(stmt_nullifier_insert,
         "INSERT OR IGNORE INTO"
         " sapling_nullifiers(nullifier)"
         " VALUES(?)");

    PREP(stmt_nullifier_exists,
         "SELECT 1 FROM sapling_nullifiers"
         " WHERE nullifier=?");

    PREP(stmt_state_set,
         "INSERT OR REPLACE INTO"
         " node_state(key,value) VALUES(?,?)");

    PREP(stmt_state_get,
         "SELECT value FROM node_state"
         " WHERE key=?");

    /* Peer model */
    PREP(stmt_peer_save,
         "INSERT OR REPLACE INTO peers"
         "(ip,port,services,last_seen,last_try,attempts,source,"
         "bandwidth_score,is_zcl23)"
         " VALUES(?,?,?,?,?,?,?,?,?)");

    PREP(stmt_peer_find,
         "SELECT id,ip,port,services,last_seen,last_try,attempts,"
         "source,bandwidth_score,is_zcl23"
         " FROM peers WHERE ip=? AND port=?");

    PREP(stmt_peer_delete,
         "DELETE FROM peers WHERE ip=? AND port=?");

    PREP(stmt_peer_count,
         "SELECT COUNT(*) FROM peers");

    /* File service model */
    PREP(stmt_file_service_save,
         "INSERT OR REPLACE INTO file_services"
         " (ip, port, p2p_port, last_seen, is_zcl23)"
         " VALUES(?, ?, ?, ?, ?)");

    PREP(stmt_file_service_find,
         "SELECT ip, port, p2p_port, last_seen, is_zcl23"
         " FROM file_services WHERE ip=? AND port=?");

#undef PREP
    return true;
}

static void finalize_statements(struct node_db *ndb)
{
    sqlite3_finalize(ndb->stmt_utxo_insert);
    sqlite3_finalize(ndb->stmt_utxo_delete);
    sqlite3_finalize(ndb->stmt_utxo_find);
    sqlite3_finalize(ndb->stmt_block_insert);
    sqlite3_finalize(ndb->stmt_block_by_hash);
    sqlite3_finalize(ndb->stmt_block_by_height);
    sqlite3_finalize(ndb->stmt_tx_insert);
    sqlite3_finalize(ndb->stmt_tx_find);
    sqlite3_finalize(ndb->stmt_wallet_utxo_insert);
    sqlite3_finalize(ndb->stmt_wallet_utxo_spend);
    sqlite3_finalize(ndb->stmt_wallet_balance);
    sqlite3_finalize(ndb->stmt_nullifier_insert);
    sqlite3_finalize(ndb->stmt_nullifier_exists);
    sqlite3_finalize(ndb->stmt_state_set);
    sqlite3_finalize(ndb->stmt_state_get);
    sqlite3_finalize(ndb->stmt_peer_save);
    sqlite3_finalize(ndb->stmt_peer_find);
    sqlite3_finalize(ndb->stmt_peer_delete);
    sqlite3_finalize(ndb->stmt_peer_count);
    sqlite3_finalize(ndb->stmt_file_service_save);
    sqlite3_finalize(ndb->stmt_file_service_find);
}

bool node_db_open(struct node_db *ndb, const char *path)
{
    memset(ndb, 0, sizeof(*ndb));

    int rc = sqlite3_open(path, &ndb->db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "db: cannot open %s: %s\n",
                path, sqlite3_errmsg(ndb->db));
        sqlite3_close(ndb->db);
        return false;
    }

    /* Performance pragmas */
    sqlite3_exec(ndb->db,
        "PRAGMA journal_mode=WAL;"
        "PRAGMA synchronous=NORMAL;"
        "PRAGMA cache_size=-65536;"
        "PRAGMA mmap_size=268435456;"
        "PRAGMA temp_store=MEMORY;"
        "PRAGMA foreign_keys=ON",
        NULL, NULL, NULL);
    sqlite3_busy_timeout(ndb->db, 10000);

    if (!create_schema(ndb)) {
        sqlite3_close(ndb->db);
        return false;
    }

    if (!prepare_statements(ndb)) {
        sqlite3_close(ndb->db);
        return false;
    }

    ndb->open = true;
    return true;
}

void node_db_close(struct node_db *ndb)
{
    if (!ndb->open) return;
    finalize_statements(ndb);
    sqlite3_close(ndb->db);
    ndb->open = false;
}

bool node_db_exec(struct node_db *ndb, const char *sql)
{
    if (!ndb->open) return false;
    char *err = NULL;
    int rc = sqlite3_exec(ndb->db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "db: exec failed: %s\n", err);
        sqlite3_free(err);
        return false;
    }
    return true;
}

bool node_db_begin(struct node_db *ndb)
{
    return node_db_exec(ndb, "BEGIN TRANSACTION");
}

bool node_db_commit(struct node_db *ndb)
{
    return node_db_exec(ndb, "COMMIT");
}

bool node_db_rollback(struct node_db *ndb)
{
    return node_db_exec(ndb, "ROLLBACK");
}

void node_db_set_sync_batch_size(struct node_db *ndb, int batch_size)
{
    if (!ndb) return;
    ndb->sync_batch_size = batch_size > 0 ? batch_size : 1;
}

bool node_db_sync_flush(struct node_db *ndb)
{
    if (!ndb || !ndb->open) return false;
    if (ndb->sync_in_batch) {
        bool ok = node_db_commit(ndb);
        ndb->sync_in_batch = false;
        ndb->sync_pending_blocks = 0;
        return ok;
    }
    return true;
}

bool node_db_state_set(struct node_db *ndb, const char *key,
                       const void *value, size_t len)
{
    if (!ndb->open) return false;
    sqlite3_stmt *s = ndb->stmt_state_set;
    sqlite3_reset(s);
    sqlite3_bind_text(s, 1, key, -1, SQLITE_STATIC);
    sqlite3_bind_blob(s, 2, value, (int)len, SQLITE_STATIC);
    return sqlite3_step(s) == SQLITE_DONE;
}

bool node_db_state_get(struct node_db *ndb, const char *key,
                       void *value, size_t max_len, size_t *out_len)
{
    if (!ndb->open) return false;
    sqlite3_stmt *s = ndb->stmt_state_get;
    sqlite3_reset(s);
    sqlite3_bind_text(s, 1, key, -1, SQLITE_STATIC);
    if (sqlite3_step(s) != SQLITE_ROW) return false;
    int blob_len = sqlite3_column_bytes(s, 0);
    if (blob_len <= 0) return false;
    size_t copy = (size_t)blob_len < max_len
                  ? (size_t)blob_len : max_len;
    memcpy(value, sqlite3_column_blob(s, 0), copy);
    if (out_len) *out_len = copy;
    return true;
}

bool node_db_state_set_int(struct node_db *ndb,
                           const char *key, int64_t val)
{
    return node_db_state_set(ndb, key, &val, sizeof(val));
}

bool node_db_state_get_int(struct node_db *ndb,
                           const char *key, int64_t *val)
{
    size_t len = 0;
    if (!node_db_state_get(ndb, key, val, sizeof(*val), &len))
        return false;
    return len == sizeof(*val);
}

int node_db_schema_version(struct node_db *ndb)
{
    int32_t ver = 0;
    size_t len = 0;
    if (!node_db_state_get(ndb, "schema_version",
                           &ver, sizeof(ver), &len))
        return 0;
    return ver;
}

int node_db_migrate(struct node_db *ndb, const char *datadir)
{
    (void)datadir;
    if (!ndb->open) return -1;

    /* Ensure schema_migrations table exists */
    node_db_exec(ndb,
        "CREATE TABLE IF NOT EXISTS schema_migrations ("
        "  version TEXT PRIMARY KEY,"
        "  applied_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now'))"
        ");"
        "INSERT OR IGNORE INTO schema_migrations(version) VALUES('001');"
    );

    int applied = 0;
    int current_ver = node_db_schema_version(ndb);
    printf("db: current schema version %d\n", current_ver);

    /* Future migrations go here as versioned blocks.
     * Each block checks schema_migrations before running.
     *
     * Pattern:
     *   if (current_ver < N) {
     *       node_db_exec(ndb, "ALTER TABLE ... ; ...");
     *       node_db_exec(ndb,
     *           "INSERT OR IGNORE INTO schema_migrations(version) "
     *           "VALUES('00N')");
     *       current_ver = N;
     *       applied++;
     *   }
     */

    if (current_ver < 2) {
        node_db_exec(ndb,
            "INSERT OR IGNORE INTO schema_migrations(version) VALUES('002')");
        int32_t v = 2;
        node_db_state_set(ndb, "schema_version", &v, sizeof(v));
        current_ver = 2;
        applied++;
    }

    if (current_ver < 3) {
        /* Add Sapling commitment tree tracking columns */
        node_db_exec(ndb,
            "ALTER TABLE blocks ADD COLUMN sapling_tree_data BLOB");
        node_db_exec(ndb,
            "ALTER TABLE wallet_sapling_notes ADD COLUMN witness_data BLOB");
        node_db_exec(ndb,
            "ALTER TABLE wallet_sapling_notes ADD COLUMN witness_height INTEGER DEFAULT 0");
        node_db_exec(ndb,
            "INSERT OR IGNORE INTO schema_migrations(version) VALUES('003')");
        int32_t v = 3;
        node_db_state_set(ndb, "schema_version", &v, sizeof(v));
        current_ver = 3;
        applied++;
    }

    if (current_ver < 4) {
        /* Block index cache — enables instant warm restart by skipping
         * the 11s LevelDB block index load. Verified cryptographically
         * via tip hash match with coins DB. */
        node_db_exec(ndb,
            "CREATE TABLE IF NOT EXISTS block_index_cache ("
            "hash BLOB NOT NULL PRIMARY KEY,"
            "prev_hash BLOB NOT NULL,"
            "height INTEGER NOT NULL,"
            "n_bits INTEGER NOT NULL,"
            "n_time INTEGER NOT NULL,"
            "n_version INTEGER NOT NULL DEFAULT 4,"
            "n_status INTEGER NOT NULL DEFAULT 0,"
            "n_file INTEGER NOT NULL DEFAULT 0,"
            "n_data_pos INTEGER NOT NULL DEFAULT 0,"
            "n_undo_pos INTEGER NOT NULL DEFAULT 0,"
            "n_tx INTEGER NOT NULL DEFAULT 0,"
            "chain_work BLOB,"
            "merkle_root BLOB,"
            "final_sapling_root BLOB,"
            "nonce BLOB,"
            "solution BLOB,"
            "n_solution_size INTEGER NOT NULL DEFAULT 0,"
            "n_cached_branch_id INTEGER NOT NULL DEFAULT 0"
            ")");
        node_db_exec(ndb,
            "CREATE INDEX IF NOT EXISTS idx_bic_height"
            " ON block_index_cache(height)");
        node_db_exec(ndb,
            "INSERT OR IGNORE INTO schema_migrations(version) VALUES('004')");
        int32_t v = 4;
        node_db_state_set(ndb, "schema_version", &v, sizeof(v));
        current_ver = 4;
        applied++;
    }

    if (current_ver < 5) {
        /* v5: Explorer + REST API tables and indexes.
         * Optimized for high-performance read queries. */

        /* Addresses table — aggregated balance cache per address.
         * Rebuilt from UTXO set on demand. */
        node_db_exec(ndb,
            "CREATE TABLE IF NOT EXISTS addresses ("
            "address_hash BLOB PRIMARY KEY,"
            "script_type INTEGER NOT NULL DEFAULT 0,"
            "balance INTEGER NOT NULL DEFAULT 0,"
            "utxo_count INTEGER NOT NULL DEFAULT 0,"
            "first_seen_height INTEGER NOT NULL DEFAULT 0,"
            "last_seen_height INTEGER NOT NULL DEFAULT 0)");

        node_db_exec(ndb,
            "CREATE INDEX IF NOT EXISTS idx_addr_balance"
            " ON addresses(balance DESC)");

        /* Chain stats — pre-computed per-block aggregate stats.
         * Used for charts (difficulty, hashrate, supply). */
        node_db_exec(ndb,
            "CREATE TABLE IF NOT EXISTS chain_stats ("
            "height INTEGER PRIMARY KEY,"
            "time INTEGER NOT NULL,"
            "difficulty REAL NOT NULL DEFAULT 0,"
            "tx_count INTEGER NOT NULL DEFAULT 0,"
            "utxo_count INTEGER NOT NULL DEFAULT 0,"
            "total_supply INTEGER NOT NULL DEFAULT 0,"
            "shielded_supply INTEGER NOT NULL DEFAULT 0,"
            "block_size INTEGER NOT NULL DEFAULT 0)");

        /* ZSLP token registry — discovered from OP_RETURN GENESIS txs */
        node_db_exec(ndb,
            "CREATE TABLE IF NOT EXISTS zslp_tokens ("
            "token_id BLOB PRIMARY KEY,"
            "ticker TEXT NOT NULL DEFAULT '',"
            "name TEXT NOT NULL DEFAULT '',"
            "decimals INTEGER NOT NULL DEFAULT 0,"
            "document_url TEXT DEFAULT '',"
            "genesis_height INTEGER NOT NULL DEFAULT 0,"
            "total_minted INTEGER NOT NULL DEFAULT 0,"
            "total_burned INTEGER NOT NULL DEFAULT 0)");

        node_db_exec(ndb,
            "CREATE INDEX IF NOT EXISTS idx_zslp_ticker"
            " ON zslp_tokens(ticker)");

        /* Additional covering index for block queries without status filter */
        node_db_exec(ndb,
            "CREATE INDEX IF NOT EXISTS idx_blocks_height_all"
            " ON blocks(height)");

        /* Index for timestamp lookups (HODL wave chart) */
        node_db_exec(ndb,
            "CREATE INDEX IF NOT EXISTS idx_blocks_time"
            " ON blocks(time)");

        /* Composite covering index for UTXO age distribution (HODL waves) */
        node_db_exec(ndb,
            "CREATE INDEX IF NOT EXISTS idx_utxo_height_value"
            " ON utxos(height, value)");

        node_db_exec(ndb,
            "INSERT OR IGNORE INTO schema_migrations(version) VALUES('005')");
        int32_t v = 5;
        node_db_state_set(ndb, "schema_version", &v, sizeof(v));
        current_ver = 5;
        applied++;
    }

    if (current_ver < 6) {
        /* v6: Add address column to wallet_sapling_notes for per-order
         * payment matching. Ignore errors — column may already exist
         * or table may not exist yet. */
        sqlite3_exec(ndb->db,
            "ALTER TABLE wallet_sapling_notes ADD COLUMN address TEXT",
            NULL, NULL, NULL);
        sqlite3_exec(ndb->db,
            "CREATE INDEX IF NOT EXISTS idx_snote_address"
            " ON wallet_sapling_notes(address) WHERE spent_txid IS NULL",
            NULL, NULL, NULL);
        node_db_exec(ndb,
            "INSERT OR IGNORE INTO schema_migrations(version) VALUES('006')");
        int32_t v = 6;
        node_db_state_set(ndb, "schema_version", &v, sizeof(v));
        current_ver = 6;
        applied++;
    }

    if (current_ver < 7) {
        /* v7: ZSLP token transfer tracking + OP_RETURN index */
        node_db_exec(ndb,
            "CREATE TABLE IF NOT EXISTS zslp_transfers ("
            "txid BLOB NOT NULL,"
            "block_height INTEGER NOT NULL,"
            "token_id BLOB NOT NULL,"
            "tx_type INTEGER NOT NULL," /* 1=GENESIS, 2=MINT, 3=SEND */
            "from_addr BLOB,"
            "to_addr BLOB,"
            "amount INTEGER NOT NULL DEFAULT 0,"
            "vout INTEGER NOT NULL DEFAULT 0,"
            "PRIMARY KEY (txid, vout))");

        node_db_exec(ndb,
            "CREATE INDEX IF NOT EXISTS idx_zslp_xfer_token"
            " ON zslp_transfers(token_id)");

        node_db_exec(ndb,
            "CREATE INDEX IF NOT EXISTS idx_zslp_xfer_height"
            " ON zslp_transfers(block_height DESC)");

        node_db_exec(ndb,
            "CREATE INDEX IF NOT EXISTS idx_zslp_xfer_addr"
            " ON zslp_transfers(to_addr) WHERE to_addr IS NOT NULL");

        /* OP_RETURN index — stores all OP_RETURN output data for scanning */
        node_db_exec(ndb,
            "CREATE TABLE IF NOT EXISTS op_returns ("
            "txid BLOB PRIMARY KEY,"
            "block_height INTEGER NOT NULL,"
            "script BLOB NOT NULL,"
            "is_slp INTEGER NOT NULL DEFAULT 0)");

        node_db_exec(ndb,
            "CREATE INDEX IF NOT EXISTS idx_opret_height"
            " ON op_returns(block_height)");

        node_db_exec(ndb,
            "CREATE INDEX IF NOT EXISTS idx_opret_slp"
            " ON op_returns(is_slp) WHERE is_slp = 1");

        node_db_exec(ndb,
            "INSERT OR IGNORE INTO schema_migrations(version) VALUES('007')");
        int32_t v = 7;
        node_db_state_set(ndb, "schema_version", &v, sizeof(v));
        current_ver = 7;
        applied++;
    }

    if (current_ver < 8) {
        /* v8: Partial indexes on shielded value columns for fast stats queries.
         * Only index non-zero rows — most blocks have zero shielded activity. */
        node_db_exec(ndb,
            "CREATE INDEX IF NOT EXISTS idx_blocks_sprout_value "
            "ON blocks(sprout_value) WHERE sprout_value != 0");

        node_db_exec(ndb,
            "CREATE INDEX IF NOT EXISTS idx_blocks_sapling_value "
            "ON blocks(sapling_value) WHERE sapling_value != 0");

        /* Covering index for time-range queries on shielded blocks */
        node_db_exec(ndb,
            "CREATE INDEX IF NOT EXISTS idx_blocks_time_sprout "
            "ON blocks(time, sprout_value) WHERE sprout_value != 0");

        node_db_exec(ndb,
            "CREATE INDEX IF NOT EXISTS idx_blocks_time_sapling "
            "ON blocks(time, sapling_value) WHERE sapling_value != 0");

        /* Index for num_tx queries (block records) */
        node_db_exec(ndb,
            "CREATE INDEX IF NOT EXISTS idx_blocks_num_tx "
            "ON blocks(num_tx DESC)");

        node_db_exec(ndb,
            "INSERT OR IGNORE INTO schema_migrations(version) VALUES('008')");
        int32_t v = 8;
        node_db_state_set(ndb, "schema_version", &v, sizeof(v));
        current_ver = 8;
        applied++;
    }

    if (current_ver < 9) {
        /* v9: Full chain indexing — permanent tx inputs/outputs,
         * Sapling spends/outputs, Sprout JoinSplits, SHA3 integrity. */

        node_db_exec(ndb,
            "CREATE TABLE IF NOT EXISTS tx_outputs ("
            "txid BLOB NOT NULL, vout INTEGER NOT NULL,"
            "value INTEGER NOT NULL, script_type INTEGER NOT NULL DEFAULT 0,"
            "address_hash BLOB, block_height INTEGER NOT NULL,"
            "PRIMARY KEY (txid, vout))");

        node_db_exec(ndb,
            "CREATE INDEX IF NOT EXISTS idx_txo_addr"
            " ON tx_outputs(address_hash) WHERE address_hash IS NOT NULL");

        node_db_exec(ndb,
            "CREATE INDEX IF NOT EXISTS idx_txo_height"
            " ON tx_outputs(block_height)");

        node_db_exec(ndb,
            "CREATE TABLE IF NOT EXISTS tx_inputs ("
            "txid BLOB NOT NULL, vin_index INTEGER NOT NULL,"
            "prev_txid BLOB NOT NULL, prev_vout INTEGER NOT NULL,"
            "block_height INTEGER NOT NULL,"
            "PRIMARY KEY (txid, vin_index))");

        node_db_exec(ndb,
            "CREATE INDEX IF NOT EXISTS idx_txi_prev"
            " ON tx_inputs(prev_txid, prev_vout)");

        node_db_exec(ndb,
            "CREATE INDEX IF NOT EXISTS idx_txi_height"
            " ON tx_inputs(block_height)");

        node_db_exec(ndb,
            "CREATE TABLE IF NOT EXISTS sapling_spends ("
            "txid BLOB NOT NULL, spend_index INTEGER NOT NULL,"
            "cv BLOB NOT NULL, anchor BLOB NOT NULL,"
            "nullifier BLOB NOT NULL, rk BLOB NOT NULL,"
            "block_height INTEGER NOT NULL,"
            "PRIMARY KEY (txid, spend_index))");

        node_db_exec(ndb,
            "CREATE INDEX IF NOT EXISTS idx_ss_nf"
            " ON sapling_spends(nullifier)");

        node_db_exec(ndb,
            "CREATE INDEX IF NOT EXISTS idx_ss_height"
            " ON sapling_spends(block_height)");

        node_db_exec(ndb,
            "CREATE TABLE IF NOT EXISTS sapling_outputs ("
            "txid BLOB NOT NULL, output_index INTEGER NOT NULL,"
            "cv BLOB NOT NULL, cm BLOB NOT NULL,"
            "ephemeral_key BLOB NOT NULL, block_height INTEGER NOT NULL,"
            "PRIMARY KEY (txid, output_index))");

        node_db_exec(ndb,
            "CREATE INDEX IF NOT EXISTS idx_so_height"
            " ON sapling_outputs(block_height)");

        node_db_exec(ndb,
            "CREATE TABLE IF NOT EXISTS joinsplits ("
            "txid BLOB NOT NULL, js_index INTEGER NOT NULL,"
            "vpub_old INTEGER NOT NULL, vpub_new INTEGER NOT NULL,"
            "anchor BLOB NOT NULL, block_height INTEGER NOT NULL,"
            "PRIMARY KEY (txid, js_index))");

        node_db_exec(ndb,
            "CREATE INDEX IF NOT EXISTS idx_js_height"
            " ON joinsplits(block_height)");

        node_db_exec(ndb,
            "CREATE TABLE IF NOT EXISTS sprout_nullifiers ("
            "nullifier BLOB PRIMARY KEY,"
            "txid BLOB NOT NULL, block_height INTEGER NOT NULL)");

        node_db_exec(ndb,
            "CREATE INDEX IF NOT EXISTS idx_spnf_height"
            " ON sprout_nullifiers(block_height)");

        node_db_exec(ndb,
            "CREATE TABLE IF NOT EXISTS view_integrity ("
            "height INTEGER PRIMARY KEY,"
            "sha3_hash BLOB NOT NULL)");

        node_db_exec(ndb,
            "INSERT OR IGNORE INTO schema_migrations(version) VALUES('009')");
        int32_t v = 9;
        node_db_state_set(ndb, "schema_version", &v, sizeof(v));
        current_ver = 9;
        applied++;
    }

    if (current_ver < 10) {
        /* v10: Add n_chain_tx to block_index_cache for full restart from SQLite.
         * Needed so difficulty validation (17-ancestor walk) works without LevelDB. */
        sqlite3_exec(ndb->db,
            "ALTER TABLE block_index_cache ADD COLUMN n_chain_tx INTEGER NOT NULL DEFAULT 0",
            NULL, NULL, NULL);
        node_db_exec(ndb,
            "INSERT OR IGNORE INTO schema_migrations(version) VALUES('010')");
        int32_t v = 10;
        node_db_state_set(ndb, "schema_version", &v, sizeof(v));
        current_ver = 10;
        applied++;
    }

    if (current_ver < 11) {
        /* v11: Peer bandwidth scores + ZCL23 flag for fast reconnection.
         * New nodes should reconnect to fast ZCL23 peers first, enabling
         * instant swarm sync on subsequent starts. */
        sqlite3_exec(ndb->db,
            "ALTER TABLE peers ADD COLUMN bandwidth_score INTEGER NOT NULL DEFAULT 0",
            NULL, NULL, NULL);
        sqlite3_exec(ndb->db,
            "ALTER TABLE peers ADD COLUMN is_zcl23 INTEGER NOT NULL DEFAULT 0",
            NULL, NULL, NULL);
        /* Index: prioritize fast ZCL23 peers for reconnection */
        node_db_exec(ndb,
            "CREATE INDEX IF NOT EXISTS idx_peers_zcl23_score "
            "ON peers(is_zcl23 DESC, bandwidth_score DESC)");
        node_db_exec(ndb,
            "INSERT OR IGNORE INTO schema_migrations(version) VALUES('011')");
        int32_t v = 11;
        node_db_state_set(ndb, "schema_version", &v, sizeof(v));
        current_ver = 11;
        applied++;
    }

    if (current_ver < 12) {
        /* v12: ZSLP address balances as a first-class model-backed table. */
        node_db_exec(ndb,
            "CREATE TABLE IF NOT EXISTS zslp_balances ("
            "token_id TEXT NOT NULL,"
            "address TEXT NOT NULL,"
            "balance INTEGER NOT NULL DEFAULT 0,"
            "PRIMARY KEY (token_id, address))");

        node_db_exec(ndb,
            "CREATE INDEX IF NOT EXISTS idx_zslp_balance_token "
            "ON zslp_balances(token_id)");

        node_db_exec(ndb,
            "CREATE INDEX IF NOT EXISTS idx_zslp_balance_address "
            "ON zslp_balances(address)");

        node_db_exec(ndb,
            "INSERT OR IGNORE INTO schema_migrations(version) VALUES('012')");
        int32_t v = 12;
        node_db_state_set(ndb, "schema_version", &v, sizeof(v));
        current_ver = 12;
        applied++;
    }

    if (current_ver < 13) {
        /* v13: App-facing lightweight models for wallet contacts and
         * onion announcement registry. */
        node_db_exec(ndb,
            "CREATE TABLE IF NOT EXISTS contacts ("
            "address TEXT PRIMARY KEY,"
            "name TEXT NOT NULL,"
            "last_used INTEGER NOT NULL DEFAULT 0)");

        node_db_exec(ndb,
            "CREATE INDEX IF NOT EXISTS idx_contacts_last_used "
            "ON contacts(last_used DESC)");

        node_db_exec(ndb,
            "CREATE TABLE IF NOT EXISTS onion_announcements ("
            "onion_address TEXT PRIMARY KEY,"
            "announced_at INTEGER NOT NULL,"
            "script_hex TEXT NOT NULL DEFAULT '')");

        node_db_exec(ndb,
            "CREATE INDEX IF NOT EXISTS idx_onion_announced_at "
            "ON onion_announcements(announced_at DESC)");

        node_db_exec(ndb,
            "INSERT OR IGNORE INTO schema_migrations(version) VALUES('013')");
        int32_t v = 13;
        node_db_state_set(ndb, "schema_version", &v, sizeof(v));
        current_ver = 13;
        applied++;
    }

    if (current_ver < 14) {
        /* v14: Store product/order tables as model-owned app schema. */
        node_db_exec(ndb,
            "CREATE TABLE IF NOT EXISTS products ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "name TEXT NOT NULL,"
            "description TEXT,"
            "price_zatoshi INTEGER NOT NULL,"
            "token_id TEXT,"
            "tokens_per_purchase INTEGER NOT NULL DEFAULT 1,"
            "active INTEGER NOT NULL DEFAULT 1)");

        node_db_exec(ndb,
            "CREATE TABLE IF NOT EXISTS orders ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "product_id INTEGER NOT NULL,"
            "customer_addr TEXT,"
            "payment_addr TEXT NOT NULL,"
            "amount_zatoshi INTEGER NOT NULL,"
            "payment_txid TEXT,"
            "mint_txid TEXT,"
            "status INTEGER NOT NULL DEFAULT 0,"
            "created_at INTEGER NOT NULL,"
            "paid_at INTEGER)");

        node_db_exec(ndb,
            "CREATE INDEX IF NOT EXISTS idx_orders_status_created "
            "ON orders(status, created_at DESC)");

        node_db_exec(ndb,
            "INSERT OR IGNORE INTO schema_migrations(version) VALUES('014')");
        int32_t v = 14;
        node_db_state_set(ndb, "schema_version", &v, sizeof(v));
        current_ver = 14;
        applied++;
    }

    if (applied > 0)
        printf("db: applied %d migration(s), now at version %d\n",
               applied, node_db_schema_version(ndb));

    return applied;
}

/* ── UTXO Lifecycle ────────────────────────────────────────────── */

bool node_db_wipe_utxos(struct node_db *ndb)
{
    if (!ndb || !ndb->open) return false;
    bool ok = true;
    ok &= node_db_exec(ndb, "DELETE FROM utxos");
    ok &= node_db_exec(ndb, "DELETE FROM node_state WHERE key='coins_best_block'");
    ok &= node_db_exec(ndb, "DELETE FROM node_state WHERE key='utxo_commitment'");
    if (ok)
        printf("db: wiped UTXO set + coins state\n");
    return ok;
}

int64_t node_db_utxo_count(struct node_db *ndb)
{
    if (!ndb || !ndb->open) return 0;
    sqlite3_stmt *stmt = NULL;
    int64_t count = 0;
    if (sqlite3_prepare_v2(ndb->db, "SELECT count(*) FROM utxos",
                           -1, &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW)
            count = sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);
    }
    return count;
}

/* ── Performance Modes ─────────────────────────────────────────── */

/* Secondary indexes dropped during IBD for throughput, rebuilt after. */
static const char *const DB_DROP_INDEXES[] = {
    "DROP INDEX IF EXISTS idx_utxo_address",
    "DROP INDEX IF EXISTS idx_utxo_value",
    "DROP INDEX IF EXISTS idx_utxo_height",
    "DROP INDEX IF EXISTS idx_utxo_height_value",
    "DROP INDEX IF EXISTS idx_tx_block",
    "DROP INDEX IF EXISTS idx_tx_height",
};
static const char *const DB_CREATE_INDEXES[] = {
    "CREATE INDEX IF NOT EXISTS idx_utxo_address"
        " ON utxos(address_hash) WHERE address_hash IS NOT NULL",
    "CREATE INDEX IF NOT EXISTS idx_utxo_value"
        " ON utxos(value DESC)",
    "CREATE INDEX IF NOT EXISTS idx_utxo_height"
        " ON utxos(height)",
    "CREATE INDEX IF NOT EXISTS idx_utxo_height_value"
        " ON utxos(height, value)",
    "CREATE INDEX IF NOT EXISTS idx_tx_block"
        " ON transactions(block_hash)",
    "CREATE INDEX IF NOT EXISTS idx_tx_height"
        " ON transactions(block_height)",
};
#define NUM_DB_INDEXES (sizeof(DB_DROP_INDEXES) / sizeof(DB_DROP_INDEXES[0]))

bool node_db_drop_indexes(struct node_db *ndb)
{
    if (!ndb || !ndb->open) return false;
    for (size_t i = 0; i < NUM_DB_INDEXES; i++)
        sqlite3_exec(ndb->db, DB_DROP_INDEXES[i], NULL, NULL, NULL);
    return true;
}

bool node_db_rebuild_indexes(struct node_db *ndb)
{
    if (!ndb || !ndb->open) return false;
    for (size_t i = 0; i < NUM_DB_INDEXES; i++)
        sqlite3_exec(ndb->db, DB_CREATE_INDEXES[i], NULL, NULL, NULL);
    return true;
}

bool node_db_ibd_turbo_mode(struct node_db *ndb)
{
    if (!ndb || !ndb->open) return false;
    sqlite3_exec(ndb->db, "PRAGMA synchronous=OFF", NULL, NULL, NULL);
    sqlite3_exec(ndb->db, "PRAGMA cache_size=-524288", NULL, NULL, NULL);
    sqlite3_exec(ndb->db, "PRAGMA wal_autocheckpoint=0", NULL, NULL, NULL);
    sqlite3_busy_timeout(ndb->db, 10000);
    node_db_drop_indexes(ndb);
    printf("db: IBD turbo mode (synchronous=OFF, indexes dropped)\n");
    return true;
}

bool node_db_normal_mode(struct node_db *ndb)
{
    if (!ndb || !ndb->open) return false;
    sqlite3_exec(ndb->db, "PRAGMA synchronous=NORMAL", NULL, NULL, NULL);
    sqlite3_exec(ndb->db, "PRAGMA cache_size=-65536", NULL, NULL, NULL);
    sqlite3_exec(ndb->db, "PRAGMA wal_autocheckpoint=1000", NULL, NULL, NULL);
    node_db_rebuild_indexes(ndb);
    node_db_wal_checkpoint(ndb);
    printf("db: normal mode (synchronous=NORMAL, indexes rebuilt)\n");
    return true;
}

bool node_db_wal_checkpoint(struct node_db *ndb)
{
    if (!ndb || !ndb->open) return false;
    sqlite3_wal_checkpoint_v2(ndb->db, NULL, SQLITE_CHECKPOINT_TRUNCATE,
                              NULL, NULL);
    return true;
}
