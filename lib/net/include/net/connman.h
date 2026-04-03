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

#define MAX_ADDNODES 16

struct connman {
    struct net_manager manager;
    const struct chain_params *params;
    bool started;
    bool dns_seed_thread_started;
    bool socket_thread_started;
    bool open_thread_started;
    bool message_thread_started;
    struct p2p_node *deferred_free[64];
    size_t num_deferred_free;
    /* Persistent addnode list — reconnected automatically on disconnect */
    struct net_address addnodes[MAX_ADDNODES];
    int num_addnodes;
    /* Data directory for persisting addrman (peers.dat) */
    const char *datadir;
};

bool connman_init(struct connman *cm, const struct chain_params *params,
                   struct node_signals *signals);
bool connman_start(struct connman *cm);
void connman_signal_stop(struct connman *cm);
void connman_join(struct connman *cm);
void connman_stop(struct connman *cm);
void connman_free(struct connman *cm);

/* Persist addrman to {datadir}/peers.dat. Call on shutdown. */
void connman_save_addrman(struct connman *cm);

/* Load addrman from {datadir}/peers.dat. Call before connman_start. */
void connman_load_addrman(struct connman *cm);

void connman_add_seed_node(struct connman *cm, const char *host,
                            uint16_t port);
void connman_open_connection(struct connman *cm,
                              const struct net_address *addr);

size_t connman_get_node_count(const struct connman *cm);

void connman_relay_transaction(struct connman *cm,
                                const struct uint256 *txid);

#endif
