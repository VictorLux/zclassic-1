/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * SQLite-backed wallet storage. Replaces wallet_db (LevelDB) for runtime.
 * Uses node.db's wallet_keys, wallet_sapling_keys, wallet_scripts,
 * wallet_seed, and wallet_transactions tables. */

#ifndef ZCL_WALLET_SQLITE_H
#define ZCL_WALLET_SQLITE_H

#include "wallet/wallet.h"
#include "wallet/sapling_keys.h"
#include "script/script.h"
#include "core/uint256.h"
#include <sqlite3.h>
#include <stdbool.h>
#include <stdint.h>

struct wallet_sqlite {
    sqlite3 *db;  /* borrowed handle to node.db */
    bool open;

    /* Prepared statements */
    sqlite3_stmt *stmt_key_write;
    sqlite3_stmt *stmt_key_read;
    sqlite3_stmt *stmt_tx_write;
    sqlite3_stmt *stmt_tx_read;
    sqlite3_stmt *stmt_seed_write;
    sqlite3_stmt *stmt_seed_read;
    sqlite3_stmt *stmt_zkey_write;
    sqlite3_stmt *stmt_zkey_read;
    sqlite3_stmt *stmt_script_write;
    sqlite3_stmt *stmt_script_read;
};

bool wallet_sqlite_open(struct wallet_sqlite *ws, sqlite3 *db);
void wallet_sqlite_close(struct wallet_sqlite *ws);

bool wallet_sqlite_write_key(struct wallet_sqlite *ws, const struct pubkey *pk,
                              const struct privkey *key);
bool wallet_sqlite_read_keys(struct wallet_sqlite *ws, struct wallet *w);

bool wallet_sqlite_write_tx(struct wallet_sqlite *ws,
                              const struct wallet_tx *wtx);
bool wallet_sqlite_read_txs(struct wallet_sqlite *ws, struct wallet *w);

bool wallet_sqlite_write_best_block(struct wallet_sqlite *ws,
                                      const struct uint256 *hash);
bool wallet_sqlite_read_best_block(struct wallet_sqlite *ws,
                                     struct uint256 *hash);

bool wallet_sqlite_write_scan_height(struct wallet_sqlite *ws, int height);
bool wallet_sqlite_read_scan_height(struct wallet_sqlite *ws, int *height);

bool wallet_sqlite_write_sapling_seed(struct wallet_sqlite *ws,
                                        const uint8_t seed[32]);
bool wallet_sqlite_read_sapling_seed(struct wallet_sqlite *ws,
                                       uint8_t seed[32]);
bool wallet_sqlite_write_sapling_key(struct wallet_sqlite *ws,
                                       uint32_t child_index,
                                       const struct sapling_key_entry *entry);
bool wallet_sqlite_read_sapling_keys(struct wallet_sqlite *ws,
                                       struct wallet *w);

bool wallet_sqlite_write_script(struct wallet_sqlite *ws,
                                  const struct uint160 *script_id,
                                  const struct script *redeem_script);
bool wallet_sqlite_read_scripts(struct wallet_sqlite *ws, struct wallet *w);

bool wallet_sqlite_flush(struct wallet_sqlite *ws, struct wallet *w);

#endif
