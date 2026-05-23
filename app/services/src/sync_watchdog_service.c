/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Sync Watchdog Service — automatic stall detection and recovery.
 *
 * Runs from the message processing loop (~every 30s). Detects:
 *   HEADER_STALL:  headers_download for >300s with no header progress
 *   BLOCK_STALL:   blocks_download for >300s with no chain height progress
 *   STATE_STUCK:   any sync state (except at_tip) unchanged for >600s
 *   REPEATED_RESTART: circuit breaker after >3 recoveries in 30 minutes */

#include "platform/time_compat.h"
#include "services/sync_watchdog_service.h"
#include "services/block_sync_service.h"
#include "services/chain_activation_controller.h"
#include "services/chain_advance_coordinator.h"
#include "services/gap_fill_service.h"
#include "supervisors/domains.h"
#include "net/connman.h"
#include "validation/chainstate.h"
#include "validation/process_block.h"
#include "net/download.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdatomic.h>

#include "util/log_macros.h"
#include "util/long_op.h"
#include "util/supervisor.h"
#include "util/thread_registry.h"
#include "health/heartbeat.h"

/* ── Thresholds ──────────────────────────────────────────── */

#define HEADER_STALL_SECS    300   /* 5 minutes */
#define REPEATED_WINDOW_SECS 1800  /* 30 minutes */
#define REPEATED_MAX         3     /* max recoveries before giving up */

/* Per-state stuck timeouts (replaces single STATE_STUCK_SECS=600).
 * HEADERS_DOWNLOAD and BLOCKS_DOWNLOAD keep longer timeouts because
 * they have dedicated stall checks (HEADER_STALL, HEADER_LAG, BLOCK_STALL)
 * that fire first — STATE_STUCK is their last-resort backup. */
static int64_t state_stuck_timeout(enum sync_state state)
{
    switch (state) {
    case SYNC_FINDING_PEERS:      return 120;
    case SYNC_HEADERS_DOWNLOAD:   return 600;  /* backup for HEADER_STALL */
    case SYNC_BLOCKS_DOWNLOAD:    return 600;  /* backup for BLOCK_STALL */
    case SYNC_CONNECTING_BLOCKS:  return 180;
    case SYNC_REORG:              return 60;
    default:                      return 300;
    }
}

/* ── Sync state timestamps (Task 2) ─────────────────────── */

_Atomic int64_t g_sync_state_entered_time = 0;
_Atomic int     g_sync_state_entry_height = 0;

/* Tip-advance tracking — last successful block connect timestamp.
 * Updated by sync_watchdog_on_block_connected() from the EV_BLOCK_CONNECTED
 * emit site in lib/net/src/msg_blocks.c. Read by
 * sync_watchdog_get_tip_advance_age() and exposed via zcl_status JSON +
 * Prometheus zcl_tip_advance_age_seconds. Independent of state-entered
 * time because state changes (e.g. AT_TIP→HEADERS_DOWNLOAD on a recovery
 * toggle) should NOT zero the tip-advance age. */
_Atomic int64_t g_last_block_connected_ts = 0;
_Atomic int     g_last_block_connected_height = 0;

/* Once-per-stall-episode emit throttle for EV_TIP_STALE.
 * Set when the STATE_STUCK path emits; cleared on any state change OR
 * any block connect (sync_watchdog_on_state_change /
 * sync_watchdog_on_block_connected). Atomic so the watchdog tick thread
 * and the message-processing thread don't race. */
_Atomic int     g_stall_event_emitted = 0;

void sync_watchdog_on_state_change(enum sync_state new_state, int height)
{
    (void)new_state;
    atomic_store(&g_sync_state_entered_time, (int64_t)platform_time_wall_time_t());
    atomic_store(&g_sync_state_entry_height, height);
    /* State changed → if a stall just ended, allow a fresh EV_TIP_STALE
     * the next time we get stuck. Don't clear g_last_block_connected_ts:
     * forced state toggles (the STATE_STUCK recovery path) shouldn't
     * mask real tip staleness. */
    atomic_store(&g_stall_event_emitted, 0);
}

void sync_watchdog_on_block_connected(int height)
{
    atomic_store(&g_last_block_connected_ts, (int64_t)platform_time_wall_time_t());
    atomic_store(&g_last_block_connected_height, height);
    /* Block actually arrived → previous stall (if any) is over. */
    atomic_store(&g_stall_event_emitted, 0);
}

int64_t sync_watchdog_get_tip_advance_age(void)
{
    int64_t last = atomic_load(&g_last_block_connected_ts);
    /* Bootstrap: if we have not seen a connect yet, age is meaningless;
     * report -1 so consumers (zcl_health, Prometheus) can skip the gate. */
    if (last == 0) return -1; // raw-return-ok:sentinel
    int64_t now = (int64_t)platform_time_wall_time_t();
    return (now > last) ? (now - last) : 0;
}

int64_t sync_get_state_duration(void)
{
    int64_t entered = atomic_load(&g_sync_state_entered_time);
    if (entered == 0)
        return 0;
    int64_t now = (int64_t)platform_time_wall_time_t();
    return (now > entered) ? (now - entered) : 0;
}

int sync_get_state_entry_height(void)
{
    return atomic_load(&g_sync_state_entry_height);
}

/* ── Watchdog internal state ─────────────────────────────── */

static struct {
    bool     initialized;
    uint64_t checks_run;
    uint64_t recoveries_triggered;
    int64_t  last_recovery_time;
    enum watchdog_recovery_type last_recovery_type;

    /* For header stall: track best header height at last check */
    int      last_header_height;

    /* For block stall: track chain height at last check */
    int      last_chain_height;

    /* Circuit breaker: timestamps of recent recoveries */
    int64_t  recovery_times[REPEATED_MAX + 1];
    int      recovery_count;

    /* Header stall escalation: consecutive HEADER_STALL recoveries */
    int      header_stall_consecutive;
    bool     header_stall_escalated;

    /* Escalation levels (Task 3): never give up */
    int      escalation_level;     /* 0=none, 1=default, 2=deep reset, 3=full reload */
    int      l1_failures;          /* L1 failures within l1_window_start */
    int64_t  l1_window_start;
    int      l2_failures;          /* L2 failures within l2_window_start */
    int64_t  l2_window_start;

    /* Progress rate tracking (Task 4) */
    struct { int height; int64_t timestamp; } progress[5];
    int      progress_count;
    int      progress_index;       /* circular index into progress[] */
    double   blocks_per_sec;

    int      last_recovery_local_height;
    int      last_recovery_peer_height;
    int      last_recovery_peer_count;
    char     last_recovery_reason[96];
} g_watchdog;

/* Last header reject reason (updated by msg_headers.c via setter) */
static char g_last_header_reject_reason[256] = {0};

