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
    sqlite3 *db;                     /* coins handle — dedicated for file DBs (P14.1), shared for :memory: */
    bool owns_db;                    /* true when db was opened here (file DB); false when shared */
    pthread_mutex_t mutex;           /* serialize all statement access */

    /* Prepared statements (all on cvs->db) */
    sqlite3_stmt *stmt_get;          /* all vouts for a txid */
    sqlite3_stmt *stmt_have;         /* existence check */
    sqlite3_stmt *stmt_insert;       /* upsert single UTXO */
    sqlite3_stmt *stmt_delete_tx;    /* delete all vouts for txid */
    sqlite3_stmt *stmt_best_get;     /* read best block hash */
    sqlite3_stmt *stmt_best_set;     /* write best block hash */
    sqlite3_stmt *stmt_commit_get;   /* read UTXO commitment */
    sqlite3_stmt *stmt_commit_set;   /* write UTXO commitment */
};

/* Open coins view. If `db` is file-backed (`sqlite3_db_filename` returns
 * a non-empty path), opens a dedicated sqlite3 handle on that same file
 * so the flush's BEGIN IMMEDIATE runs on an independent `nVdbeWrite`
 * counter — avoids the live-node stall where SAVEPOINT on a shared
 * handle failed with "SQL statements in progress" whenever any other
 * subsystem had a writer VDBE mid-execution (P14.1, 2026-04-19).
 * `:memory:` handles fall back to the shared connection with SAVEPOINT
 * nesting (used by a handful of unit tests that pass a throwaway DB). */
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
