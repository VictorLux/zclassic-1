/* Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef ZCL_RPC_WALLET_RPC_H
#define ZCL_RPC_WALLET_RPC_H

#include "rpc/server.h"

struct wallet;

void rpc_wallet_set_state(struct wallet *w);
void register_wallet_rpc_commands(struct rpc_table *t);

#endif
