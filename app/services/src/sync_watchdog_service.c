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
#include "controllers/network_controller.h"
#include "validation/chainstate.h"
#include "net/download.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdatomic.h>

#include "util/log_macros.h"

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

void sync_watchdog_on_state_change(enum sync_state new_state, int height)
{
    (void)new_state;
    atomic_store(&g_sync_state_entered_time, (int64_t)time(NULL));
    atomic_store(&g_sync_state_entry_height, height);
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

#define PEER_FLOOR_MIN_HEALTHY    3
#define PEER_FLOOR_TRIGGER_SECS  60
#define SYNC_VIOLATION_GAP      100   /* blocks behind peer max */
#define SYNC_VIOLATION_SECS     600   /* sustained for 10 min */

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

    /* d. Escalation: instead of giving up, escalate recovery */
    {
        int level = check_escalation_level(now);
        if (level >= 3 && g_watchdog.escalation_level < 3) {
            escalation_l3(cm, dm);
            record_recovery(now, WATCHDOG_REPEATED_RESTART);
            return WATCHDOG_REPEATED_RESTART;
        }
        if (level >= 2 && g_watchdog.escalation_level < 2) {
            escalation_l2(cm, dm);
            record_recovery(now, WATCHDOG_REPEATED_RESTART);
            return WATCHDOG_REPEATED_RESTART;
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

    /* a2. HEADER_LAG: in blocks_download but headers are far behind peers */
    if (state == SYNC_BLOCKS_DOWNLOAD && duration > 60) {
        int current_header_height = -1;
        if (ms && ms->pindex_best_header)
            current_header_height = ms->pindex_best_header->nHeight;

        int max_peer_height = connman_max_peer_height(cm);

        if (current_header_height >= 0 && max_peer_height >= 0 &&
            current_header_height < (max_peer_height - 500)) {
            printf("[watchdog] HEADER_LAG: headers at %d, peers at %d "
                   "(gap %d), transitioning to SYNC_HEADERS_DOWNLOAD\n",
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

/* ── Independent watchdog thread ─────────────────────────────────
 *
 * Runs sync_watchdog_check on a fixed 30s cadence. Decoupled from the
 * message-processing loop so peer-id churn, message starvation, or a
 * stuck peer can never prevent the watchdog from firing. */

#define WATCHDOG_TICK_SECS 30

struct watchdog_thread_args {
    _Atomic bool *running;
    struct connman *cm;
    struct download_manager *dm;
    struct main_state *ms;
};

static struct watchdog_thread_args g_watchdog_args;

static void *sync_watchdog_thread_entry(void *arg)
{
    struct watchdog_thread_args *a = arg;
    if (!a || !a->running)
        return NULL;

    /* Sleep in 1-second slices so shutdown is responsive. */
    int slept = 0;
    while (atomic_load(a->running)) {
        if (slept >= WATCHDOG_TICK_SECS) {
            sync_watchdog_check(a->cm, a->dm, a->ms);
            slept = 0;
        }
        struct timespec ts = { .tv_sec = 1, .tv_nsec = 0 };
        nanosleep(&ts, NULL);
        slept++;
    }
    return NULL;
}

bool sync_watchdog_thread_start(pthread_t *thread,
                                bool *started,
                                _Atomic bool *running,
                                struct connman *cm,
                                struct download_manager *dm,
                                struct main_state *ms)
{
    if (!thread || !started || !running)
        LOG_FAIL("watchdog", "thread_start: NULL argument "
                 "(thread=%p, started=%p, running=%p)",
                 (void *)thread, (void *)started, (void *)running);
    if (*started)
        return false;
    g_watchdog_args.running = running;
    g_watchdog_args.cm = cm;
    g_watchdog_args.dm = dm;
    g_watchdog_args.ms = ms;
    if (pthread_create(thread, NULL, sync_watchdog_thread_entry,
                       &g_watchdog_args) != 0) {
        LOG_FAIL("watchdog", "pthread_create failed");
        return false;
    }
    *started = true;
    return true;
}

void sync_watchdog_thread_stop(pthread_t *thread, bool *started)
{
    if (!thread || !started || !*started)
        return;
    pthread_join(*thread, NULL);
    *started = false;
}