static void local_recovery_reset(void);

void sync_watchdog_set_last_reject_reason(const char *reason)
{
    if (reason) {
        strncpy(g_last_header_reject_reason, reason,
                sizeof(g_last_header_reject_reason) - 1);
        g_last_header_reject_reason[sizeof(g_last_header_reject_reason) - 1]
            = '\0';
    }
}

/* ── Public API ──────────────────────────────────────────── */

void sync_watchdog_init(void)
{
    memset(&g_watchdog, 0, sizeof(g_watchdog));
    g_watchdog.initialized = true;
    g_watchdog.last_header_height = -1;
    g_watchdog.last_chain_height = -1;
    local_recovery_reset();
    atomic_store(&g_sync_state_entered_time, (int64_t)platform_time_wall_time_t());
    sync_set_state_change_callback(sync_watchdog_on_state_change);
}

const char *watchdog_recovery_type_name(enum watchdog_recovery_type type)
{
    switch (type) {
    case WATCHDOG_NONE:             return "NONE";
    case WATCHDOG_HEADER_STALL:     return "HEADER_STALL";
    case WATCHDOG_BLOCK_STALL:      return "BLOCK_STALL";
    case WATCHDOG_STATE_STUCK:      return "STATE_STUCK";
    case WATCHDOG_HEADER_LAG:       return "HEADER_LAG";
    case WATCHDOG_REPEATED_RESTART: return "REPEATED_RESTART";
    case WATCHDOG_PEER_FLOOR:       return "PEER_FLOOR";
    case WATCHDOG_SYNC_VIOLATION:   return "SYNC_VIOLATION";
    case WATCHDOG_QUEUE_STARVED:    return "QUEUE_STARVED";
    case WATCHDOG_LOCAL_HEADER_REFILL: return "LOCAL_HEADER_REFILL";
    }
    return "UNKNOWN";
}

void sync_watchdog_get_status(struct sync_watchdog_status *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));

    out->enabled = g_watchdog.initialized;
    out->checks_run = g_watchdog.checks_run;
    out->recoveries_triggered = g_watchdog.recoveries_triggered;
    out->last_recovery_time = g_watchdog.last_recovery_time;
    out->last_recovery_type = g_watchdog.last_recovery_type;
    out->current_state = sync_get_state();
    out->current_state_duration_secs = sync_get_state_duration();
    out->current_state_entry_height = sync_get_state_entry_height();
    out->escalation_level = g_watchdog.escalation_level;
    out->last_recovery_local_height = g_watchdog.last_recovery_local_height;
    out->last_recovery_peer_height = g_watchdog.last_recovery_peer_height;
    out->last_recovery_peer_count = g_watchdog.last_recovery_peer_count;
    snprintf(out->last_recovery_reason, sizeof(out->last_recovery_reason),
             "%s", g_watchdog.last_recovery_reason);
}

void sync_watchdog_get_stats(struct watchdog_stats *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));

    out->checks_run = (int)g_watchdog.checks_run;
    out->recoveries_total = (int)g_watchdog.recoveries_triggered;
    out->escalation_level = g_watchdog.escalation_level;
    out->blocks_per_sec = g_watchdog.blocks_per_sec;
    out->last_recovery_time = g_watchdog.last_recovery_time;
    out->last_recovery = g_watchdog.last_recovery_type;
}

/* ── State-dump (see CLAUDE.md "Adding state introspection") ──── */

#include "json/json.h"

bool sync_watchdog_dump_state_json(struct json_value *out, const char *key)
{
    (void)key;
    if (!out) return false;

    struct sync_watchdog_status ws;
    struct watchdog_stats st;
    sync_watchdog_get_status(&ws);
    sync_watchdog_get_stats(&st);

    json_set_object(out);
    json_push_kv_bool(out, "enabled", ws.enabled);
    json_push_kv_int(out, "checks_run", (int64_t)ws.checks_run);
    json_push_kv_int(out, "recoveries_triggered",
                     (int64_t)ws.recoveries_triggered);
    json_push_kv_int(out, "last_recovery_time", ws.last_recovery_time);
    json_push_kv_str(out, "last_recovery_type",
                     watchdog_recovery_type_name(ws.last_recovery_type));
    json_push_kv_str(out, "current_state",
                     sync_state_name(ws.current_state));
    json_push_kv_int(out, "current_state_duration_secs",
                     ws.current_state_duration_secs);
    json_push_kv_int(out, "current_state_entry_height",
                     (int64_t)ws.current_state_entry_height);
    json_push_kv_int(out, "escalation_level", (int64_t)ws.escalation_level);
    json_push_kv_str(out, "last_recovery_reason",
                     ws.last_recovery_reason);
    json_push_kv_int(out, "last_recovery_local_height",
                     ws.last_recovery_local_height);
    json_push_kv_int(out, "last_recovery_peer_height",
                     ws.last_recovery_peer_height);
    json_push_kv_int(out, "last_recovery_peer_count",
                     ws.last_recovery_peer_count);
    json_push_kv_real(out, "blocks_per_sec", st.blocks_per_sec);
    json_push_kv_int(out, "utxo_paused_height",
                     (int64_t)process_block_get_utxo_activation_paused_height());
    {
        struct watchdog_local_recovery_stats lr;
        sync_watchdog_get_local_recovery_stats(&lr);
        json_push_kv_bool(out, "local_recovery_active", lr.active);
        json_push_kv_bool(out, "legacy_advisory_gated_by_native_retries",
                          lr.mirror_repair_gated);
        json_push_kv_bool(out, "mirror_repair_gated_by_local_retries",
                          lr.mirror_repair_gated);
        json_push_kv_bool(out, "local_retries_exhausted",
                          lr.retries_exhausted);
        json_push_kv_int(out, "local_missing_height", lr.missing_height);
        json_push_kv_int(out, "local_retry_count", lr.retry_count);
        json_push_kv_int(out, "local_distinct_peer_count",
                         lr.distinct_peer_count);
        json_push_kv_int(out, "local_peer_rotation_count",
                         lr.peer_rotation_count);
        json_push_kv_str(out, "local_recovery_mode", lr.mode);
        json_push_kv_str(out, "last_local_refill_reason", lr.last_reason);
    }
    return true;
}

/* Forward declarations */
static int disconnect_outbound_peers(struct connman *cm);
static void watchdog_kick_local_sync(const char *reason);

