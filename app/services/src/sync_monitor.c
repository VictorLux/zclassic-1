/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

// one-result-type-ok:monitor-no-fallible-surface — E2 (one way out): this is
// a sync-monitor / recovery-stats recorder, not an operation executor. Its
// functions are void recorders + setters (init/set_context/record_*/get_*),
// pointer accessors (connman/download_manager/main_state), pure query
// predicates (bool sync_monitor_active_next_child_exists — an existence
// check), and counts/ages (int eligible peers, int64_t tip_advance_age with a
// documented `raw-return-ok:sentinel` -1). None of these is a success/failure
// result that bare bool would strip a reason from. Stats travel via the
// struct watchdog_stats / watchdog_local_recovery_stats out-params; the one
// kick path logs failure context via LOG_WARN with outcome.reason.

#include "services/sync_monitor.h"
#include "util/log_macros.h"

#include "framework/condition.h"
#include "net/connman.h"
#include "platform/time_compat.h"
#include "sync/sync_planner.h"
#include "services/chain_activation_controller.h"
#include "services/gap_fill_service.h"
#include "sync/sync_state.h"
#include "validation/chainstate.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#define LOCAL_HEADER_REFILL_MIN_PEERS 3
#define LOCAL_HEADER_REFILL_MAX_RETRIES 3

static _Atomic int64_t g_last_block_connected_ts;
static _Atomic int g_last_block_connected_height;
static _Atomic int g_recoveries_total;
static _Atomic int64_t g_last_recovery_time;
static _Atomic int g_last_recovery_type;
static _Atomic int g_last_recovery_local_height;
static _Atomic int g_last_recovery_peer_height;
static _Atomic int g_last_recovery_peer_count;
static _Atomic int g_last_recovery_target_height;
static _Atomic int g_last_recovery_manifest_height;
static char g_last_recovery_reason[96];
static char g_last_recovery_trigger[64];

static struct connman *g_condition_cm;
static struct download_manager *g_condition_dm;
static struct main_state *g_condition_ms;

static struct {
    bool active;
    bool retries_exhausted;
    int missing_height;
    int retry_count;
    int distinct_peer_count;
    int peer_rotation_count;
    char mode[32];
    char last_reason[64];
} g_local_recovery;

void sync_monitor_init(void)
{
    memset(&g_local_recovery, 0, sizeof(g_local_recovery));
    memset(g_last_recovery_reason, 0, sizeof(g_last_recovery_reason));
    memset(g_last_recovery_trigger, 0, sizeof(g_last_recovery_trigger));
    atomic_store(&g_recoveries_total, 0);
    atomic_store(&g_last_recovery_time, 0);
    atomic_store(&g_last_recovery_type, WATCHDOG_NONE);
    atomic_store(&g_last_recovery_local_height, -1);
    atomic_store(&g_last_recovery_peer_height, -1);
    atomic_store(&g_last_recovery_peer_count, 0);
    atomic_store(&g_last_recovery_target_height, -1);
    atomic_store(&g_last_recovery_manifest_height, -1);
    sync_state_monitor_init();
}

void sync_monitor_set_context(struct connman *cm,
                              struct download_manager *dm,
                              struct main_state *ms)
{
    g_condition_cm = cm;
    g_condition_dm = dm;
    g_condition_ms = ms;
    condition_engine_set_main_state(ms);

    if (ms && atomic_load(&g_last_block_connected_ts) == 0) {
        int height = active_chain_height(&ms->chain_active);
        if (height >= 0) {
            atomic_store(&g_last_block_connected_ts,
                         (int64_t)platform_time_wall_time_t());
            atomic_store(&g_last_block_connected_height, height);
        }
    }
}

struct connman *sync_monitor_connman(void)
{
    return g_condition_cm;
}

struct download_manager *sync_monitor_download_manager(void)
{
    return g_condition_dm;
}

struct main_state *sync_monitor_main_state(void)
{
    return g_condition_ms ? g_condition_ms : condition_engine_main_state();
}

void sync_monitor_on_block_connected(int height)
{
    atomic_store(&g_last_block_connected_ts,
                 (int64_t)platform_time_wall_time_t());
    atomic_store(&g_last_block_connected_height, height);
}

int64_t sync_monitor_tip_advance_age(void)
{
    int64_t last = atomic_load(&g_last_block_connected_ts);
    if (last == 0)
        return -1; // raw-return-ok:sentinel
    int64_t now = (int64_t)platform_time_wall_time_t();
    return (now > last) ? (now - last) : 0;
}

