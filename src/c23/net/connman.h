/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef ZCL_NET_CONNMAN_H
#define ZCL_NET_CONNMAN_H

#include "net/net.h"
#include "chain/chainparams.h"
#include <stdbool.h>

struct connman {
    struct net_manager manager;
    const struct chain_params *params;
    bool started;
};

bool connman_init(struct connman *cm, const struct chain_params *params,
                   struct node_signals *signals);
bool connman_start(struct connman *cm);
void connman_stop(struct connman *cm);
void connman_free(struct connman *cm);

void connman_add_seed_node(struct connman *cm, const char *host,
                            uint16_t port);
void connman_open_connection(struct connman *cm,
                              const struct net_address *addr);

size_t connman_get_node_count(const struct connman *cm);

void connman_relay_transaction(struct connman *cm,
                                const struct uint256 *txid);

#endif
