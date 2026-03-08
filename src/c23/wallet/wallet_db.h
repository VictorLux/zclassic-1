/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef ZCL_WALLET_DB_H
#define ZCL_WALLET_DB_H

#include "storage/dbwrapper.h"
#include "wallet/wallet.h"
#include <stdbool.h>

struct wallet_db {
    struct db_wrapper db;
    bool open;
};

bool wallet_db_open(struct wallet_db *wdb, const char *path);
void wallet_db_close(struct wallet_db *wdb);

bool wallet_db_write_key(struct wallet_db *wdb, const struct pubkey *pk,
                          const struct privkey *key);
bool wallet_db_read_keys(struct wallet_db *wdb, struct wallet *w);

bool wallet_db_write_tx(struct wallet_db *wdb, const struct wallet_tx *wtx);
bool wallet_db_read_txs(struct wallet_db *wdb, struct wallet *w);

bool wallet_db_write_best_block(struct wallet_db *wdb,
                                  const struct uint256 *hash);
bool wallet_db_read_best_block(struct wallet_db *wdb, struct uint256 *hash);

bool wallet_db_flush(struct wallet_db *wdb, struct wallet *w);

#endif
