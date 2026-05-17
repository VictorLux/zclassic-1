/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Sync Watchdog Service — automatic stall detection and recovery.
 *
 * Runs from the message processing loop (~every 30s). Detects:
 *   HEADER_STALL:  headers_download for >300s with no header progress
 *   BLOCK_STALL:   blocks_download for >300s with no chain height progress
 *   STATE_STUCK:   any sync state (except at_tip) unchanged for >600s
 *   REPEATED_RESTART: circuit breaker after >3 recoveries in 30 minutes */

#include "services/sync_watchdog_service.h"
/* Wave 9b: was controllers/network_controller.h — sync_watchdog uses connman_* only (no rpc_net_*), so the controller dep was vestigial. */
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
#include "util/thread_registry.h"
#include "health/heartbeat.h"

/* ── Thresholds ──────────────────────────────────────────── */

#define HEADER_STALL_SECS    300   /* 5 minutes */
#define BLOCK_STALL_SECS     300   /* 5 minutes */
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

/* Wave 8: tip-advance tracking — last successful block connect timestamp.
 * Updated by sync_watchdog_on_block_connected() from the EV_BLOCK_CONNECTED
 * emit site in lib/net/src/msg_blocks.c. Read by
 * sync_watchdog_get_tip_advance_age() and exposed via zcl_status JSON +
 * Prometheus zcl_tip_advance_age_seconds. Independent of state-entered
 * time because state changes (e.g. AT_TIP→HEADERS_DOWNLOAD on a recovery
 * toggle) should NOT zero the tip-advance age. */
_Atomic int64_t g_last_block_connected_ts = 0;
_Atomic int     g_last_block_connected_height = 0;

/* Wave 8: once-per-stall-episode emit throttle for EV_TIP_STALE.
 * Set when the STATE_STUCK path emits; cleared on any state change OR
 * any block connect (sync_watchdog_on_state_change /
 * sync_watchdog_on_block_connected). Atomic so the watchdog tick thread
 * and the message-processing thread don't race. */
_Atomic int     g_stall_event_emitted = 0;

void sync_watchdog_on_state_change(enum sync_state new_state, int height)
{
    (void)new_state;
    atomic_store(&g_sync_state_entered_time, (int64_t)time(NULL));
    atomic_store(&g_sync_state_entry_height, height);
    /* State changed → if a stall just ended, allow a fresh EV_TIP_STALE
     * the next time we get stuck. Don't clear g_last_block_connected_ts:
     * forced state toggles (the STATE_STUCK recovery path) shouldn't
     * mask real tip staleness. */
    atomic_store(&g_stall_event_emitted, 0);
}

void sync_watchdog_on_block_connected(int height)
{
    atomic_store(&g_last_block_connected_ts, (int64_t)time(NULL));
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
    int64_t now = (int64_t)time(NULL);
    return (now > last) ? (now - last) : 0;
}

int64_t sync_get_state_duration(void)
{
    int64_t entered = atomic_load(&g_sync_state_entered_time);
    if (entered == 0)
        return 0;
    int64_t now = (int64_t)time(NULL);
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
} g_watchdog;

/* Last header reject reason (updated by msg_headers.c via setter) */
static char g_last_header_reject_reason[256] = {0};

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
    atomic_store(&g_sync_state_entered_time, (int64_t)time(NULL));
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
    case WATCHDOG_UTXO_PAUSE:       return "UTXO_PAUSE";
    case WATCHDOG_QUEUE_STARVED:    return "QUEUE_STARVED";
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
    json_push_kv_real(out, "blocks_per_sec", st.blocks_per_sec);
    json_push_kv_int(out, "utxo_paused_height",
                     (int64_t)process_block_get_utxo_activation_paused_height());
    return true;
}

/* Forward declarations */
static int disconnect_outbound_peers(struct connman *cm);

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

