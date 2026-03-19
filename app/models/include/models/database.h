/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef ZCL_DB_H
#define ZCL_DB_H

#include <sqlite3.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct node_db {
    sqlite3 *db;
    bool open;

    /* Prepared statements cached for hot paths */
    sqlite3_stmt *stmt_utxo_insert;
    sqlite3_stmt *stmt_utxo_delete;
    sqlite3_stmt *stmt_utxo_find;
    sqlite3_stmt *stmt_block_insert;
    sqlite3_stmt *stmt_block_by_hash;
    sqlite3_stmt *stmt_block_by_height;
    sqlite3_stmt *stmt_tx_insert;
    sqlite3_stmt *stmt_tx_find;
    sqlite3_stmt *stmt_wallet_utxo_insert;
    sqlite3_stmt *stmt_wallet_utxo_spend;
    sqlite3_stmt *stmt_wallet_balance;
    sqlite3_stmt *stmt_nullifier_insert;
    sqlite3_stmt *stmt_nullifier_exists;
    sqlite3_stmt *stmt_state_set;
    sqlite3_stmt *stmt_state_get;

    /* Batch sync: accumulate N blocks before COMMIT for throughput.
     * batch_size=1 is the safe default (per-block COMMIT).
     * During IBD, set batch_size=100+ for 10-50x SQLite throughput. */
    int  sync_batch_size;       /* target blocks per COMMIT (default 1) */
    int  sync_pending_blocks;   /* blocks since last COMMIT */
    bool sync_in_batch;         /* true if a batch transaction is open */
};

/* Open or create the node database at path (e.g. ~/.zclassic-c23/node.db).
 * Creates all tables and indexes if they don't exist. */
bool node_db_open(struct node_db *ndb, const char *path);
void node_db_close(struct node_db *ndb);

/* Execute raw SQL (for migrations, debugging). */
bool node_db_exec(struct node_db *ndb, const char *sql);

/* Transaction control for batch operations. */
bool node_db_begin(struct node_db *ndb);
bool node_db_commit(struct node_db *ndb);
bool node_db_rollback(struct node_db *ndb);

/* Set SQLite sync batch size (blocks per COMMIT).
 * 1 = safe default (per-block). 100+ = aggressive IBD mode. */
void node_db_set_sync_batch_size(struct node_db *ndb, int batch_size);

/* Flush any pending batch transaction (call on shutdown or before reorg). */
bool node_db_sync_flush(struct node_db *ndb);

/* Key-value state store (replaces misc flags). */
bool node_db_state_set(struct node_db *ndb, const char *key,
                       const void *value, size_t len);
bool node_db_state_get(struct node_db *ndb, const char *key,
                       void *value, size_t max_len, size_t *out_len);
bool node_db_state_set_int(struct node_db *ndb, const char *key, int64_t val);
bool node_db_state_get_int(struct node_db *ndb, const char *key, int64_t *val);

/* Schema version for future migrations. */
int node_db_schema_version(struct node_db *ndb);

/* Rails-style migration runner.
 * Runs all pending migrations from db/migrate/ directory.
 * Tracks applied migrations in schema_migrations table.
 * Returns number of migrations applied, or -1 on error. */
int node_db_migrate(struct node_db *ndb, const char *datadir);

#endif
