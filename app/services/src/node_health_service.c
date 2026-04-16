/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "services/node_health_service.h"
#include "services/sync_watchdog_service.h"
#include "config/runtime.h"
#include "controllers/sync_controller.h"
#include "controllers/network_controller.h"
#include "models/block.h"
#include "models/database.h"
#include "net/onion_service.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"
#include "net/connman.h"
#include "net/download.h"
#include "net/tor_integration.h"
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "util/log_macros.h"

/* Read process start time from /proc/self/stat (field 22, starttime in
 * clock ticks since boot).  Combined with /proc/uptime this gives the
 * true process age even on the very first healthcheck call. */
static int64_t proc_uptime_seconds(void)
{
    /* System uptime */
    double sys_up = 0;
    FILE *f = fopen("/proc/uptime", "r");
    if (!f) return 0;
    if (fscanf(f, "%lf", &sys_up) != 1) { fclose(f); return 0; }
    fclose(f);

    /* Process start time (field 22 of /proc/self/stat) */
    f = fopen("/proc/self/stat", "r");
    if (!f) return 0;
    char buf[1024];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n == 0) return 0;
    buf[n] = '\0';

    /* Skip past comm field (contains parens, may have spaces) */
    const char *p = strrchr(buf, ')');
    if (!p) return 0;
    p++;
    /* Fields after ')': state(3)..starttime(22) — skip 19 fields */
    for (int i = 0; i < 19; i++) {
        while (*p == ' ') p++;
        while (*p && *p != ' ') p++;
    }
    while (*p == ' ') p++;
    long long starttime = 0;
    if (sscanf(p, "%lld", &starttime) != 1) return 0;

    long clk = sysconf(_SC_CLK_TCK);
    if (clk <= 0) clk = 100;
    double proc_start_sec = (double)starttime / (double)clk;
    double age = sys_up - proc_start_sec;
    return age > 0 ? (int64_t)age : 0;
}

static int64_t get_rss_kb(void)
{
    FILE *f = fopen("/proc/self/status", "r");
    if (!f) return -1;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "VmRSS:", 6) == 0) {
            int64_t kb = 0;
            sscanf(line + 6, " %lld", (long long *)&kb);
            fclose(f);
            return kb;
        }
    }
    fclose(f);
    return -1;
}
static const int64_t HEALTH_JOB_STALL_SECONDS = 120;

static bool health_query_int(sqlite3 *db, const char *sql, int *out)
{
    sqlite3_stmt *stmt = NULL;
    bool ok = false;

    if (!db || !sql || !out)
        return false;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        *out = sqlite3_column_int(stmt, 0);
        ok = true;
    }
    sqlite3_finalize(stmt);
    return ok;
}