static void record_recovery(int64_t now, enum watchdog_recovery_type type)
{
    g_watchdog.recoveries_triggered++;
    g_watchdog.last_recovery_time = now;
    g_watchdog.last_recovery_type = type;

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
/* non-static so tests can backdate via extern.
 * Follows the pattern of g_sync_state_entered_time. */
int64_t g_utxo_pause_first_seen = 0;
int64_t g_queue_starved_first_seen = 0;   /* Round 7 A7 */

#define PEER_FLOOR_MIN_HEALTHY    3
#define PEER_FLOOR_TRIGGER_SECS  60
#define SYNC_VIOLATION_GAP      100   /* blocks behind peer max */
#define SYNC_VIOLATION_SECS     600   /* sustained for 10 min */
#define UTXO_PAUSE_TRIGGER_SECS 300   /* clear after 5 min */
#define QUEUE_STARVED_TRIGGER_SECS 120 /* starved for >2 min */
#define QUEUE_STARVED_RATIO_DEN    10  /* < 1/10th of IBD in-flight limit */

enum watchdog_recovery_type sync_watchdog_check(
    struct connman *cm,
    struct download_manager *dm,
    struct main_state *ms)
{
    if (!g_watchdog.initialized)
        return WATCHDOG_NONE;

    g_watchdog.checks_run++;

    int64_t now = (int64_t)time(NULL);
    enum sync_state state = sync_get_state();
    int64_t duration = sync_get_state_duration();

    /* ── Part C: PEER_FLOOR — < 3 healthy outbound for > 60s ──
     * Even with peers in the table, if none are past handshake we
     * can't sync. Forces a fresh seed walk + drops any inbound peers
     * blocking outbound slots. */
    {
        size_t healthy = connman_outbound_healthy_count(cm);
        if (healthy < PEER_FLOOR_MIN_HEALTHY) {
            if (g_peer_floor_first_violation == 0)
                g_peer_floor_first_violation = now;
            if (now - g_peer_floor_first_violation > PEER_FLOOR_TRIGGER_SECS) {
                printf("[watchdog] PEER_FLOOR: only %zu/%d healthy "
                       "outbound — forcing seed rewalk\n",
                       healthy, PEER_FLOOR_MIN_HEALTHY);
                event_emitf(EV_SYNC_STATE_CHANGE, 0,
                            "watchdog PEER_FLOOR healthy=%zu", healthy);
                /* Disconnect outbound peers stuck below handshake
                 * (PEER_CONNECTING/PEER_CONNECTED) to free slots for
                 * the outbound thread's aggressive backfill. */
                if (cm) {
                    zcl_mutex_lock(&cm->manager.cs_nodes);
                    for (size_t i = 0; i < cm->manager.num_nodes; i++) {
                        struct p2p_node *n = cm->manager.nodes[i];
                        if (n && !n->inbound && !n->disconnect &&
                            n->state < PEER_HANDSHAKE_COMPLETE)
                            n->disconnect = true;
                    }
                    zcl_mutex_unlock(&cm->manager.cs_nodes);
                }
                g_peer_floor_first_violation = now;  /* re-arm */
                record_recovery(now, WATCHDOG_PEER_FLOOR);
                return WATCHDOG_PEER_FLOOR;
            }
        } else {
            g_peer_floor_first_violation = 0;
        }
    }

    /* ── UTXO_PAUSE — activation paused > 300s ──
     *
     * process_block_note_utxo_failure() pauses activate_best_chain at
     * a specific height when reimport has already been attempted and
     * did NOT heal the chain (lib/validation/src/process_block.c:504).
     * Pre-Round-7 this state was silent to the watchdog: sync state
     * stayed BLOCKS_DOWNLOAD, no height progress event, BLOCK_STALL
     * eventually fired but its recovery (re-queue download manager)
     * didn't address the root cause. Now: detect the pause, clear it
     * after 300s, let activate_best_chain re-try. If it re-pauses
     * within the 30min escalation window, L1→L2→L3 picks up. */
    {
        int paused = process_block_get_utxo_activation_paused_height();
        if (paused >= 0) {
            if (g_utxo_pause_first_seen == 0)
                g_utxo_pause_first_seen = now;
            if (now - g_utxo_pause_first_seen > UTXO_PAUSE_TRIGGER_SECS) {
                printf("[watchdog] UTXO_PAUSE: activation paused at h=%d "
                       "for %llds — clearing pause and re-arming\n",
                       paused,
                       (long long)(now - g_utxo_pause_first_seen));
                event_emitf(EV_SYNC_STATE_CHANGE, 0,
                            "watchdog UTXO_PAUSE h=%d", paused);
                process_block_clear_utxo_activation_pause_range(
                    paused, paused);
                g_utxo_pause_first_seen = now;  /* re-arm */
                record_recovery(now, WATCHDOG_UTXO_PAUSE);
                return WATCHDOG_UTXO_PAUSE;
            }
        } else {
            g_utxo_pause_first_seen = 0;
        }
    }

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
                g_sync_violation_first_seen = now;  /* re-arm */
                record_recovery(now, WATCHDOG_SYNC_VIOLATION);
                return WATCHDOG_SYNC_VIOLATION;
            }
        } else {
            g_sync_violation_first_seen = 0;
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
     * Wave 9q: gate escalation on actual progress. If blocks are
     * being connected (blocks_per_sec >= 1.0), the chain IS
     * advancing — no escalation is warranted regardless of L1/L2
     * failure-count accumulation from transient hiccups. Without
     * this gate, the live node climbed cleanly to h=9491 (~50 bps
     * sustained) then L3 fired anyway, wiping in-memory state and
     * resetting the active chain to h=897. Each L3 fires
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
            record_recovery(now, WATCHDOG_HEADER_STALL);
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
     * Wave 9l: this branch is only meaningful when headers are STUCK
     * relative to peers — not when we're just far behind doing a
     * genesis-up sync. Pre-9l the check fired purely on gap > 500,
     * which for a 3 M-block-behind cold sync stays true for HOURS,
     * triggering L1/L2/L3 escalation every cycle and resetting the
     * sync state machine before any block can connect.
     *
     * The header_stall path above (a) already covers "headers not
     * advancing for >300 s" with a tip-comparison check. This branch
     * adds the additional condition that headers must ALSO be far
     * behind peers and ALSO stuck — i.e. the lag is sustained, not
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

        if (header_stuck &&
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

    /* b. BLOCK_STALL: blocks_download >300s with no chain height progress */
    if (state == SYNC_BLOCKS_DOWNLOAD && duration > BLOCK_STALL_SECS) {
        int current_height = -1;
        if (ms)
            current_height = active_chain_height(&ms->chain_active);

        if (current_height >= 0 &&
            current_height <= g_watchdog.last_chain_height) {
            /* Reset download manager: clear in-flight, re-queue */
            if (dm) {
                zcl_mutex_lock(&dm->cs);
                /* Clear all in-flight entries */
                for (size_t i = 0; i < dm->num_slots; i++) {
                    if (dm->slots[i].active) {
                        /* Re-queue this block */
                        if (dm->queue_len < dm->queue_cap) {
                            dm->queue[dm->queue_len] = dm->slots[i].hash;
                            dm->queue_heights[dm->queue_len] = dm->slots[i].height;
                            dm->queue_len++;
                        }
                        dm->slots[i].active = false;
                        dm->num_active--;
                    }
                }
                zcl_mutex_unlock(&dm->cs);
            }

            /* After re-queuing timed-out blocks, check if queue is
             * still empty — if so, force-populate from block index */
            uint64_t post_queued = 0, post_inflight = 0;
            dl_get_stats(dm, NULL, NULL, NULL, &post_inflight, &post_queued);
            if (post_queued == 0 && post_inflight == 0 && ms) {
                int chain_h = active_chain_height(&ms->chain_active);
                struct uint256 scan_hashes[256];
                int32_t scan_heights[256];
                size_t scan_count = 0;
                size_t iter = 0;
                struct block_index *bi;
                while (block_map_next(&ms->map_block_index, &iter,
                                      NULL, &bi)) {
                    if (!bi || scan_count >= 256) break;
                    if (bi->nHeight <= chain_h) continue;
                    if (bi->nHeight > chain_h + 2048) continue;
                    if (bi->nStatus & BLOCK_HAVE_DATA) continue;
                    if (bi->nStatus & BLOCK_FAILED_MASK) continue;
                    if (!bi->phashBlock) continue;
                    scan_hashes[scan_count] = *bi->phashBlock;
                    scan_heights[scan_count] = bi->nHeight;
                    scan_count++;
                }
                if (scan_count > 0) {
                    dl_queue_blocks(dm, scan_hashes, scan_heights,
                                    scan_count);
                    printf("[watchdog] BLOCK_STALL: force-queued %zu "
                           "blocks from index\n", scan_count);
                } else {
                    printf("[watchdog] BLOCK_STALL: no downloadable "
                           "blocks, reverting to HEADERS_DOWNLOAD\n");
                    sync_set_state(SYNC_HEADERS_DOWNLOAD,
                                   "watchdog BLOCK_STALL: no blocks");
                }
            }

            printf("[watchdog] BLOCK_STALL recovery: reset download manager, "
                   "re-queued blocks\n");
            record_recovery(now, WATCHDOG_BLOCK_STALL);
            g_watchdog.last_chain_height = -1;
            return WATCHDOG_BLOCK_STALL;
        }
        g_watchdog.last_chain_height = current_height;
    } else if (state != SYNC_BLOCKS_DOWNLOAD) {
        g_watchdog.last_chain_height = -1;
    }

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
            record_recovery(now, WATCHDOG_STATE_STUCK);
            return WATCHDOG_STATE_STUCK;
        }
    }

    /* c. STATE_STUCK: any state (except at_tip) exceeded per-state timeout */
    if (state != SYNC_AT_TIP && duration > state_stuck_timeout(state)) {
        /* Wave 8: surface this through node.log + event stream, not just
         * the systemd journal. The 2026-05-15 25-hour stall went unseen
         * because printf goes to stdout (journal) while operators grep
         * node.log via zcl_node_log. Throttle to once per stall episode
         * (cleared on state change or block-connect). */
        int our_h_log = ms ? active_chain_height(&ms->chain_active) : -1;
        int peer_max_log = cm ? connman_max_peer_height(cm) : -1;
        int peer_count_log = cm ? (int)connman_outbound_healthy_count(cm) : 0;
        if (!atomic_exchange(&g_stall_event_emitted, 1)) {
            LOG_FAIL("watchdog",
                     "STATE_STUCK state=%s duration_s=%lld timeout_s=%lld "
                     "our_h=%d peer_max=%d peers=%d — forcing header re-sync",
                     sync_state_name(state), (long long)duration,
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
        record_recovery(now, WATCHDOG_STATE_STUCK);
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

static void sync_watchdog_periodic_tick(void *arg)
{
    struct watchdog_periodic_args *a = arg;
    if (!a) return;
    sync_watchdog_check(a->cm, a->dm, a->ms);

    /* Wave 8: emit a structured heartbeat every other tick (60s).
     * Absence of EV_SYNC_HEARTBEAT for >120s implies the watchdog
     * thread itself wedged — a different failure shape than a sync
     * stall and worth distinguishing in monitoring. */
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
    return true;
}

void sync_watchdog_stop(void)
{
    if (g_watchdog_health_id == HEALTH_INVALID_ID) return;
    health_unregister(g_watchdog_health_id);
    g_watchdog_health_id = HEALTH_INVALID_ID;
}
