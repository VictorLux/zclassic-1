/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#ifndef ZCL_COINS_VIEW_SQLITE_H
#define ZCL_COINS_VIEW_SQLITE_H

#include "coins/coins_view.h"
#include "coins/utxo_commitment.h"
#include <sqlite3.h>
#include <stdbool.h>
#include <pthread.h>

struct coins_view_sqlite {
    struct coins_view view;          /* vtable-based polymorphism */
    sqlite3 *db;                     /* shared handle for coins (reads+writes) */
    bool owns_db;                    /* true = we opened db, must close it */
    pthread_mutex_t mutex;           /* serialize all statement access */

    /* Prepared statements (all on shared db handle) */
    sqlite3_stmt *stmt_get;          /* all vouts for a txid */
    sqlite3_stmt *stmt_have;         /* existence check */
    sqlite3_stmt *stmt_insert;       /* upsert single UTXO */
    sqlite3_stmt *stmt_delete_tx;    /* delete all vouts for txid */
    sqlite3_stmt *stmt_best_get;     /* read best block hash */
    sqlite3_stmt *stmt_best_set;     /* write best block hash */
    sqlite3_stmt *stmt_commit_get;   /* read UTXO commitment */
    sqlite3_stmt *stmt_commit_set;   /* write UTXO commitment */
};

/* Open coins view. Opens a dedicated SQLite connection to the same
 * database file as `db`. This avoids SAVEPOINT/transaction conflicts
 * with node_db which runs BEGIN TRANSACTION on the shared handle. */
bool coins_view_sqlite_open(struct coins_view_sqlite *cvs, sqlite3 *db);
void coins_view_sqlite_close(struct coins_view_sqlite *cvs);

/* coins_view vtable implementations */
bool coins_view_sqlite_get_coins(struct coins_view_sqlite *cvs,
                                  const struct uint256 *txid,
                                  struct coins *out);
bool coins_view_sqlite_have_coins(struct coins_view_sqlite *cvs,
                                   const struct uint256 *txid);
bool coins_view_sqlite_get_best_block(struct coins_view_sqlite *cvs,
                                       struct uint256 *hash);
bool coins_view_sqlite_batch_write(struct coins_view_sqlite *cvs,
                                    struct coins_map *map_coins,
                                    const struct uint256 *hash_block);
/* Extended version: writes UTXO commitment atomically inside the
 * same SAVEPOINT transaction as the coins flush. Pass NULL to skip. */
bool coins_view_sqlite_batch_write_ex(struct coins_view_sqlite *cvs,
                                       struct coins_map *map_coins,
                                       const struct uint256 *hash_block,
                                       const struct utxo_commitment *commit);

/* UTXO commitment persistence */
bool coins_view_sqlite_write_commitment(struct coins_view_sqlite *cvs,
                                         const struct utxo_commitment *uc);
bool coins_view_sqlite_read_commitment(struct coins_view_sqlite *cvs,
                                        struct utxo_commitment *uc);

#endif