static void watchdog_reconcile_at_tip(enum sync_state state)
{
    if (state == SYNC_IDLE)
        sync_set_state(SYNC_FINDING_PEERS,
                       "watchdog at-tip reconciliation via peers");
    if (sync_get_state() == SYNC_FINDING_PEERS)
        sync_set_state(SYNC_HEADERS_DOWNLOAD,
                       "watchdog at-tip reconciliation via headers");
    if (sync_get_state() == SYNC_HEADERS_DOWNLOAD)
        sync_set_state(SYNC_BLOCKS_DOWNLOAD,
                       "watchdog at-tip reconciliation via blocks");
    if (sync_get_state() == SYNC_BLOCKS_DOWNLOAD)
        sync_set_state(SYNC_CONNECTING_BLOCKS,
                       "watchdog at-tip reconciliation via connect");
    if (sync_get_state() == SYNC_CONNECTING_BLOCKS)
        sync_set_state(SYNC_AT_TIP, "watchdog at-tip reconciliation");
}

/* ── Escalation check (replaces circuit breaker) ────────── */

#define ESCALATION_WINDOW_SECS 1800  /* 30 minutes */
#define ESCALATION_THRESHOLD   3     /* failures before escalating */

static int check_escalation_level(int64_t now)
{
    /* Check L2→L3: 3 L2 failures in 30min */
    if (g_watchdog.escalation_level >= 2) {
        if (now - g_watchdog.l2_window_start > ESCALATION_WINDOW_SECS) {
            g_watchdog.l2_failures = 0;
            g_watchdog.l2_window_start = now;
        }
        if (g_watchdog.l2_failures >= ESCALATION_THRESHOLD)
            return 3;
    }

    /* Check L1→L2: 3 L1 failures in 30min */
    if (g_watchdog.escalation_level >= 1) {
        if (now - g_watchdog.l1_window_start > ESCALATION_WINDOW_SECS) {
            g_watchdog.l1_failures = 0;
            g_watchdog.l1_window_start = now;
        }
        if (g_watchdog.l1_failures >= ESCALATION_THRESHOLD)
            return 2;
    }

    return g_watchdog.escalation_level > 0 ? g_watchdog.escalation_level : 1;
}

static void record_recovery_detail(int64_t now,
                                   enum watchdog_recovery_type type,
                                   const char *reason,
                                   int local_height,
                                   int peer_height,
                                   int peer_count)
{
    g_watchdog.recoveries_triggered++;
    g_watchdog.last_recovery_time = now;
    g_watchdog.last_recovery_type = type;
    g_watchdog.last_recovery_local_height = local_height;
    g_watchdog.last_recovery_peer_height = peer_height;
    g_watchdog.last_recovery_peer_count = peer_count;
    snprintf(g_watchdog.last_recovery_reason,
             sizeof(g_watchdog.last_recovery_reason),
             "%s", reason && *reason ? reason : "unspecified");

    /* Track failures per escalation level */
    int level = g_watchdog.escalation_level;
    if (level <= 1) {
        if (g_watchdog.l1_window_start == 0)
            g_watchdog.l1_window_start = now;
        g_watchdog.l1_failures++;
        g_watchdog.escalation_level = 1;
    } else if (level == 2) {
        if (g_watchdog.l2_window_start == 0)
            g_watchdog.l2_window_start = now;
        g_watchdog.l2_failures++;
    }

    /* Shift old timestamps if full */
    if (g_watchdog.recovery_count >= REPEATED_MAX + 1) {
        memmove(&g_watchdog.recovery_times[0],
                &g_watchdog.recovery_times[1],
                sizeof(int64_t) * REPEATED_MAX);
        g_watchdog.recovery_count = REPEATED_MAX;
    }
    g_watchdog.recovery_times[g_watchdog.recovery_count++] = now;
}

static void record_recovery(int64_t now, enum watchdog_recovery_type type)
{
    record_recovery_detail(now, type, watchdog_recovery_type_name(type),
                           -1, -1, -1);
}

/* L2 recovery: clear download state, force header re-sync from genesis */
static void escalation_l2(struct connman *cm, struct download_manager *dm)
{
    printf("[watchdog] ESCALATION L2: clearing download state, "
           "restarting header sync from genesis\n");
    event_emitf(EV_SYNC_STATE_CHANGE, 0,
                "watchdog ESCALATION L2: deep header reset");

    /* Clear download manager completely */
    if (dm) {
        zcl_mutex_lock(&dm->cs);
        for (size_t i = 0; i < dm->num_slots; i++) {
            dm->slots[i].active = false;
        }
        dm->num_active = 0;
        dm->queue_len = 0;
        zcl_mutex_unlock(&dm->cs);
    }

    disconnect_outbound_peers(cm);

    if (!sync_set_state(SYNC_HEADERS_DOWNLOAD,
                        "watchdog L2: header resync from genesis")) {
        sync_set_state(SYNC_IDLE, "watchdog L2 via idle");
        sync_set_state(SYNC_HEADERS_DOWNLOAD,
                       "watchdog L2: header resync from genesis");
    }
    g_watchdog.escalation_level = 2;
}

/* L3 recovery: full state reset — reload block index, restart from IDLE */
static void escalation_l3(struct connman *cm, struct download_manager *dm)
{
    printf("[watchdog] ESCALATION L3: full state reset, "
           "reloading block index\n");
    event_emitf(EV_SYNC_STATE_CHANGE, 0,
                "watchdog ESCALATION L3: full state reset");

    /* Clear download manager */
    if (dm) {
        zcl_mutex_lock(&dm->cs);
        for (size_t i = 0; i < dm->num_slots; i++) {
            dm->slots[i].active = false;
        }
        dm->num_active = 0;
        dm->queue_len = 0;
        zcl_mutex_unlock(&dm->cs);
    }

    disconnect_outbound_peers(cm);

    /* Reset to IDLE to force full re-initialization */
    sync_set_state(SYNC_IDLE, "watchdog L3: full state reset");
    g_watchdog.escalation_level = 3;
    /* Reset L2/L3 failure counters to give the new level a chance */
    g_watchdog.l2_failures = 0;
    g_watchdog.l2_window_start = 0;
}

/* ── Disconnect all outbound peers ───────────────────────── */

static int disconnect_outbound_peers(struct connman *cm)
{
    if (!cm) return 0;
    int disconnected = 0;

    zcl_mutex_lock(&cm->manager.cs_nodes);
    for (size_t i = 0; i < cm->manager.num_nodes; i++) {
        struct p2p_node *node = cm->manager.nodes[i];
        if (node && !node->inbound && !node->disconnect) {
            node->disconnect = true;
            disconnected++;
        }
    }
    zcl_mutex_unlock(&cm->manager.cs_nodes);

    return disconnected;
}

static void watchdog_kick_local_sync(const char *reason)
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
        fprintf(stderr, // obs-ok:pre-existing-diagnostic
                "[watchdog] local activation kick failed (%s): %s\n",
                reason ? reason : "unspecified",
                outcome.reason[0] ? outcome.reason : "unknown");
    }
}

/* ── Main watchdog check ─────────────────────────────────── */

