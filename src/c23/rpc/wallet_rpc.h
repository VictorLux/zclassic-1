/* Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef ZCL_RPC_WALLET_RPC_H
#define ZCL_RPC_WALLET_RPC_H

#include "rpc/server.h"

struct wallet;
struct main_state;
struct wallet_db;
struct tx_mempool;
struct connman;

void rpc_wallet_set_state(struct wallet *w, struct main_state *ms,
                          const char *datadir, struct wallet_db *wdb,
                          struct tx_mempool *mempool,
                          struct connman *connman);
void register_wallet_rpc_commands(struct rpc_table *t);

#endif
