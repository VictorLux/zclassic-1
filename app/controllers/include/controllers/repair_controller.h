/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#ifndef ZCL_RPC_REPAIR_H
#define ZCL_RPC_REPAIR_H

#include "rpc/server.h"

struct main_state;
struct coins_view_cache;
struct node_db;
struct chain_params;

void rpc_repair_set_state(struct main_state *ms,
                           struct coins_view_cache *coins_tip,
                           struct node_db *ndb,
                           const char *datadir,
                           const struct chain_params *params);
void register_repair_rpc_commands(struct rpc_table *t);

#endif
