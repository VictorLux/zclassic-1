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
    "value INTEGER NOT NULL,script BLOB NOT NULL,"
    "script_type INTEGER NOT NULL DEFAULT 0,"
    "address_hash BLOB,height INTEGER NOT NULL,"
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
    "value INTEGER NOT NULL,address_hash BLOB NOT NULL,"
    "script BLOB NOT NULL,height INTEGER NOT NULL,"
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
    "PRIMARY KEY (txid,output_index))",

    "CREATE INDEX IF NOT EXISTS idx_snote_unspent"
    " ON wallet_sapling_notes(ivk) WHERE spent_txid IS NULL",

    "CREATE INDEX IF NOT EXISTS idx_snote_nullifier"
    " ON wallet_sapling_notes(nullifier)",

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
    "UNIQUE(ip,port))",

    "CREATE INDEX IF NOT EXISTS idx_peers_seen"
    " ON peers(last_seen DESC)",

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
         "INSERT OR IGNORE INTO wallet_utxos"
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

    if (applied > 0)
        printf("db: applied %d migration(s), now at version %d\n",
               applied, node_db_schema_version(ndb));

    return applied;
}
