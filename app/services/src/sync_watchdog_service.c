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
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdatomic.h>

#include "util/log_macros.h"

/* ── Thresholds ──────────────────────────────────────────── */

#define HEADER_STALL_SECS    300   /* 5 minutes */
#define BLOCK_STALL_SECS     300   /* 5 minutes */
#define STATE_STUCK_SECS     600   /* 10 minutes */
#define REPEATED_WINDOW_SECS 1800  /* 30 minutes */
#define REPEATED_MAX         3     /* max recoveries before giving up */

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
}

/* ── Circuit breaker check ───────────────────────────────── */

static bool check_repeated_restart(int64_t now)
{
    /* Count recoveries within the window */
    int recent = 0;
    for (int i = 0; i < g_watchdog.recovery_count; i++) {
        if (now - g_watchdog.recovery_times[i] < REPEATED_WINDOW_SECS)
            recent++;
    }
    return (recent >= REPEATED_MAX);
}

static void record_recovery(int64_t now, enum watchdog_recovery_type type)
{
    g_watchdog.recoveries_triggered++;
    g_watchdog.last_recovery_time = now;
    g_watchdog.last_recovery_type = type;

    /* Shift old timestamps if full */
    if (g_watchdog.recovery_count >= REPEATED_MAX + 1) {
        memmove(&g_watchdog.recovery_times[0],
                &g_watchdog.recovery_times[1],
                sizeof(int64_t) * REPEATED_MAX);
        g_watchdog.recovery_count = REPEATED_MAX;
    }
    g_watchdog.recovery_times[g_watchdog.recovery_count++] = now;
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

    /* d. REPEATED_RESTART circuit breaker */
    if (check_repeated_restart(now)) {
        /* Already logged — don't spam */
        if (g_watchdog.last_recovery_type != WATCHDOG_REPEATED_RESTART) {
            printf("[watchdog] REPEATED failures — manual intervention needed\n");
            g_watchdog.last_recovery_type = WATCHDOG_REPEATED_RESTART;
        }
        return WATCHDOG_NONE;
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

    /* c. STATE_STUCK: any state (except at_tip) unchanged >600s */
    if (state != SYNC_AT_TIP && duration > STATE_STUCK_SECS) {
        printf("[watchdog] STATE_STUCK: %s for %llds, "
               "forcing header re-sync\n",
               sync_state_name(state), (long long)duration);

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