void sync_monitor_record_recovery(enum watchdog_recovery_type type,
                                  int local_height,
                                  int peer_height,
                                  int peer_count,
                                  const char *reason)
{
    atomic_fetch_add(&g_recoveries_total, 1);
    atomic_store(&g_last_recovery_time,
                 (int64_t)platform_time_wall_time_t());
    atomic_store(&g_last_recovery_type, (int)type);
    atomic_store(&g_last_recovery_local_height, local_height);
    atomic_store(&g_last_recovery_peer_height, peer_height);
    atomic_store(&g_last_recovery_peer_count, peer_count);
    atomic_store(&g_last_recovery_target_height, -1);
    atomic_store(&g_last_recovery_manifest_height, -1);
    snprintf(g_last_recovery_reason, sizeof(g_last_recovery_reason), "%s",
             reason ? reason : "");
    g_last_recovery_trigger[0] = '\0';
}

void sync_monitor_record_snapshot_resnapshot(int local_height,
                                             int peer_height,
                                             int peer_count,
                                             int target_height,
                                             int manifest_height,
                                             const char *trigger,
                                             const char *reason)
{
    sync_monitor_record_recovery(WATCHDOG_SNAPSHOT_RESNAPSHOT,
                                 local_height, peer_height, peer_count,
                                 reason);
    atomic_store(&g_last_recovery_target_height, target_height);
    atomic_store(&g_last_recovery_manifest_height, manifest_height);
    snprintf(g_last_recovery_trigger, sizeof(g_last_recovery_trigger), "%s",
             trigger ? trigger : "");
}

void sync_monitor_kick_local_sync(const char *reason)
{
    gap_fill_kick();

    struct chain_activation_controller *ctl = boot_activation_controller();
    if (!ctl || !ctl->ms || !ctl->coins_tip || !ctl->params || !ctl->datadir)
        return;

    enum activation_state state = activation_get_state(ctl);
    if (state != ACTIVATION_READY && state != ACTIVATION_AT_TIP)
        return;

    struct activation_exec_outcome outcome;
    activation_request_connect(ctl, ACTIVATION_SRC_HEADERS_ALL_DATA,
                               NULL, &outcome);
    if (outcome.result == ACTIVATION_EXEC_FAILED) {
        LOG_WARN("sync_monitor", "[sync_monitor] local activation kick failed (%s): %s", reason ? reason : "unspecified", outcome.reason[0] ? outcome.reason : "unknown");
    }
}

bool sync_monitor_active_next_child_exists(struct main_state *ms,
                                           struct block_index *tip,
                                           int next_h)
{
    if (!ms || !tip)
        return false;

    size_t iter = 0;
    struct block_index *bi = NULL;
    while (block_map_next(&ms->map_block_index, &iter, NULL, &bi)) {
        if (bi && bi->nHeight == next_h && bi->pprev == tip &&
            bi->phashBlock && !(bi->nStatus & BLOCK_FAILED_MASK))
            return true;
    }
    return false;
}

int sync_monitor_local_header_refill(struct connman *cm,
                                     int next_h,
                                     const char *reason)
{
    if (!cm)
        return 0;

    int eligible = 0;
    struct p2p_node *worst = NULL;

    zcl_mutex_lock(&cm->manager.cs_nodes);
    for (size_t i = 0; i < cm->manager.num_nodes; i++) {
        struct p2p_node *n = cm->manager.nodes[i];
        if (!n || n->disconnect || n->inbound)
            continue;
        if (n->starting_height < next_h ||
            n->state < PEER_HANDSHAKE_COMPLETE)
            continue;

        eligible++;
        n->last_getheaders_time = 0;
        n->getheaders_stale_count = 0;
        if (n->state == PEER_HANDSHAKE_COMPLETE ||
            n->state == PEER_ACTIVE ||
            n->state == PEER_SYNCING_BLOCKS ||
            n->state == PEER_STALE) {
            (void)peer_set_state_checked((uint32_t)n->id, &n->state,
                                         PEER_SYNCING_HEADERS,
                                         "condition local header refill");
        }

        if (!worst ||
            n->total_headers_delivered < worst->total_headers_delivered)
            worst = n;
    }

    if (g_local_recovery.retry_count > 0 && worst && eligible >= 2 &&
        eligible < LOCAL_HEADER_REFILL_MIN_PEERS) {
        worst->disconnect = true;
        g_local_recovery.peer_rotation_count++;
    }
    zcl_mutex_unlock(&cm->manager.cs_nodes);

    if (g_local_recovery.retry_count > 0 &&
        eligible < LOCAL_HEADER_REFILL_MIN_PEERS) {
        connman_kick_seed_discovery(cm);
    }

    g_local_recovery.active = true;
    g_local_recovery.missing_height = next_h;
    g_local_recovery.retry_count++;
    g_local_recovery.distinct_peer_count = eligible;
    snprintf(g_local_recovery.mode, sizeof(g_local_recovery.mode),
             "%s", "next-child-missing");
    snprintf(g_local_recovery.last_reason,
             sizeof(g_local_recovery.last_reason), "%s",
             reason ? reason : "");
    if (eligible >= LOCAL_HEADER_REFILL_MIN_PEERS ||
        g_local_recovery.retry_count >= LOCAL_HEADER_REFILL_MAX_RETRIES)
        g_local_recovery.retries_exhausted = true;

    return eligible;
}

