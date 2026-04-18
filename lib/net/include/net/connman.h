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

/* Capacity of the deferred-free list: max # of nodes that can be awaiting
 * free in one socket cycle. Must exceed DEFAULT_MAX_PEER_CONNECTIONS (125)
 * with headroom, since the P2.5 fix can also re-defer a node whose ref_count
 * is still non-zero — that means the list can accumulate across cycles when
 * the message handler holds references. 256 leaves ~125 slots of headroom. */
#define CONNMAN_DEFERRED_FREE_CAP 256

struct connman {
    struct net_manager manager;
    const struct chain_params *params;
    bool started;
    bool dns_seed_thread_started;
    bool socket_thread_started;
    bool open_thread_started;
    bool message_thread_started;
    struct p2p_node *deferred_free[CONNMAN_DEFERRED_FREE_CAP];
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

/* Return the highest starting_height among all connected peers, or -1. */
int connman_max_peer_height(struct connman *cm);

void connman_relay_transaction(struct connman *cm,
                                const struct uint256 *txid);

/* Access the connman's per-peer bandwidth state (wave 10 #3).
 * Returns NULL if bandwidth quotas are not active. */
struct peer_bandwidth;
struct peer_bandwidth *connman_peer_bandwidth(void);

/* P2.5: one pass of the message-handler loop body.
 *
 * Snapshots cm->manager.nodes[] under cs_nodes + bumps ref_count on each
 * non-disconnected entry, releases cs_nodes, calls the process_messages
 * and send_messages signals against the local copy, then re-acquires
 * cs_nodes to decrement refs. Returns true if any peer saw work.
 *
 * Exposed outside the message thread so the P2.5 stress test can drive
 * the cycle directly without needing to stand up a full connman_start(). */
bool connman_run_message_cycle(struct connman *cm);

/* P2.5: one pass of the socket-handler deferred-free sweep.
 *
 * Walks cm->deferred_free[], freeing entries whose ref_count has reached
 * zero and re-parking any that are still held by an in-flight snapshot.
 * Caller must hold cm->manager.cs_nodes. Exposed for the stress test. */
void connman_run_deferred_free_sweep(struct connman *cm);

#endif
