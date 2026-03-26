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
struct node_db;
void rpc_blockchain_set_state(struct main_state *ms, struct tx_mempool *mp,
                               const char *datadir);
void rpc_blockchain_set_coins_db(struct coins_view_db *cvdb,
                                  struct coins_view_cache *coins_tip);
void rpc_blockchain_set_node_db(struct node_db *ndb);
void register_blockchain_rpc_commands(struct rpc_table *t);

/* MMR (Merkle Mountain Range) — append block hashes for power node sync */
struct mmr;
void rpc_blockchain_mmr_append(const uint8_t block_hash[32]);
void rpc_blockchain_mmr_init_from_state(struct node_db *ndb);
void rpc_blockchain_mmr_catchup(struct main_state *ms);
void rpc_blockchain_mmr_save(struct node_db *ndb);
struct mmr *rpc_blockchain_get_mmr(void);

#endif