void sync_monitor_get_local_recovery_stats(
    struct watchdog_local_recovery_stats *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->active = g_local_recovery.active;
    out->mirror_repair_gated = g_local_recovery.active &&
        !g_local_recovery.retries_exhausted;
    out->retries_exhausted = g_local_recovery.retries_exhausted;
    out->missing_height = g_local_recovery.missing_height;
    out->retry_count = g_local_recovery.retry_count;
    out->distinct_peer_count = g_local_recovery.distinct_peer_count;
    out->peer_rotation_count = g_local_recovery.peer_rotation_count;
    snprintf(out->mode, sizeof(out->mode), "%s", g_local_recovery.mode);
    snprintf(out->last_reason, sizeof(out->last_reason), "%s",
             g_local_recovery.last_reason);
}

void sync_monitor_get_stats(struct watchdog_stats *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->recoveries_total = atomic_load(&g_recoveries_total);
    out->last_recovery_time = atomic_load(&g_last_recovery_time);
    out->last_recovery = atomic_load(&g_last_recovery_type);
    out->last_recovery_local_height =
        atomic_load(&g_last_recovery_local_height);
    out->last_recovery_peer_height =
        atomic_load(&g_last_recovery_peer_height);
    out->last_recovery_peer_count =
        atomic_load(&g_last_recovery_peer_count);
    out->last_recovery_target_height =
        atomic_load(&g_last_recovery_target_height);
    out->last_recovery_manifest_height =
        atomic_load(&g_last_recovery_manifest_height);
    snprintf(out->last_recovery_reason,
             sizeof(out->last_recovery_reason), "%s",
             g_last_recovery_reason);
    snprintf(out->last_recovery_trigger,
             sizeof(out->last_recovery_trigger), "%s",
             g_last_recovery_trigger);
}

const char *watchdog_recovery_type_name(enum watchdog_recovery_type type)
{
    switch (type) {
    case 0: return "NONE";
    case 1: return "HEADER_STALL";
    case 2: return "HEADER_LAG";
    case 3: return "BLOCK_STALL";
    case 4: return "STATE_STUCK";
    case 5: return "REPEATED_RESTART";
    case 6: return "PEER_FLOOR";
    case 7: return "SYNC_VIOLATION";
    case 8: return "QUEUE_STARVED";
    case 9: return "LOCAL_HEADER_REFILL";
    case 10: return "SNAPSHOT_RESNAPSHOT";
    }
    return "UNKNOWN";
}

#ifdef ZCL_TESTING
void sync_monitor_test_set_local_recovery(bool active,
                                          bool retries_exhausted,
                                          int missing_height,
                                          int retry_count,
                                          const char *mode)
{
    g_local_recovery.active = active;
    g_local_recovery.retries_exhausted = retries_exhausted;
    g_local_recovery.missing_height = missing_height;
    g_local_recovery.retry_count = retry_count;
    g_local_recovery.distinct_peer_count = 0;
    g_local_recovery.peer_rotation_count = 0;
    snprintf(g_local_recovery.mode, sizeof(g_local_recovery.mode), "%s",
             mode ? mode : "");
    g_local_recovery.last_reason[0] = '\0';
}

void sync_monitor_test_set_tip_advance_ts(int64_t ts)
{
    atomic_store(&g_last_block_connected_ts, ts);
}
#endif
