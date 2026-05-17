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

/* Initial capacity of the deferred-free list: starts at 256, grows
 * dynamically on overflow up to CONNMAN_DEFERRED_FREE_HARD_CAP. The fixed
 * cap-256 used to overflow under Tor-driven churn while the message
 * handler held snapshot refs, triggering the deliberate-leak fallback in
 * thread_socket_handler. grow the array instead. The hard
 * ceiling is 8× DEFAULT_MAX_PEER_CONNECTIONS = 1000, large enough that
 * hitting it indicates a genuine leak (and tripping the SIGABRT handler
 * is the right outcome). */
#define CONNMAN_DEFERRED_FREE_INIT_CAP 256
#define CONNMAN_DEFERRED_FREE_HARD_CAP 1000

enum connman_outbound_target_source {
    CONNMAN_TARGET_NONE = 0,
    CONNMAN_TARGET_ADDNODE,
    CONNMAN_TARGET_ADDRMAN,
};

struct connman {
    struct net_manager manager;
    const struct chain_params *params;
    bool started;
    bool dns_seed_thread_started;
    bool socket_thread_started;
    bool open_thread_started;
    bool message_thread_started;
    struct p2p_node **deferred_free;
    size_t num_deferred_free;
    size_t deferred_free_cap;
    /* Persistent addnode list — reconnected automatically on disconnect */
    struct net_address addnodes[MAX_ADDNODES];
    int num_addnodes;
    size_t next_addnode_cursor;
    int64_t addnode_last_attempt[MAX_ADDNODES];
    int addnode_backoff_sec[MAX_ADDNODES];
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

/* Count of outbound peers in PEER_HANDSHAKE_COMPLETE or later. Used by
 * the sync watchdog to distinguish slot-burning peers stuck in
 * PEER_CONNECTING from peers actually able to serve us blocks. */
size_t connman_outbound_healthy_count(struct connman *cm);

/* Return the highest starting_height among all connected peers, or -1. */
int connman_max_peer_height(struct connman *cm);

void connman_relay_transaction(struct connman *cm,
                                const struct uint256 *txid);

/* Access the connman's per-peer bandwidth state.
 * Returns NULL if bandwidth quotas are not active. */
struct peer_bandwidth;
struct peer_bandwidth *connman_peer_bandwidth(void);

/* one pass of the message-handler loop body.
 *
 * Snapshots cm->manager.nodes[] under cs_nodes + bumps ref_count on each
 * non-disconnected entry, releases cs_nodes, calls the process_messages
 * and send_messages signals against the local copy, then re-acquires
 * cs_nodes to decrement refs. Returns true if any peer saw work.
 *
 * Exposed outside the message thread so the stress test can drive
 * the cycle directly without needing to stand up a full connman_start(). */
bool connman_run_message_cycle(struct connman *cm);

/* one pass of the socket-handler deferred-free sweep.
 *
 * Walks cm->deferred_free[], freeing entries whose ref_count has reached
 * zero and re-parking any that are still held by an in-flight snapshot.
 * Caller must hold cm->manager.cs_nodes. Exposed for the stress test. */
void connman_run_deferred_free_sweep(struct connman *cm);

bool connman_pick_next_outbound_target(
    struct connman *cm,
    size_t *addnode_cursor,
    struct addr_info *result,
    enum connman_outbound_target_source *source,
    size_t *addnode_index);

void connman_record_addnode_attempt(struct connman *cm,
                                    size_t addnode_index,
                                    bool success);

#endif
