/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "services/node_health_service.h"
#include "controllers/network_controller.h"
#include "models/block.h"
#include "models/database.h"
#include "net/connman.h"
#include "net/download.h"
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

static int64_t g_health_start_time = 0;

void node_health_collect(struct node_health_snapshot *snapshot,
                         struct node_db *ndb)
{
    struct node_health_snapshot empty = {0};
    if (!snapshot) return;
    *snapshot = empty;

    snapshot->sync_state = sync_get_state();
    snapshot->synced = (snapshot->sync_state == SYNC_AT_TIP);

    struct connman *cm = rpc_net_get_connman();
    snapshot->peer_count = cm ? connman_get_node_count(cm) : 0;
    snapshot->has_peers = snapshot->peer_count > 0;

    snapshot->tip_height = -1;
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

    dl_get_stats(msg_get_download_mgr(),
                 &snapshot->blocks_requested,
                 &snapshot->blocks_received,
                 &snapshot->blocks_timed_out,
                 &snapshot->in_flight,
                 &snapshot->queued);

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

    snapshot->healthy = snapshot->synced && snapshot->has_peers;
}