/* ── Persistent counters for the floor / sync-violation invariants ──
 *
 * Each invariant has a "first observed" timestamp. Once a violation
 * has been continuous for the required window we fire the recovery
 * exactly once and reset the timestamp. The recovery is deliberately
 * idempotent (rewalk seeds / force-rotate peers) so multiple firings
 * during persistent network outages don't make things worse. */

static int64_t g_peer_floor_first_violation = 0;
static int64_t g_sync_violation_first_seen = 0;
int64_t g_queue_starved_first_seen = 0;

#define PEER_FLOOR_MIN_HEALTHY    3
#define PEER_FLOOR_TRIGGER_SECS  60
#define SYNC_VIOLATION_GAP      100   /* blocks behind peer max */
#define SYNC_VIOLATION_SECS     600   /* sustained for 10 min */
#define QUEUE_STARVED_TRIGGER_SECS 120 /* starved for >2 min */
#define QUEUE_STARVED_RATIO_DEN    10  /* < 1/10th of IBD in-flight limit */
#define LOCAL_HEADER_REFILL_MIN_PEERS 3
#define LOCAL_HEADER_REFILL_MAX_RETRIES 3

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

static void local_recovery_reset(void)
{
    memset(&g_local_recovery, 0, sizeof(g_local_recovery));
}

static bool active_next_child_exists(struct main_state *ms,
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

static int local_header_refill_from_peers(struct connman *cm,
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
                                         "watchdog local header refill");
        }

        if (!worst ||
            n->total_headers_delivered < worst->total_headers_delivered)
            worst = n;
    }

    /* Self-preservation: never disconnect a peer when it's the only one
     * carrying the missing height. With eligible<=1, "rotating" the worst
     * peer means rotating our only fetch source — the chain stops dead.
     * The right response there is to widen the peer set (seed re-walk),
     * not to discard what we have. */
    if (g_local_recovery.retry_count > 0 && worst && eligible >= 2 &&
        eligible < LOCAL_HEADER_REFILL_MIN_PEERS) {
        worst->disconnect = true;
        g_local_recovery.peer_rotation_count++;
    }
    zcl_mutex_unlock(&cm->manager.cs_nodes);

    /* When the eligible set is too narrow to risk rotation, kick the
     * outbound discovery so the addrman has a fresh selection on the
     * next tick. This is idempotent and cheap. */
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

