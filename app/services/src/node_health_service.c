/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "services/node_health_service.h"
#include "controllers/network_controller.h"
#include "models/block.h"
#include "models/database.h"
#include "net/onion_service.h"
#include "validation/main_state.h"
#include "net/connman.h"
#include "net/download.h"
#include "net/tor_integration.h"
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

static int64_t g_health_start_time = 0;

void node_health_collect(struct node_health_snapshot *snapshot,
                         struct node_db *ndb,
                         const struct main_state *ms)
{
    struct node_health_snapshot empty = {0};
    if (!snapshot) return;
    *snapshot = empty;

    snapshot->sync_state = sync_get_state();
    snapshot->synced = (snapshot->sync_state == SYNC_AT_TIP);
    snapshot->tip_height = -1;
    snapshot->header_height = -1;
    snapshot->peer_best_height = -1;
    snapshot->tor_ready = tor_integration_is_ready();
    snapshot->onion_service_ready = false;

    {
        const char *onion = onion_service_get_address();
        if (onion && onion[0]) {
            snprintf(snapshot->onion_address, sizeof(snapshot->onion_address),
                     "%s", onion);
            snapshot->onion_service_ready = true;
        }
    }

    struct connman *cm = rpc_net_get_connman();
    snapshot->peer_count = cm ? connman_get_node_count(cm) : 0;
    snapshot->has_peers = snapshot->peer_count > 0;

    if (ndb && ndb->open) {
        snapshot->tip_height = db_block_max_height(ndb);
        snapshot->utxo_count = node_db_utxo_count(ndb);

        const char *db_path = sqlite3_db_filename(ndb->db, "main");
        if (db_path) {
            char wal_path[1024];
            struct stat wal_st;
            snprintf(wal_path, sizeof(wal_path), "%s-wal", db_path);
            if (stat(wal_path, &wal_st) == 0)
                snapshot->wal_size_bytes = wal_st.st_size;
        }
    }

    if (ms && ms->pindex_best_header)
        snapshot->header_height = ms->pindex_best_header->nHeight;
    if (snapshot->header_height < 0)
        snapshot->header_height = snapshot->tip_height;

    if (cm) {
        int64_t newest_peer_block_time = 0;
        zcl_mutex_lock(&cm->manager.cs_nodes);
        for (size_t i = 0; i < cm->manager.num_nodes; i++) {
            const struct p2p_node *node = cm->manager.nodes[i];
            if (!node) continue;
            if (node->starting_height > snapshot->peer_best_height)
                snapshot->peer_best_height = node->starting_height;
            if (node->last_block_time > newest_peer_block_time)
                newest_peer_block_time = node->last_block_time;
        }
        zcl_mutex_unlock(&cm->manager.cs_nodes);

        if (newest_peer_block_time > 0) {
            int64_t now = (int64_t)time(NULL);
            if (now > newest_peer_block_time) {
                snapshot->tip_stale_seconds = now - newest_peer_block_time;
                snapshot->tip_stale = snapshot->tip_stale_seconds > 600;
            }
        }
    }

    if (snapshot->peer_best_height >= 0 && snapshot->tip_height >= 0 &&
        snapshot->peer_best_height > snapshot->tip_height) {
        snapshot->tip_lag = snapshot->peer_best_height - snapshot->tip_height;
    }

    dl_get_stats(msg_get_download_mgr(),
                 &snapshot->blocks_requested,
                 &snapshot->blocks_received,
                 &snapshot->blocks_timed_out,
                 &snapshot->in_flight,
                 &snapshot->queued);
    snapshot->queue_backed_up =
        (snapshot->queued > 256 || snapshot->in_flight > 128);

    {
        struct error_ring *er = error_ring_global();
        const struct error_entry *last_err = error_ring_last(er);
        snapshot->error_total = error_ring_total(er);
        if (last_err && last_err->message[0]) {
            snprintf(snapshot->last_error, sizeof(snapshot->last_error),
                     "%s", last_err->message);
        }
    }

    if (g_health_start_time == 0)
        g_health_start_time = (int64_t)time(NULL);
    snapshot->uptime_seconds = (int64_t)time(NULL) - g_health_start_time;

    if (!snapshot->has_peers) {
        snprintf(snapshot->degraded_reason, sizeof(snapshot->degraded_reason),
                 "no_peers");
    } else if (!snapshot->synced) {
        snprintf(snapshot->degraded_reason, sizeof(snapshot->degraded_reason),
                 "sync_state_%s", sync_state_name(snapshot->sync_state));
    } else if (snapshot->tip_lag > 1) {
        snprintf(snapshot->degraded_reason, sizeof(snapshot->degraded_reason),
                 "tip_lag_%d", snapshot->tip_lag);
    } else if (snapshot->tip_stale) {
        snprintf(snapshot->degraded_reason, sizeof(snapshot->degraded_reason),
                 "tip_stale");
    } else if (snapshot->queue_backed_up) {
        snprintf(snapshot->degraded_reason, sizeof(snapshot->degraded_reason),
                 "download_queue_backed_up");
    } else if (snapshot->error_total > 0 && snapshot->last_error[0]) {
        snprintf(snapshot->degraded_reason, sizeof(snapshot->degraded_reason),
                 "recent_error");
    }

    snapshot->healthy = snapshot->synced &&
                        snapshot->has_peers &&
                        !snapshot->tip_stale &&
                        snapshot->tip_lag <= 1;
}