static bool health_query_int64(sqlite3 *db, const char *sql, int64_t *out)
{
    sqlite3_stmt *stmt = NULL;
    bool ok = false;

    if (!db || !sql || !out)
        return false;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        *out = sqlite3_column_int64(stmt, 0);
        ok = true;
    }
    sqlite3_finalize(stmt);
    return ok;
}

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
    snapshot->tor_enabled = tor_integration_is_enabled();
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

    if (ms) {
        struct block_index *tip = active_chain_tip(&ms->chain_active);
        if (tip) {
            snapshot->tip_height = tip->nHeight;
            if (tip->nTime > 0) {
                int64_t now = (int64_t)time(NULL);
                if (now > (int64_t)tip->nTime) {
                    snapshot->tip_stale_seconds = now - (int64_t)tip->nTime;
                    snapshot->tip_stale = snapshot->tip_stale_seconds > 600;
                }
            }
        }
    }

    if (ndb && ndb->open) {
        struct node_db_status dbs = {0};
        struct db_service *dbsvc = app_runtime_db_service();
        struct db_service_status svc_status = {0};
        sqlite3 *query_db = app_runtime_query_db();
        node_db_get_status(ndb, &dbs);
        db_service_get_status(dbsvc, &svc_status);
        snapshot->db_open = dbs.open;
        snapshot->db_tx_open = dbs.tx_open;
        snapshot->db_turbo_mode = dbs.turbo_mode;
        snapshot->db_last_sqlite_rc = dbs.last_sqlite_rc;
        snapshot->db_service_started = svc_status.started;
        snapshot->db_service_worker_started = svc_status.worker_started;
        snapshot->db_service_stop_requested = svc_status.stop_requested;
        snapshot->db_service_queue_depth = svc_status.queue_depth;
        if (svc_status.started_at > 0) {
            int64_t now = (int64_t)time(NULL);
            if (now >= svc_status.started_at)
                snapshot->db_service_uptime_seconds =
                    now - svc_status.started_at;
        }
        snprintf(snapshot->db_last_op, sizeof(snapshot->db_last_op),
                 "%s", dbs.last_op);
        if (dbs.last_activity_time > 0) {
            int64_t now = (int64_t)time(NULL);
            if (now >= dbs.last_activity_time)
                snapshot->db_last_activity_age_seconds =
                    now - dbs.last_activity_time;
        }
        if (snapshot->tip_height < 0) {
            if (!health_query_int(query_db,
                                  "SELECT COALESCE(MAX(height), -1) FROM blocks",
                                  &snapshot->tip_height)) {
                snapshot->tip_height = db_block_max_height(ndb);
            }
        }
        if (!health_query_int64(query_db,
                                "SELECT count(*) FROM utxos",
                                &snapshot->utxo_count)) {
            snapshot->utxo_count = node_db_utxo_count(ndb);
        }

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

        (void)newest_peer_block_time;
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
        struct node_db_sync_job_status jobs = {0};
        node_db_sync_get_job_status(&jobs);
        snapshot->catchup_active = jobs.catchup_active;
        snapshot->catchup_height = jobs.catchup_height;
        snapshot->catchup_target_height = jobs.catchup_target_height;
        snapshot->import_active = jobs.import_active;
        snapshot->import_rows_written = jobs.import_rows_written;
        if (jobs.catchup_started_at > 0) {
            int64_t now = (int64_t)time(NULL);
            if (now >= jobs.catchup_started_at)
                snapshot->catchup_uptime_seconds = now - jobs.catchup_started_at;
        }
        if (jobs.catchup_last_progress_at > 0) {
            int64_t now = (int64_t)time(NULL);
            if (now >= jobs.catchup_last_progress_at)
                snapshot->catchup_progress_age_seconds =
                    now - jobs.catchup_last_progress_at;
        }
        if (jobs.import_started_at > 0) {
            int64_t now = (int64_t)time(NULL);
            if (now >= jobs.import_started_at)
                snapshot->import_uptime_seconds = now - jobs.import_started_at;
        }
        if (jobs.import_last_progress_at > 0) {
            int64_t now = (int64_t)time(NULL);
            if (now >= jobs.import_last_progress_at)
                snapshot->import_progress_age_seconds =
                    now - jobs.import_last_progress_at;
        }

        struct error_ring *er = error_ring_global();
        const struct error_entry *last_err = error_ring_last(er);
        snapshot->error_total = error_ring_total(er);
        if (last_err && last_err->message[0]) {
            snprintf(snapshot->last_error, sizeof(snapshot->last_error),
                     "%s", last_err->message);
        }
    }

    snapshot->uptime_seconds = proc_uptime_seconds();

    {
        int64_t rss_kb = get_rss_kb();
        snapshot->memory_rss_mb = (rss_kb > 0) ? rss_kb / 1024 : -1;
    }

    if (!snapshot->has_peers) {
        snprintf(snapshot->degraded_reason, sizeof(snapshot->degraded_reason),
                 "no_peers");
    } else if (snapshot->catchup_active &&
               snapshot->catchup_progress_age_seconds > HEALTH_JOB_STALL_SECONDS) {
        snprintf(snapshot->degraded_reason, sizeof(snapshot->degraded_reason),
                 "catchup_stalled_%llds",
                 (long long)snapshot->catchup_progress_age_seconds);
    } else if (snapshot->import_active &&
               snapshot->import_progress_age_seconds > HEALTH_JOB_STALL_SECONDS) {
        snprintf(snapshot->degraded_reason, sizeof(snapshot->degraded_reason),
                 "import_stalled_%llds",
                 (long long)snapshot->import_progress_age_seconds);
    } else if (!snapshot->synced) {
        snprintf(snapshot->degraded_reason, sizeof(snapshot->degraded_reason),
                 "sync_state_%s", sync_state_name(snapshot->sync_state));
    } else if (snapshot->header_height > snapshot->tip_height + 1) {
        snprintf(snapshot->degraded_reason, sizeof(snapshot->degraded_reason),
                 "headers_ahead_%d", snapshot->header_height - snapshot->tip_height);
    } else if (snapshot->tip_lag > 1) {
        snprintf(snapshot->degraded_reason, sizeof(snapshot->degraded_reason),
                 "tip_lag_%d", snapshot->tip_lag);
    } else if (snapshot->tip_stale) {
        snprintf(snapshot->degraded_reason, sizeof(snapshot->degraded_reason),
                 "tip_stale");
    } else if (snapshot->queue_backed_up) {
        snprintf(snapshot->degraded_reason, sizeof(snapshot->degraded_reason),
                 "download_queue_backed_up");
    } else if (snapshot->db_service_started &&
               !snapshot->db_service_worker_started) {
        snprintf(snapshot->degraded_reason, sizeof(snapshot->degraded_reason),
                 "db_service_worker_down");
    } else if (snapshot->db_service_queue_depth > 32) {
        snprintf(snapshot->degraded_reason, sizeof(snapshot->degraded_reason),
                 "db_service_queue_%zu", snapshot->db_service_queue_depth);
    } else if (snapshot->db_tx_open &&
               snapshot->db_last_activity_age_seconds > 60) {
        snprintf(snapshot->degraded_reason, sizeof(snapshot->degraded_reason),
                 "db_tx_open_%llds",
                 (long long)snapshot->db_last_activity_age_seconds);
    } else if (snapshot->error_total > 0 && snapshot->last_error[0]) {
        snprintf(snapshot->degraded_reason, sizeof(snapshot->degraded_reason),
                 "recent_error");
    } else if (snapshot->memory_rss_mb > 4096) {
        snprintf(snapshot->degraded_reason, sizeof(snapshot->degraded_reason),
                 "high_memory_usage");
    }

    snapshot->healthy = snapshot->synced &&
                        snapshot->has_peers &&
                        snapshot->header_height <= snapshot->tip_height + 1 &&
                        !snapshot->tip_stale &&
                        snapshot->tip_lag <= 1;

    /* Watchdog stats */
    {
        struct watchdog_stats wd;
        sync_watchdog_get_stats(&wd);
        snapshot->wd_checks_run = wd.checks_run;
        snapshot->wd_recoveries = wd.recoveries_total;
        snapshot->wd_blocks_per_sec = wd.blocks_per_sec;
        snapshot->wd_escalation_level = wd.escalation_level;
        snapshot->wd_last_recovery_time = wd.last_recovery_time;
        snapshot->wd_last_recovery_type = (int)wd.last_recovery;
        snprintf(snapshot->wd_last_recovery_name,
                 sizeof(snapshot->wd_last_recovery_name),
                 "%s", watchdog_recovery_type_name(wd.last_recovery));
    }
}