void sync_watchdog_get_local_recovery_stats(
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

enum watchdog_recovery_type sync_watchdog_check(
    struct connman *cm,
    struct download_manager *dm,
    struct main_state *ms)
{
    if (!g_watchdog.initialized)
        return WATCHDOG_NONE;

    g_watchdog.checks_run++;

    int64_t now = (int64_t)platform_time_wall_time_t();
    enum sync_state state = sync_get_state();
    int64_t duration = sync_get_state_duration();
    int local_height = ms ? active_chain_height(&ms->chain_active) : -1;
    int best_header_height =
        ms && ms->pindex_best_header ? ms->pindex_best_header->nHeight : -1;
    int peer_max_height = cm ? connman_max_peer_height(cm) : -1;
    size_t healthy = connman_outbound_healthy_count(cm);

    /* Local sync is the primary authority. Mirror/zclassicd is an
     * auxiliary source owned by legacy_mirror_sync_service; the watchdog
     * must not invoke it before exercising peer download, gap-fill, and
     * activation recovery. */

    if (state != SYNC_AT_TIP && local_height >= 0 &&
        best_header_height >= 0 && best_header_height <= local_height &&
        peer_max_height <= local_height && healthy > 0) {
        watchdog_reconcile_at_tip(state);
        return WATCHDOG_NONE;
    }

    if (state != SYNC_AT_TIP && ms && dm &&
        local_height >= 0 && best_header_height > local_height) {
        struct sync_next_block_download download;
        if (syncsvc_queue_next_block_download(&download, ms, dm)) {
            printf("[watchdog] NEXT_BLOCK_DOWNLOAD: h=%d reason=%s "
                   "queued=%d\n",
                   download.height, download.reason, download.queued);
            event_emitf(EV_SYNC_STATE_CHANGE, 0,
                        "watchdog NEXT_BLOCK_DOWNLOAD h=%d reason=%s",
                        download.height, download.reason);
            watchdog_kick_local_sync("native-next-block-download");
            record_recovery_detail(now, WATCHDOG_BLOCK_STALL,
                                   download.reason, local_height,
                                   best_header_height, (int)healthy);
            return WATCHDOG_BLOCK_STALL;
        }
    }

    /* ── Part C: PEER_FLOOR — < 3 healthy outbound for > 60s ──
     * Even with peers in the table, if none are past handshake we
     * can't sync. Forces a fresh seed walk + drops any inbound peers
     * blocking outbound slots. */
    {
        if (healthy < PEER_FLOOR_MIN_HEALTHY) {
            if (g_peer_floor_first_violation == 0)
                g_peer_floor_first_violation = now;
            if (now - g_peer_floor_first_violation > PEER_FLOOR_TRIGGER_SECS) {
                struct cac_decision decision;
                bool recover =
                    chain_advance_coordinator_peer_floor_recovery_needed(
                        (int)healthy,
                        PEER_FLOOR_MIN_HEALTHY,
                        local_height,
                        peer_max_height,
                        &decision);
                printf("[watchdog] PEER_FLOOR: only %zu/%d healthy "
                       "outbound, coordinator=%s reason=%s\n",
                       healthy, PEER_FLOOR_MIN_HEALTHY,
                       recover ? "recover" : "wait",
                       decision.reason);
                event_emitf(EV_SYNC_STATE_CHANGE, 0,
                            "watchdog PEER_FLOOR healthy=%zu decision=%s "
                            "reason=%s",
                            healthy,
                            cac_decision_result_name(decision.result),
                            decision.reason);
                if (!recover) {
                    g_peer_floor_first_violation = now;
                    return WATCHDOG_NONE;
                }
                /* Disconnect outbound peers stuck below handshake
                 * (PEER_CONNECTING/PEER_CONNECTED) to free slots for
                 * the outbound thread's aggressive backfill. */
                if (cm) {
                    size_t inbound_seen = 0;
                    size_t inbound_dropped = 0;
                    size_t outbound_dropped = 0;
                    zcl_mutex_lock(&cm->manager.cs_nodes);
                    for (size_t i = 0; i < cm->manager.num_nodes; i++) {
                        struct p2p_node *n = cm->manager.nodes[i];
                        if (n && !n->inbound && !n->disconnect &&
                            n->state < PEER_HANDSHAKE_COMPLETE) {
                            n->disconnect = true;
                            outbound_dropped++;
                        }
                        if (n && n->inbound && !n->disconnect) {
                            inbound_seen++;
                            if (inbound_seen > 2) {
                                n->disconnect = true;
                                inbound_dropped++;
                            }
                        }
                    }
                    zcl_mutex_unlock(&cm->manager.cs_nodes);
                    for (int ai = 0; ai < cm->num_addnodes; ai++) {
                        if (cm->addnode_protocol_failures[ai] == 0 &&
                            cm->addnode_tcp_failures[ai] > 0) {
                            cm->addnode_backoff_sec[ai] = 0;
                            cm->addnode_last_attempt[ai] = 0;
                        }
                    }
                    event_emitf(EV_SYNC_STATE_CHANGE, 0,
                                "watchdog PEER_FLOOR churn "
                                "drop_outbound=%zu drop_inbound=%zu "
                                "reset_tcp_backoff=true",
                                outbound_dropped, inbound_dropped);
                }
                g_peer_floor_first_violation = now;  /* re-arm */
                watchdog_kick_local_sync("peer-floor");
                record_recovery_detail(now, WATCHDOG_PEER_FLOOR,
                                       decision.reason,
                                       local_height,
                                       peer_max_height,
                                       (int)healthy);
                return WATCHDOG_PEER_FLOOR;
            }
        } else {
            g_peer_floor_first_violation = 0;
        }
    }

    /* moved to condition utxo_activation_paused (PR-1, 2026-05-23) */

    /* ── Part D: SYNC_VIOLATION — peer_max - tip > 100 for > 600s ──
     * Active-tense invariant: the node MUST stay within 100 blocks
     * of its best-known peer. If it drifts further for 10 min,
     * skip L1 peer rotation and go straight to L2 (drop outbound +
     * rewalk seeds). */
    if (cm && ms) {
        int our_height = active_chain_height(&ms->chain_active);
        int peer_max = connman_max_peer_height(cm);
        bool violated = (peer_max > 0 && our_height >= 0 &&
                         peer_max - our_height > SYNC_VIOLATION_GAP);
        if (violated) {
            if (g_sync_violation_first_seen == 0)
                g_sync_violation_first_seen = now;
            if (now - g_sync_violation_first_seen > SYNC_VIOLATION_SECS) {
                printf("[watchdog] SYNC_VIOLATION: tip=%d, peer_max=%d, "
                       "gap=%d for %llds — forcing L2 recovery\n",
                       our_height, peer_max, peer_max - our_height,
                       (long long)(now - g_sync_violation_first_seen));
                event_emitf(EV_SYNC_STATE_CHANGE, 0,
                            "watchdog SYNC_VIOLATION tip=%d peer=%d gap=%d",
                            our_height, peer_max, peer_max - our_height);
                escalation_l2(cm, dm);
                watchdog_kick_local_sync("sync-violation");
                g_sync_violation_first_seen = now;  /* re-arm */
                record_recovery(now, WATCHDOG_SYNC_VIOLATION);
                return WATCHDOG_SYNC_VIOLATION;
            }
        } else {
            g_sync_violation_first_seen = 0;
        }
    }

    /* NEXT_CHILD_MISSING — peers are ahead but the local block index
     * has no connectable header at active_tip+1. This is a local-first
     * recovery: make multiple eligible peers immediately due for
     * getheaders from the active tip, rotate a poor peer on repeated
     * attempts, and leave mirror repair gated until local retries have
     * been exercised. */
    if (cm && ms) {
        int peer_max = connman_max_peer_height(cm);
        int tip_h = -1;
        int next_h = -1;
        bool missing = false;

        zcl_mutex_lock(&ms->cs_main);
        struct block_index *tip = active_chain_tip(&ms->chain_active);
        if (tip) {
            tip_h = tip->nHeight;
            next_h = tip_h + 1;
            missing = (peer_max >= next_h &&
                       !active_next_child_exists(ms, tip, next_h));
        }
        zcl_mutex_unlock(&ms->cs_main);

        if (missing) {
            int peers = local_header_refill_from_peers(
                cm, next_h, "next-child-missing");
            struct cac_decision decision;
            bool proceed =
                chain_advance_coordinator_local_header_refill_needed(
                    tip_h,
                    next_h,
                    peer_max,
                    peers,
                    g_local_recovery.retry_count,
                    g_local_recovery.retries_exhausted,
                    &decision);
            printf("[watchdog] LOCAL_HEADER_REFILL: missing h=%d after "
                   "tip=%d peers_ahead=%d eligible=%d retry=%d "
                   "coordinator=%s reason=%s\n",
                   next_h, tip_h, peer_max, peers,
                   g_local_recovery.retry_count,
                   proceed ? "proceed" : "blocked",
                   decision.reason);
            event_emitf(EV_SYNC_STATE_CHANGE, 0,
                        "watchdog LOCAL_HEADER_REFILL h=%d peer_max=%d "
                        "eligible=%d retry=%d decision=%s reason=%s",
                        next_h, peer_max, peers,
                        g_local_recovery.retry_count,
                        cac_decision_result_name(decision.result),
                        decision.reason);
            if (!proceed)
                return WATCHDOG_NONE;
            if (!sync_set_state(SYNC_HEADERS_DOWNLOAD,
                                "watchdog local header refill")) {
                sync_set_state(SYNC_IDLE,
                               "watchdog local header refill via idle");
                sync_set_state(SYNC_HEADERS_DOWNLOAD,
                               "watchdog local header refill");
            }
            watchdog_kick_local_sync("next-child-missing");
            record_recovery(now, WATCHDOG_LOCAL_HEADER_REFILL);
            return WATCHDOG_LOCAL_HEADER_REFILL;
        } else if (g_local_recovery.active &&
                   g_local_recovery.missing_height == next_h) {
            local_recovery_reset();
        }
    }

    /* Progress rate tracking: record {height, timestamp} each cycle */
    {
        int current_height = -1;
        if (ms)
            current_height = active_chain_height(&ms->chain_active);

        if (current_height >= 0) {
            int idx = g_watchdog.progress_index % 5;
            g_watchdog.progress[idx].height = current_height;
            g_watchdog.progress[idx].timestamp = now;
            g_watchdog.progress_index++;
            if (g_watchdog.progress_count < 5)
                g_watchdog.progress_count++;

            /* Compute blocks/sec over the window */
            if (g_watchdog.progress_count >= 2) {
                int oldest = (g_watchdog.progress_index - g_watchdog.progress_count) % 5;
                if (oldest < 0) oldest += 5;
                int newest = (g_watchdog.progress_index - 1) % 5;
                int64_t dt = g_watchdog.progress[newest].timestamp -
                             g_watchdog.progress[oldest].timestamp;
                int dh = g_watchdog.progress[newest].height -
                         g_watchdog.progress[oldest].height;
                if (dt > 0)
                    g_watchdog.blocks_per_sec = (double)dh / (double)dt;
                else
                    g_watchdog.blocks_per_sec = 0.0;

                /* SLOW_PROGRESS warning during IBD with peers */
                if (g_watchdog.blocks_per_sec < 0.5 &&
                    state != SYNC_AT_TIP &&
                    state != SYNC_IDLE &&
                    connman_get_node_count(cm) > 0) {
                    printf("[watchdog] SLOW_PROGRESS: %.2f blocks/sec "
                           "(height %d)\n",
                           g_watchdog.blocks_per_sec, current_height);
                }
            }
        }
    }

    /* d. Escalation: instead of giving up, escalate recovery.
     *
     * Gate escalation on actual progress. If blocks are being
     * connected (blocks_per_sec >= 1.0), the chain IS advancing —
     * no escalation is warranted regardless of L1/L2 failure-count
     * accumulation from transient hiccups. Each L3 fires
     * `disconnect_outbound_peers` + `sync_set_state(SYNC_IDLE)`,
     * triggering a full re-bootstrap that costs minutes. */
    {
        int level = check_escalation_level(now);
        bool chain_advancing = (g_watchdog.blocks_per_sec >= 1.0);
        if (level >= 3 && g_watchdog.escalation_level < 3 && !chain_advancing) {
            escalation_l3(cm, dm);
            record_recovery(now, WATCHDOG_REPEATED_RESTART);
            return WATCHDOG_REPEATED_RESTART;
        }
        if (level >= 2 && g_watchdog.escalation_level < 2 && !chain_advancing) {
            escalation_l2(cm, dm);
            record_recovery(now, WATCHDOG_REPEATED_RESTART);
            return WATCHDOG_REPEATED_RESTART;
        }
        /* If we WERE escalated but the chain has resumed advancing,
         * decay the level back so the next recovery has a clean slate. */
        if (chain_advancing && g_watchdog.escalation_level > 0) {
            g_watchdog.escalation_level = 0;
            g_watchdog.l1_failures = 0;
            g_watchdog.l2_failures = 0;
        }
    }

    /* a. HEADER_STALL: headers_download >300s with no header progress */
    if (state == SYNC_HEADERS_DOWNLOAD && duration > HEADER_STALL_SECS) {
        int current_header_height = -1;
        if (ms && ms->pindex_best_header)
            current_header_height = ms->pindex_best_header->nHeight;

        /* Only trigger if header height hasn't advanced since last check */
        if (current_header_height >= 0 &&
            current_header_height <= g_watchdog.last_header_height) {
            int ndisconnected = disconnect_outbound_peers(cm);
            sync_set_state(SYNC_FINDING_PEERS, "watchdog HEADER_STALL recovery");
            printf("[watchdog] HEADER_STALL recovery: disconnected %d peers, "
                   "resetting sync\n", ndisconnected);
            record_recovery_detail(now, WATCHDOG_HEADER_STALL,
                                   g_last_header_reject_reason[0]
                                       ? g_last_header_reject_reason
                                       : "header_height_not_advancing",
                                   ms ? active_chain_height(&ms->chain_active)
                                      : -1,
                                   current_header_height,
                                   ndisconnected);
            g_watchdog.last_header_height = -1;

            /* Escalation: if peer rotation hasn't fixed it after 2 cycles */
            g_watchdog.header_stall_consecutive++;
            if (g_watchdog.header_stall_consecutive >= 2 &&
                !g_watchdog.header_stall_escalated) {
                g_watchdog.header_stall_escalated = true;
                printf("[watchdog] ESCALATION: persistent header stall "
                       "(%d consecutive recoveries)\n",
                       g_watchdog.header_stall_consecutive);
                if (g_last_header_reject_reason[0]) {
                    printf("[watchdog] ESCALATION: last reject reason: %s\n",
                           g_last_header_reject_reason);
                    if (strstr(g_last_header_reject_reason, "equihash") ||
                        strstr(g_last_header_reject_reason, "solution-size"))
                        printf("[watchdog] ESCALATION: height corruption "
                               "detected, headers rejected with wrong-era "
                               "validation rules\n");
                }
            }

            return WATCHDOG_HEADER_STALL;
        }
        g_watchdog.last_header_height = current_header_height;
    } else if (state != SYNC_HEADERS_DOWNLOAD) {
        /* Reset tracking when not in header download */
        g_watchdog.last_header_height = -1;
        g_watchdog.header_stall_consecutive = 0;
        g_watchdog.header_stall_escalated = false;
    }

    /* a2. HEADER_LAG: headers far behind peers AND not advancing.
     *
     * Only meaningful when headers are STUCK relative to peers — not
     * when we're just far behind doing a genesis-up sync. The
     * header_stall path above (a) covers "headers not advancing for
     * >300 s" with a tip-comparison check. This branch adds the
     * additional condition that headers must ALSO be far behind
     * peers and ALSO stuck — i.e. the lag is sustained, not
     * progress-driven. Use the same last_header_height comparison
     * pattern as HEADER_STALL.
     *
     * Originally written to handle "all peers stale" in HEADERS_DOWNLOAD
     * — that case is now also covered by HEADER_STALL's existing peer
     * rotation, so this branch reduces to a redundant safety net that
     * only fires when both conditions hold. */
    if ((state == SYNC_BLOCKS_DOWNLOAD && duration > 60) ||
        (state == SYNC_HEADERS_DOWNLOAD && duration > 300)) {
        int current_header_height = -1;
        if (ms && ms->pindex_best_header)
            current_header_height = ms->pindex_best_header->nHeight;

        int max_peer_height = connman_max_peer_height(cm);

        /* Progress check: only fire if header tip has NOT advanced
         * since the last cycle. Headers advancing → genuine progress,
         * no recovery needed. */
        bool header_stuck = (current_header_height >= 0 &&
                             current_header_height <= g_watchdog.last_header_height);
        bool first_block_lag_sample =
            (state == SYNC_BLOCKS_DOWNLOAD &&
             g_watchdog.last_header_height < 0);

        if ((header_stuck || first_block_lag_sample) &&
            current_header_height >= 0 && max_peer_height >= 0 &&
            current_header_height < (max_peer_height - 500)) {
            printf("[watchdog] HEADER_LAG: headers at %d (no advance "
                   "since last cycle), peers at %d (gap %d), "
                   "transitioning to SYNC_HEADERS_DOWNLOAD\n",
                   current_header_height, max_peer_height,
                   max_peer_height - current_header_height);
            event_emitf(EV_SYNC_STATE_CHANGE, 0,
                        "watchdog HEADER_LAG: headers=%d peers=%d gap=%d",
                        current_header_height, max_peer_height,
                        max_peer_height - current_header_height);

            if (!sync_set_state(SYNC_HEADERS_DOWNLOAD,
                                "watchdog HEADER_LAG recovery")) {
                sync_set_state(SYNC_IDLE, "watchdog HEADER_LAG via idle");
                sync_set_state(SYNC_HEADERS_DOWNLOAD,
                               "watchdog HEADER_LAG recovery");
            }
            record_recovery(now, WATCHDOG_HEADER_LAG);
            return WATCHDOG_HEADER_LAG;
        }

        /* Headers ARE advancing — update the watermark so the next
         * cycle compares against current progress. This is the same
         * pattern HEADER_STALL uses below. */
        if (current_header_height >= 0 &&
            current_header_height > g_watchdog.last_header_height) {
            g_watchdog.last_header_height = current_header_height;
        }
    }

    /* moved to condition block_failed_mask_at_tip (PR-1, 2026-05-23) */

    /* QUEUE_STARVED — in-flight slots < 10% of IBD cap for
     * >120s while in BLOCKS_DOWNLOAD with peers connected. Means we
     * have peers but they're not feeding the pipeline; BLOCK_STALL
     * (5 min, zero-progress) fires later than we'd like. Recovery:
     * disconnect outbound peers that haven't delivered a block
     * recently so the outbound thread picks fresh ones.
     * Gated on connman_get_node_count(cm) > 0 to avoid colliding with
     * the PEER_FLOOR / STATE_STUCK paths when there are no peers. */
    if (state == SYNC_BLOCKS_DOWNLOAD && dm &&
        connman_get_node_count(cm) > 0) {
        uint64_t inflight = 0, queued = 0;
        dl_get_stats(dm, NULL, NULL, NULL, &inflight, &queued);
        size_t starved_threshold =
            DL_MAX_IN_FLIGHT_TOTAL_IBD / QUEUE_STARVED_RATIO_DEN;
        bool starved = (inflight < starved_threshold);
        if (starved) {
            if (g_queue_starved_first_seen == 0)
                g_queue_starved_first_seen = now;
            if (now - g_queue_starved_first_seen >
                    QUEUE_STARVED_TRIGGER_SECS) {
                printf("[watchdog] QUEUE_STARVED: in_flight=%llu queued=%llu "
                       "(threshold=%zu) for %llds — rotating slow peers + "
                       "kicking refill\n",
                       (unsigned long long)inflight,
                       (unsigned long long)queued,
                       starved_threshold,
                       (long long)(now - g_queue_starved_first_seen));
                event_emitf(EV_SYNC_STATE_CHANGE, 0,
                            "watchdog QUEUE_STARVED in_flight=%llu queued=%llu",
                            (unsigned long long)inflight,
                            (unsigned long long)queued);
                int dropped = disconnect_outbound_peers(cm);
                (void)dropped;
                watchdog_kick_local_sync("queue-starved");
                g_queue_starved_first_seen = now;  /* re-arm */
                record_recovery(now, WATCHDOG_QUEUE_STARVED);
                return WATCHDOG_QUEUE_STARVED;
            }
        } else {
            g_queue_starved_first_seen = 0;
        }
    } else {
        g_queue_starved_first_seen = 0;
    }

    /* e. STALE_TIP: at_tip but peers are far ahead */
    if (state == SYNC_AT_TIP && duration > 120) {
        int our_height = -1;
        if (ms)
            our_height = active_chain_height(&ms->chain_active);
        int max_peer = connman_max_peer_height(cm);
        if (max_peer > 0 && our_height >= 0 && max_peer > our_height + 144) {
            printf("[watchdog] STALE_TIP: at_tip h=%d but peers at %d "
                   "(gap %d), reverting to HEADERS_DOWNLOAD\n",
                   our_height, max_peer, max_peer - our_height);
            event_emitf(EV_SYNC_STATE_CHANGE, 0,
                        "watchdog STALE_TIP: h=%d peers=%d gap=%d",
                        our_height, max_peer, max_peer - our_height);
            if (!sync_set_state(SYNC_HEADERS_DOWNLOAD,
                                "watchdog STALE_TIP recovery")) {
                sync_set_state(SYNC_IDLE, "watchdog STALE_TIP via idle");
                sync_set_state(SYNC_HEADERS_DOWNLOAD,
                               "watchdog STALE_TIP recovery");
            }
            watchdog_kick_local_sync("stale-tip");
            record_recovery(now, WATCHDOG_STATE_STUCK);
            return WATCHDOG_STATE_STUCK;
        }
    }

    /* c. STATE_STUCK: any state (except at_tip) exceeded per-state timeout */
    if (state != SYNC_AT_TIP && duration > state_stuck_timeout(state)) {
        /* Long-operation suppression (WS-2a): if a long_op_scope is
         * actively ticking (snapshot import, bulk copy, wallet rescan)
         * then progress is happening elsewhere and STATE_STUCK would
         * trigger a counterproductive header re-sync. Skip this tick. */
        int64_t lo_age = 0;
        if (long_op_is_active(&lo_age) && lo_age < 60) {
            const char *lo_label = long_op_recent_label();
            printf("[watchdog] suppressing STATE_STUCK: long_op active "
                   "(label=%s age=%llds)\n",
                   lo_label ? lo_label : "(unknown)",
                   (long long)lo_age);
            return WATCHDOG_NONE;
        }

        /* Surface through node.log + event stream, not just the systemd
         * journal — operators grep node.log via zcl_node_log, and printf
         * goes to stdout (journal) only. Throttle to once per stall
         * episode (cleared on state change or block-connect). */
        int our_h_log = ms ? active_chain_height(&ms->chain_active) : -1;
        int peer_max_log = cm ? connman_max_peer_height(cm) : -1;
        int peer_count_log = cm ? (int)connman_outbound_healthy_count(cm) : 0;
        if (!atomic_exchange(&g_stall_event_emitted, 1)) {
            fprintf(stderr, // obs-ok:paired-watchdog-event
                    "[watchdog] %s:%d %s(): STATE_STUCK state=%s "
                    "duration_s=%lld timeout_s=%lld our_h=%d peer_max=%d "
                    "peers=%d -- forcing header re-sync\n",
                    __FILE__, __LINE__, __func__, sync_state_name(state),
                    (long long)duration,
                    (long long)state_stuck_timeout(state),
                    our_h_log, peer_max_log, peer_count_log);
            event_emitf(EV_TIP_STALE, 0,
                        "state=%s since=%lld peers=%d max_peer=%d our_h=%d",
                        sync_state_name(state), (long long)duration,
                        peer_count_log, peer_max_log, our_h_log);
        }
        printf("[watchdog] STATE_STUCK: %s for %llds (timeout %llds), "
               "forcing header re-sync\n",
               sync_state_name(state), (long long)duration,
               (long long)state_stuck_timeout(state));

        /* Try direct transition; if not allowed, go through IDLE first */
        if (!sync_set_state(SYNC_HEADERS_DOWNLOAD,
                            "watchdog STATE_STUCK recovery")) {
            sync_set_state(SYNC_IDLE, "watchdog STATE_STUCK via idle");
            sync_set_state(SYNC_HEADERS_DOWNLOAD,
                           "watchdog STATE_STUCK recovery");
        }
        watchdog_kick_local_sync("state-stuck");
        record_recovery_detail(now, WATCHDOG_STATE_STUCK,
                               sync_state_name(state),
                               our_h_log,
                               peer_max_log,
                               peer_count_log);
        return WATCHDOG_STATE_STUCK;
    }

    return WATCHDOG_NONE;
}

/* ── Periodic watchdog tick (Move 3 — unified heartbeat) ─────────
 *
 * Runs sync_watchdog_check on a fixed 30s cadence via the lib/health
 * sweeper, decoupled from the message-processing loop. */

#define WATCHDOG_TICK_SECS 30

struct watchdog_periodic_args {
    struct connman          *cm;
    struct download_manager *dm;
    struct main_state       *ms;
};

static struct watchdog_periodic_args g_watchdog_args;
static health_subsystem_id g_watchdog_health_id = HEALTH_INVALID_ID;

/* Supervisor liveness contract (Round 5).
 *
 * The lib/health sweeper above is the *primary* driver. The supervisor
 * is a redundant, independent driver: if the sweeper thread wedges
 * (which it did silently on 2026-05-21 for 8.6 h), the supervisor
 * keeps calling the same tick from its own thread, so the watchdog's
 * stall-detection logic still runs.
 *
 * sync_watchdog_check() is mutex-protected and edge-idempotent on
 * state_stuck_since, so double-firing (sweeper + supervisor both
 * calling it) is safe — at worst we get two acquire-mutex / release
 * pairs per 30-second window. */
static struct liveness_contract g_wd_contract;
static supervisor_child_id      g_wd_supervisor_id = SUPERVISOR_INVALID_ID;

/* Forward decl: the supervisor callbacks below dispatch into the
 * periodic tick, which is defined further down. */
static void sync_watchdog_periodic_tick(void *arg);

static void sync_watchdog_supervisor_tick(struct liveness_contract *c)
{
    (void)c;
    sync_watchdog_periodic_tick(&g_watchdog_args);
}

static void sync_watchdog_supervisor_stall(struct liveness_contract *c)
{
    /* Supervisor noticed the watchdog hasn't ticked within its deadline
     * (120 s). That means the lib/health sweeper is stuck. Force-run
     * the tick on the supervisor's own thread so stall detection keeps
     * working even when the sweeper is dead. The supervisor's edge-
     * once semantics mean this fires exactly once per stall episode;
     * a successful tick (which calls supervisor_tick implicitly via
     * the contract's on_tick path on the next sweep) rearms the edge. */
    fprintf(stderr,  // obs-ok:supervisor-rescue-precedes-tick
        "[supervisor] sync.watchdog deadline missed (%lluus age) — "
        "force-running tick from supervisor thread\n",
        (unsigned long long)(c ? 0 : 0));
    fflush(stderr);
    sync_watchdog_periodic_tick(&g_watchdog_args);
}

static void sync_watchdog_periodic_tick(void *arg)
{
    struct watchdog_periodic_args *a = arg;
    if (!a) return;
    sync_watchdog_check(a->cm, a->dm, a->ms);

    /* Emit a structured heartbeat every other tick (60s). Absence of
     * EV_SYNC_HEARTBEAT for >120s implies the watchdog thread itself
     * wedged — a different failure shape than a sync stall and worth
     * distinguishing in monitoring. */
    static _Atomic int s_heartbeat_skip = 0;
    if ((atomic_fetch_add(&s_heartbeat_skip, 1) & 1) == 0) {
        enum sync_state state = sync_get_state();
        int our_h = a->ms ? active_chain_height(&a->ms->chain_active) : -1;
        int peer_max = a->cm ? connman_max_peer_height(a->cm) : -1;
        int64_t tip_age = sync_watchdog_get_tip_advance_age();
        event_emitf(EV_SYNC_HEARTBEAT, 0,
                    "state=%s h=%d max_peer=%d tip_age=%lld",
                    sync_state_name(state), our_h, peer_max,
                    (long long)tip_age);
    }
}

bool sync_watchdog_start(struct connman *cm,
                          struct download_manager *dm,
                          struct main_state *ms)
{
    if (g_watchdog_health_id != HEALTH_INVALID_ID)
        return false;  /* already started */

    g_watchdog_args.cm = cm;
    g_watchdog_args.dm = dm;
    g_watchdog_args.ms = ms;

    /* Lazy-start the heartbeat sweeper. Idempotent. */
    (void)health_start();

    g_watchdog_health_id = health_register_periodic(
        "sync.watchdog", WATCHDOG_TICK_SECS,
        sync_watchdog_periodic_tick, &g_watchdog_args);
    if (g_watchdog_health_id == HEALTH_INVALID_ID) {
        LOG_FAIL("watchdog", "health_register_periodic failed");
        return false;
    }

    /* Round 5: register a parallel supervisor contract so the watchdog
     * tick keeps running if the lib/health sweeper itself wedges. */
    if (g_wd_supervisor_id == SUPERVISOR_INVALID_ID) {
        liveness_contract_init(&g_wd_contract, "sync.watchdog");
        atomic_store(&g_wd_contract.period_secs,   WATCHDOG_TICK_SECS);
        atomic_store(&g_wd_contract.deadline_secs, WATCHDOG_TICK_SECS * 4);
        g_wd_contract.on_tick  = sync_watchdog_supervisor_tick;
        g_wd_contract.on_stall = sync_watchdog_supervisor_stall;
        supervisor_domains_init();
        g_wd_supervisor_id =
            supervisor_register_in_domain(g_chain_sup, &g_wd_contract);
        /* Registration failure is non-fatal — the lib/health sweeper
         * is still active. Log + continue. */
        if (g_wd_supervisor_id == SUPERVISOR_INVALID_ID) {
            fprintf(stderr,  // obs-ok:supervisor-register-fallback-warn
                "[watchdog] WARN supervisor_register failed; relying on "
                "lib/health sweeper alone\n");
            fflush(stderr);
        }
    }
    return true;
}

void sync_watchdog_stop(void)
{
    if (g_wd_supervisor_id != SUPERVISOR_INVALID_ID) {
        supervisor_unregister(g_wd_supervisor_id);
        g_wd_supervisor_id = SUPERVISOR_INVALID_ID;
    }
    if (g_watchdog_health_id == HEALTH_INVALID_ID) return;
    health_unregister(g_watchdog_health_id);
    g_watchdog_health_id = HEALTH_INVALID_ID;
}
