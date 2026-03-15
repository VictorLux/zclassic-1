/* Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef ZCL_RPC_BLOCKCHAIN_H
#define ZCL_RPC_BLOCKCHAIN_H

#include "rpc/server.h"
#include "validation/main_state.h"
#include "validation/txmempool.h"

struct coins_view_db;
struct coins_view_cache;
void rpc_blockchain_set_state(struct main_state *ms, struct tx_mempool *mp,
                               const char *datadir);
void rpc_blockchain_set_coins_db(struct coins_view_db *cvdb,
                                  struct coins_view_cache *coins_tip);
void register_blockchain_rpc_commands(struct rpc_table *t);

#endif
