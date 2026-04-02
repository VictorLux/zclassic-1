/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef ZCL_NODE_HEALTH_SERVICE_H
#define ZCL_NODE_HEALTH_SERVICE_H

#include "event/event.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct node_db;
struct main_state;

struct node_health_snapshot {
    enum sync_state sync_state;
    bool healthy;
    bool synced;
    bool has_peers;
    bool tor_ready;
    bool onion_service_ready;
    bool tip_stale;
    bool queue_backed_up;
    size_t peer_count;
    int tip_height;
    int header_height;
    int peer_best_height;
    int tip_lag;
    int64_t tip_stale_seconds;
    int64_t utxo_count;
    int64_t wal_size_bytes;
    int64_t uptime_seconds;
    int error_total;
    char last_error[EVENT_PAYLOAD_SIZE + 1];
    char degraded_reason[128];
    char onion_address[128];
    uint64_t blocks_requested;
    uint64_t blocks_received;
    uint64_t blocks_timed_out;
    uint64_t in_flight;
    uint64_t queued;
};

void node_health_collect(struct node_health_snapshot *snapshot,
                         struct node_db *ndb,
                         const struct main_state *ms);

#endif
