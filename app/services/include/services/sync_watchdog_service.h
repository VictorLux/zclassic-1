/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Sync Watchdog Service — detects and recovers from sync stalls.
 *
 * The watchdog monitors sync state progression and triggers automatic
 * recovery when the node gets stuck:
 *   - HEADER_STALL:  headers not advancing for 300s
 *   - BLOCK_STALL:   chain height not advancing for 300s
 *   - STATE_STUCK:   any non-tip state unchanged for 600s
 *   - REPEATED_RESTART: circuit breaker after 3 recoveries in 30min */

#ifndef ZCL_SERVICES_SYNC_WATCHDOG_H
#define ZCL_SERVICES_SYNC_WATCHDOG_H

#include "event/event.h"
#include "net/connman.h"
#include "net/download.h"
#include "validation/main_state.h"
#include <stdbool.h>
#include <stdint.h>

/* Recovery type identifiers */
enum watchdog_recovery_type {
    WATCHDOG_NONE = 0,
    WATCHDOG_HEADER_STALL,
    WATCHDOG_HEADER_LAG,
    WATCHDOG_BLOCK_STALL,
    WATCHDOG_STATE_STUCK,
    WATCHDOG_REPEATED_RESTART,
    WATCHDOG_PEER_FLOOR,    /* < 3 healthy outbound for > 60s */
    WATCHDOG_SYNC_VIOLATION,/* peer_max - tip > 100 for > 600s (Part D) */
};

/* Watchdog status snapshot (for RPC) */
struct sync_watchdog_status {
    bool     enabled;
    uint64_t checks_run;
    uint64_t recoveries_triggered;
    int64_t  last_recovery_time;
    enum watchdog_recovery_type last_recovery_type;
    enum sync_state current_state;
    int64_t  current_state_duration_secs;
    int      current_state_entry_height;
    int      escalation_level;
};

/* Extended watchdog stats (for MCP health) */
struct watchdog_stats {
    int      checks_run;
    int      recoveries_total;
    int      escalation_level;
    double   blocks_per_sec;
    int64_t  last_recovery_time;
    enum watchdog_recovery_type last_recovery;
};

/* Get extended watchdog stats for MCP health endpoint. */
void sync_watchdog_get_stats(struct watchdog_stats *out);

/* Initialize watchdog state and register state-change callback.
 * Call once at startup. */
void sync_watchdog_init(void);

/* Run one watchdog check cycle. Call from message processing loop (~every 30s).
 * Returns the recovery type triggered, or WATCHDOG_NONE. */
enum watchdog_recovery_type sync_watchdog_check(
    struct connman *cm,
    struct download_manager *dm,
    struct main_state *ms);

/* Get current watchdog status for RPC. */
void sync_watchdog_get_status(struct sync_watchdog_status *out);

/* Return human-readable name for a recovery type. */
const char *watchdog_recovery_type_name(enum watchdog_recovery_type type);

/* Get how long the current sync state has been active (seconds). */
int64_t sync_get_state_duration(void);

/* Get the height at which the current sync state was entered. */
int sync_get_state_entry_height(void);

/* Record the last header rejection reason for escalation diagnostics. */
void sync_watchdog_set_last_reject_reason(const char *reason);

/* Called internally when sync state changes to update timestamps.
 * Registered as callback via sync_set_state_change_callback(). */
void sync_watchdog_on_state_change(enum sync_state new_state, int height);

/* Independent watchdog tick thread.
 *
 * The watchdog must fire on a fixed cadence regardless of message-loop
 * activity. The previous design called sync_watchdog_check() from the
 * msg loop only on the peer with id==0, which silently disabled the
 * watchdog once peer ids rotated past zero (the live failure mode that
 * left a node 22k blocks behind for >9h with checks_run=1). The thread
 * here runs every 30s as long as `*running` is true. */
bool sync_watchdog_thread_start(pthread_t *thread,
                                bool *started,
                                _Atomic bool *running,
                                struct connman *cm,
                                struct download_manager *dm,
                                struct main_state *ms);

void sync_watchdog_thread_stop(pthread_t *thread, bool *started);

/* State-dump convention (see CLAUDE.md "Adding state introspection").
 * Writes the watchdog's runtime state as a JSON object into `out`.
 * `out` must already be initialized; this function fills it. `key` is
 * unused by this subsystem. Reentrant-safe via atomic loads. */
struct json_value;
bool sync_watchdog_dump_state_json(struct json_value *out, const char *key);

#endif /* ZCL_SERVICES_SYNC_WATCHDOG_H */
